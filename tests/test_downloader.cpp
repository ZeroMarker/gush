#include <gtest/gtest.h>
#include "downloader.h"
#include "utils.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>

namespace {

constexpr int kBlockSize = 16384;

std::string peerId20() {
    return "-GT0001-ABCDEFGHIJKL";  // exactly 20 bytes
}

uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool sendFull(int fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Write a wire message (4-byte length prefix + id + payload) possibly split
// into two chunks with a small delay, exercising partial-read handling.
bool sendMessageSplit(int fd, uint8_t id, const std::vector<uint8_t>& payload,
                      bool splitPayload) {
    std::vector<uint8_t> head(5);
    uint32_t len = static_cast<uint32_t>(1 + payload.size());
    head[0] = (len >> 24) & 0xFF;
    head[1] = (len >> 16) & 0xFF;
    head[2] = (len >> 8) & 0xFF;
    head[3] = len & 0xFF;
    head[4] = id;

    if (!sendFull(fd, head.data(), 5)) return false;
    if (!payload.empty()) {
        if (splitPayload && payload.size() > 3) {
            // Split payload into two halves to force partial-recv reassembly
            size_t half = payload.size() / 2;
            if (!sendFull(fd, payload.data(), half)) return false;
            usleep(5000);
            if (!sendFull(fd, payload.data() + half, payload.size() - half)) return false;
        } else {
            if (!sendFull(fd, payload.data(), payload.size())) return false;
        }
    }
    return true;
}

// Minimal local mock peer: answers the handshake, then serves unchoke,
// bitfield and block data for every request received.
class MockPeerServer {
public:
    MockPeerServer(const std::string& infoHash, const std::string& pieceData)
        : infoHash_(infoHash), pieceData_(pieceData) {}

    bool start() {
        lfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd_ < 0) return false;

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // OS-assigned port
        if (bind(lfd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (listen(lfd_, 1) != 0) return false;

        socklen_t len = sizeof(addr);
        if (getsockname(lfd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port_ = ntohs(addr.sin_port);

        thread_ = std::thread([this]() {
            int cfd = accept(lfd_, nullptr, nullptr);
            if (cfd >= 0) {
                serve(cfd);
                close(cfd);
            }
        });
        return true;
    }

    uint16_t port() const { return port_; }

    void join() {
        if (thread_.joinable()) thread_.join();
        if (lfd_ >= 0) close(lfd_);
    }

private:
    void serve(int fd) {
        // Read client handshake (68 bytes), possibly in fragments
        std::vector<uint8_t> hs(68, 0);
        size_t got = 0;
        while (got < 68) {
            ssize_t n = recv(fd, hs.data() + got, 68 - got, 0);
            if (n <= 0) return;
            got += static_cast<size_t>(n);
        }
        if (hs[0] != 19) return;

        // Reply with our own handshake (fragmented on purpose)
        std::vector<uint8_t> resp(68, 0);
        resp[0] = 19;
        memcpy(&resp[1], "BitTorrent protocol", 19);
        memcpy(&resp[28], infoHash_.data(), 20);
        memcpy(&resp[48], "-GT0001-MOCK-PEER-000", 20);
        if (!sendFull(fd, resp.data(), 34)) return;       // partial handshake
        usleep(3000);
        if (!sendFull(fd, resp.data() + 34, 34)) return;

        bool servedBitfield = false;
        while (true) {
            uint8_t lenBuf[4];
            if (recv(fd, lenBuf, 4, MSG_WAITALL) != 4) return;
            uint32_t len = readBE32(lenBuf);
            if (len == 0) continue;  // keep-alive

            uint8_t id = 0;
            if (recv(fd, &id, 1, MSG_WAITALL) != 1) return;

            if (id == 2) {  // interested
                // Unchoke (split) + bitfield: peer has piece 0
                if (!sendMessageSplit(fd, 1, {}, true)) return;
                if (!servedBitfield) {
                    std::vector<uint8_t> bf(1, 0x80);
                    if (!sendMessageSplit(fd, 5, bf, false)) return;
                    servedBitfield = true;
                }
            } else if (id == 6) {  // request
                std::vector<uint8_t> payload(12);
                if (recv(fd, payload.data(), 12, MSG_WAITALL) != 12) return;
                uint32_t piece = readBE32(payload.data());
                uint32_t offset = readBE32(payload.data() + 4);
                uint32_t length = readBE32(payload.data() + 8);
                (void)piece;
                if (offset + length > pieceData_.size()) return;

                std::vector<uint8_t> msg;
                msg.reserve(8 + length);
                msg.push_back(static_cast<uint8_t>((piece >> 24) & 0xFF));
                msg.push_back(static_cast<uint8_t>((piece >> 16) & 0xFF));
                msg.push_back(static_cast<uint8_t>((piece >> 8) & 0xFF));
                msg.push_back(static_cast<uint8_t>(piece & 0xFF));
                msg.push_back(static_cast<uint8_t>((offset >> 24) & 0xFF));
                msg.push_back(static_cast<uint8_t>((offset >> 16) & 0xFF));
                msg.push_back(static_cast<uint8_t>((offset >> 8) & 0xFF));
                msg.push_back(static_cast<uint8_t>(offset & 0xFF));
                msg.insert(msg.end(), pieceData_.begin() + offset,
                           pieceData_.begin() + offset + length);
                if (!sendMessageSplit(fd, 7, msg, true)) return;  // block, split payload
            } else {
                std::vector<uint8_t> skip(len - 1);
                if (skip.size() > 0 && recv(fd, skip.data(), skip.size(), MSG_WAITALL)
                        != static_cast<ssize_t>(skip.size())) {
                    return;
                }
            }
        }
    }

    std::string infoHash_;
    std::string pieceData_;
    int lfd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
};

// Make a single-file torrent with exactly one 16 KiB piece
TorrentInfo makeSingleFileTorrent(const std::string& data, const std::string& fileName) {
    TorrentInfo t;
    t.infoHash = std::string(20, 'x');
    t.peerId = peerId20();
    t.pieceLength = kBlockSize;
    t.fileName = fileName;
    t.name = fileName;
    t.fileLength = static_cast<int64_t>(data.size());
    t.pieces = utils::sha1(data);
    return t;
}

// Make a multi-file torrent whose single piece straddles two files
TorrentInfo makeMultiFileTorrent(const std::string& data) {
    TorrentInfo t;
    t.infoHash = std::string(20, 'y');
    t.peerId = peerId20();
    t.pieceLength = kBlockSize;
    t.name = "multi";
    t.fileLength = 0;

    TorrentInfo::FileEntry a;
    a.path = "dir1/a.bin";
    a.length = 10000;
    t.files.push_back(a);

    TorrentInfo::FileEntry b;
    b.path = "b.bin";
    b.length = static_cast<int64_t>(data.size()) - 10000;
    t.files.push_back(b);

    t.pieces = utils::sha1(data);
    return t;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

std::filesystem::path tempDir() {
    auto dir = std::filesystem::temp_directory_path() /
               ("gush_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST(DownloaderTest, DownloadSinglePieceFromLocalPeer) {
    const std::string pieceData(kBlockSize, 'A');
    TorrentInfo torrent = makeSingleFileTorrent(pieceData, "test.bin");

    MockPeerServer server(torrent.infoHash, pieceData);
    ASSERT_TRUE(server.start());

    DownloadOptions opts;
    opts.refreshTrackers = false;
    opts.maxPeers = 4;

    auto dir = tempDir();
    Downloader dl(torrent, dir.string(), opts);
    Peer p;
    p.ip = "127.0.0.1";
    p.port = server.port();
    dl.addPeers({p});
    dl.start();

    bool complete = false;
    for (int i = 0; i < 300 && !complete; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        complete = dl.isComplete();
    }
    dl.stop();
    server.join();

    EXPECT_TRUE(complete) << "download did not finish within 30s";
    EXPECT_NEAR(dl.getProgress(), 1.0, 0.001);

    // Data was verified and written to disk byte-for-byte
    EXPECT_EQ(readFile(dir / "test.bin"), pieceData);
    std::filesystem::remove_all(dir);
}

TEST(DownloaderTest, MultiFilePieceStraddlingBoundary) {
    // 10 KB in file A, rest in file B; the 16 KiB piece crosses both files
    const std::string pieceData(kBlockSize, 'B');
    TorrentInfo torrent = makeMultiFileTorrent(pieceData);

    MockPeerServer server(torrent.infoHash, pieceData);
    ASSERT_TRUE(server.start());

    DownloadOptions opts;
    opts.refreshTrackers = false;
    opts.maxPeers = 4;

    auto dir = tempDir();
    Downloader dl(torrent, dir.string(), opts);
    Peer p;
    p.ip = "127.0.0.1";
    p.port = server.port();
    dl.addPeers({p});
    dl.start();

    bool complete = false;
    for (int i = 0; i < 300 && !complete; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        complete = dl.isComplete();
    }
    dl.stop();
    server.join();

    EXPECT_TRUE(complete) << "multi-file download did not finish within 30s";

    EXPECT_EQ(readFile(dir / "dir1" / "a.bin"), pieceData.substr(0, 10000));
    EXPECT_EQ(readFile(dir / "b.bin"), pieceData.substr(10000));
    std::filesystem::remove_all(dir);
}
