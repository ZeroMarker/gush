#include <gtest/gtest.h>
#include "metadata.h"
#include "utils.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <functional>
#include <chrono>

namespace {

constexpr uint8_t kExtMsgId = 20;   // BEP 10 extension protocol
constexpr uint8_t kExtHandshake = 0;
constexpr int kMetadataPieceSize = 16384;

// --- wire helpers -----------------------------------------------------------

bool sendFull(int fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recvFull(int fd, uint8_t* data, size_t size) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = recv(fd, data + got, size - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

void pushBE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xFF);
    v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);
    v.push_back(x & 0xFF);
}

uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Build a bencoded extended-handshake dict announcing metadata size + ut_metadata.
std::vector<uint8_t> makeExtHandshake(int metadataSize, int utMetadataId) {
    std::string body = "d13:metadata_sizei" + std::to_string(metadataSize) +
                       "e1:md11:ut_metadatai" + std::to_string(utMetadataId) + "eee";
    return std::vector<uint8_t>(body.begin(), body.end());
}

// Send an extended message: <len><20><extId><payload>, optionally split.
bool sendExt(int fd, uint8_t extId, const std::vector<uint8_t>& payload,
             bool split = false) {
    std::vector<uint8_t> frame;
    pushBE32(frame, static_cast<uint32_t>(2 + payload.size()));
    frame.push_back(kExtMsgId);
    frame.push_back(extId);
    frame.insert(frame.end(), payload.begin(), payload.end());

    if (split && frame.size() > 8) {
        size_t cut = frame.size() / 2;
        if (!sendFull(fd, frame.data(), cut)) return false;
        usleep(5000);
        return sendFull(fd, frame.data() + cut, frame.size() - cut);
    }
    return sendFull(fd, frame.data(), frame.size());
}

// Read a full wire frame (len + id + payload) from the client.
bool readFrame(int fd, uint8_t& id, std::vector<uint8_t>& payload) {
    uint8_t lenBuf[4];
    if (!recvFull(fd, lenBuf, 4)) return false;
    uint32_t len = readBE32(lenBuf);
    if (len == 0) { id = 255; return true; }  // keep-alive
    if (!recvFull(fd, &id, 1)) return false;
    payload.resize(len - 1);
    if (len > 1 && !recvFull(fd, payload.data(), payload.size())) return false;
    return true;
}

// The "remote peer" performing a full BEP 9/10 metadata exchange.
class MetadataPeerServer {
public:
    // script(fd, metadata): called after the extended handshake. It should
    // answer the client's ut_metadata requests.
    bool start(std::function<void(int, const std::string&)> script,
               const std::string& metadata, int metadataSizeOverride = -1,
               int utMetadataId = 1) {
        metadata_ = metadata;
        sizeOverride_ = metadataSizeOverride;
        utMetadataId_ = utMetadataId;

        lfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd_ < 0) return false;

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(lfd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (listen(lfd_, 1) != 0) return false;

        socklen_t len = sizeof(addr);
        if (getsockname(lfd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port_ = ntohs(addr.sin_port);

        thread_ = std::thread([this, script = std::move(script)]() {
            int cfd = accept(lfd_, nullptr, nullptr);
            if (cfd < 0) return;
            serve(cfd, script);
            close(cfd);
        });
        return true;
    }

    uint16_t port() const { return port_; }

    void join() {
        if (lfd_ >= 0) { close(lfd_); lfd_ = -1; }
        if (thread_.joinable()) thread_.join();
    }

    ~MetadataPeerServer() { join(); }

private:
    void serve(int fd, std::function<void(int, const std::string&)> script) {
        // --- handshake ---
        std::vector<uint8_t> hs(68);
        if (!recvFull(fd, hs.data(), 68)) return;
        if (hs[0] != 19) return;
        EXPECT_NE(hs[25] & 0x10, 0);       // BEP 10 reserved byte 5

        std::vector<uint8_t> resp(68, 0);
        resp[0] = 19;
        memcpy(&resp[1], "BitTorrent protocol", 19);
        memcpy(&resp[28], &hs[28], 20);   // echo the client's info hash
        memcpy(&resp[48], "-GT0001-SERVER-0000", 20);
        resp[27] = 0x10;                  // BEP 10 extended protocol flag
        if (!sendFull(fd, resp.data(), 68)) return;

        // --- extended handshake ---
        uint8_t id = 0;
        std::vector<uint8_t> payload;
        if (!readFrame(fd, id, payload)) return;           // client's ext handshake
        if (id != kExtMsgId || payload.empty() || payload[0] != kExtHandshake) return;

        int metadataSize = (sizeOverride_ >= 0) ? sizeOverride_
                                                : static_cast<int>(metadata_.size());
        if (!sendExt(fd, kExtHandshake, makeExtHandshake(metadataSize, utMetadataId_),
                     /*split=*/true)) return;

        script(fd, metadata_);
    }

    std::string metadata_;
    int sizeOverride_ = -1;
    int utMetadataId_ = 1;
    int lfd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
};

// Build a request/response body: d8:msg_typei<type>e5:piecei<N>e
std::vector<uint8_t> metaDict(int msgType, int piece) {
    std::string body = "d8:msg_typei" + std::to_string(msgType) +
                       "e5:piecei" + std::to_string(piece) + "ee";
    return std::vector<uint8_t>(body.begin(), body.end());
}

// Standard metadata-serving script: answer every request with the piece data.
void serveMetadataPieces(int fd, const std::string& metadata) {
    int numPieces = (static_cast<int>(metadata.size()) + kMetadataPieceSize - 1)
                    / kMetadataPieceSize;
    for (int i = 0; i < numPieces; i++) {
        uint8_t id = 0;
        std::vector<uint8_t> payload;
        if (!readFrame(fd, id, payload)) return;   // the request
        if (id != kExtMsgId || payload.empty()) return;
        // payload[0] is the ut_metadata id, rest is the bencoded request dict

        auto body = metaDict(1, i);
        std::vector<uint8_t> data(body.begin(), body.end());
        size_t off = static_cast<size_t>(i) * kMetadataPieceSize;
        size_t len = std::min<size_t>(kMetadataPieceSize, metadata.size() - off);
        data.insert(data.end(), metadata.begin() + off, metadata.begin() + off + len);
        if (!sendExt(fd, 1, data, /*split=*/i == 1)) return;  // split the 2nd piece
    }
}

MagnetLink makeMagnet(const std::string& metadata) {
    MagnetLink magnet;
    magnet.infoHashRaw = utils::sha1(metadata);
    magnet.infoHash = utils::toHex(magnet.infoHashRaw);
    magnet.displayName = "test";
    return magnet;
}

}  // namespace

// Full happy path: handshake (fragmented), ext handshake, two metadata pieces
// (second split on the wire) assembled and hash-verified.
TEST(MetadataDownloaderTest, DownloadMetadataTwoPieces) {
    std::string metadata(kMetadataPieceSize + 4096, 'M');  // 20 KiB > 16 KiB
    // Prefix with a real bencoded dict so the payload parses cleanly
    std::string infoDict = "d4:name6:file.b6:lengthi" +
                           std::to_string(kMetadataPieceSize + 4096) + "e10:piece leni16384e6:pieces20:" +
                           std::string(20, 'x') + "e";
    metadata = infoDict + std::string(kMetadataPieceSize - infoDict.size() + 4096, 'M');

    MetadataPeerServer server;
    ASSERT_TRUE(server.start(serveMetadataPieces, metadata));

    MetadataDownloader dl(makeMagnet(metadata));
    dl.addPeer("127.0.0.1", server.port());
    server.join();

    EXPECT_TRUE(dl.isComplete());
    EXPECT_EQ(dl.getTorrentData(), metadata);
}

// Single small piece, fragmented everything.
TEST(MetadataDownloaderTest, DownloadMetadataSingleSmallPiece) {
    std::string metadata = "d4:name4:test6:lengthi5ee";

    MetadataPeerServer server;
    ASSERT_TRUE(server.start(serveMetadataPieces, metadata));

    MetadataDownloader dl(makeMagnet(metadata));
    dl.addPeer("127.0.0.1", server.port());
    server.join();

    EXPECT_TRUE(dl.isComplete());
    EXPECT_EQ(dl.getTorrentData(), metadata);
}

// Peer that rejects every metadata request must fail the download cleanly.
TEST(MetadataDownloaderTest, PeerRejectFails) {
    std::string metadata = "d4:name4:test6:lengthi5ee";

    MetadataPeerServer server;
    ASSERT_TRUE(server.start([](int fd, const std::string&) {
        uint8_t id = 0;
        std::vector<uint8_t> payload;
        if (!readFrame(fd, id, payload)) return;
        // msg_type=2 (reject)
        if (!sendExt(fd, 1, metaDict(2, 0), false)) return;
        // drain anything else briefly so the client's read returns
        usleep(50000);
    }, metadata));

    MetadataDownloader dl(makeMagnet(metadata));
    dl.addPeer("127.0.0.1", server.port());
    server.join();

    EXPECT_FALSE(dl.isComplete());
    EXPECT_TRUE(dl.getTorrentData().empty());
}

// Metadata whose bytes do not match the info hash must be rejected.
TEST(MetadataDownloaderTest, HashMismatchRejected) {
    std::string metadata = "d4:name4:test6:lengthi5ee";
    std::string wrongData(metadata.size(), 'X');  // not sha1(infoHashRaw)

    MetadataPeerServer server;
    ASSERT_TRUE(server.start([wrongData](int fd, const std::string&) {
        uint8_t id = 0;
        std::vector<uint8_t> payload;
        if (!readFrame(fd, id, payload)) return;
        auto body = metaDict(1, 0);
        std::vector<uint8_t> data(body.begin(), body.end());
        data.insert(data.end(), wrongData.begin(), wrongData.end());
        sendExt(fd, 1, data, false);
        usleep(50000);
    }, metadata));

    MetadataDownloader dl(makeMagnet(metadata));
    dl.addPeer("127.0.0.1", server.port());
    server.join();

    EXPECT_FALSE(dl.isComplete());
    EXPECT_TRUE(dl.getTorrentData().empty());
}

// Peer advertises 16 KiB but sends fewer bytes: size mismatch must fail.
TEST(MetadataDownloaderTest, SizeMismatchRejected) {
    std::string metadata = "d4:name4:test6:lengthi5ee";

    MetadataPeerServer server;
    ASSERT_TRUE(server.start([](int fd, const std::string&) {
        uint8_t id = 0;
        std::vector<uint8_t> payload;
        if (!readFrame(fd, id, payload)) return;
        auto body = metaDict(1, 0);
        std::vector<uint8_t> data(body.begin(), body.end());
        data.insert(data.end(), 100, 'Z');  // far fewer than advertised 16384
        sendExt(fd, 1, data, false);
        usleep(50000);
    }, metadata, /*sizeOverride=*/kMetadataPieceSize));

    MetadataDownloader dl(makeMagnet(metadata));
    dl.addPeer("127.0.0.1", server.port());
    server.join();

    EXPECT_FALSE(dl.isComplete());
    EXPECT_TRUE(dl.getTorrentData().empty());
}
