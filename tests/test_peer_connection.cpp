#include <gtest/gtest.h>
#include "peer.h"
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

std::string peerId20() {
    return "-GT0001-ABCDEFGHIJKL";
}

// --- tiny wire helpers ------------------------------------------------------

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

// A listening "remote peer" whose behaviour is scripted per test.
class FakePeer {
public:
    bool start() {
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
        return true;
    }

    uint16_t port() const { return port_; }

    void serve(std::function<void(int)> script) {
        thread_ = std::thread([this, script = std::move(script)]() {
            int cfd = accept(lfd_, nullptr, nullptr);
            if (cfd >= 0) {
                script(cfd);
                close(cfd);
            }
        });
    }

    void join() {
        if (lfd_ >= 0) {
            close(lfd_);   // unblock a pending accept() if the script never ran
            lfd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    ~FakePeer() { join(); }

private:
    int lfd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
};

// Server-side handshake: verify the client's 68-byte handshake, reply with
// our own, then consume the client's interested message (5 bytes).
bool serverHandshake(int fd, const std::string& infoHash,
                     std::vector<uint8_t>& clientHandshake) {
    clientHandshake.resize(68);
    if (!recvFull(fd, clientHandshake.data(), 68)) return false;
    if (clientHandshake[0] != 19) return false;

    std::vector<uint8_t> resp(68, 0);
    resp[0] = 19;
    memcpy(&resp[1], "BitTorrent protocol", 19);
    memcpy(&resp[28], infoHash.data(), 20);
    memcpy(&resp[48], "-GT0001-SERVER-0000", 20);
    if (!sendFull(fd, resp.data(), 68)) return false;

    uint8_t interested[5];
    if (!recvFull(fd, interested, 5)) return false;
    return interested[4] == static_cast<uint8_t>(MessageId::Interested);
}

// Read one wire message from the client (4-byte len + id + payload).
bool readClientMessage(int fd, uint8_t& id, std::vector<uint8_t>& payload) {
    uint8_t lenBuf[4];
    if (!recvFull(fd, lenBuf, 4)) return false;
    uint32_t len = readBE32(lenBuf);
    if (len == 0) { id = static_cast<uint8_t>(MessageId::KeepAlive); return true; }
    if (!recvFull(fd, &id, 1)) return false;
    payload.resize(len - 1);
    if (len > 1 && !recvFull(fd, payload.data(), payload.size())) return false;
    return true;
}

// Send a message from the server side, optionally splitting the frame.
void sendServerMessage(int fd, uint8_t id, const std::vector<uint8_t>& payload,
                       bool split = false) {
    std::vector<uint8_t> frame;
    pushBE32(frame, static_cast<uint32_t>(1 + payload.size()));
    frame.push_back(id);
    frame.insert(frame.end(), payload.begin(), payload.end());

    if (split && frame.size() > 6) {
        size_t cut = frame.size() / 2;
        ASSERT_TRUE(sendFull(fd, frame.data(), cut));
        usleep(5000);
        ASSERT_TRUE(sendFull(fd, frame.data() + cut, frame.size() - cut));
    } else {
        ASSERT_TRUE(sendFull(fd, frame.data(), frame.size()));
    }
}

// Send a bare keep-alive frame (4 zero bytes), possibly in fragments.
void sendServerKeepAlive(int fd, bool split = false) {
    if (split) {
        ASSERT_TRUE(sendFull(fd, reinterpret_cast<const uint8_t*>("\x00\x00\x00"), 3));
        usleep(5000);
        ASSERT_TRUE(sendFull(fd, reinterpret_cast<const uint8_t*>("\x00"), 1));
    } else {
        const uint8_t ka[4] = {0, 0, 0, 0};
        ASSERT_TRUE(sendFull(fd, ka, 4));
    }
}

TorrentInfo makeTorrent(int numPieces = 8) {
    TorrentInfo t;
    t.infoHash = std::string(20, 'H');
    t.peerId = peerId20();
    t.pieceLength = 16384;
    t.fileName = "x.bin";
    t.name = "x.bin";
    t.fileLength = static_cast<int64_t>(numPieces) * 16384;
    t.pieces.assign(static_cast<size_t>(numPieces) * 20, '\0');
    return t;
}

// Poll receiveMessageNonBlocking until a message arrives (or attempts run out).
bool recvMsgWithRetry(PeerConnection& conn, PeerMessage& msg, int attempts = 50) {
    for (int i = 0; i < attempts; i++) {
        if (conn.receiveMessageNonBlocking(msg)) return true;
        usleep(2000);
    }
    return false;
}

}  // namespace

// Client handshake bytes must be well-formed and carry the right info hash,
// and sendRequest must be encoded per the wire protocol.
TEST(PeerConnectionTest, HandshakeAndRequestEncoding) {
    TorrentInfo torrent = makeTorrent();
    FakePeer peer;
    ASSERT_TRUE(peer.start());

    std::string infoHash = torrent.infoHash;

    std::vector<uint8_t> clientHs;
    uint8_t reqId = 0;
    std::vector<uint8_t> reqPayload;
    peer.serve([&](int fd) {
        ASSERT_TRUE(serverHandshake(fd, infoHash, clientHs));
        // Verify protocol string and info hash of the client handshake
        EXPECT_EQ(clientHs[0], 19);
        EXPECT_EQ(std::string(reinterpret_cast<char*>(&clientHs[1]), 19),
                  "BitTorrent protocol");
        EXPECT_EQ(std::string(reinterpret_cast<char*>(&clientHs[28]), 20), infoHash);

        // Unchoke + bitfield (piece 0 set), then wait for the request
        sendServerMessage(fd, static_cast<uint8_t>(MessageId::Unchoke), {});
        std::vector<uint8_t> bf(1, 0x80);
        sendServerMessage(fd, static_cast<uint8_t>(MessageId::Bitfield), bf);
        ASSERT_TRUE(readClientMessage(fd, reqId, reqPayload));
    });

    PeerConnection conn("127.0.0.1", peer.port(), torrent, peerId20());
    ASSERT_TRUE(conn.connect());

    // Drain the unchoke + bitfield the server sent
    PeerMessage msg;
    ASSERT_TRUE(recvMsgWithRetry(conn, msg));
    EXPECT_EQ(msg.id, MessageId::Unchoke);
    ASSERT_TRUE(recvMsgWithRetry(conn, msg));
    EXPECT_EQ(msg.id, MessageId::Bitfield);

    ASSERT_TRUE(conn.sendRequest(0, 0, 16384));
    peer.join();

    EXPECT_EQ(reqId, static_cast<uint8_t>(MessageId::Request));
    ASSERT_EQ(reqPayload.size(), 12u);
    EXPECT_EQ(readBE32(reqPayload.data()), 0u);          // piece
    EXPECT_EQ(readBE32(reqPayload.data() + 4), 0u);      // offset
    EXPECT_EQ(readBE32(reqPayload.data() + 8), 16384u);  // length

    // State updates from unchoke + bitfield must have been applied
    EXPECT_FALSE(conn.peerChoking());
    EXPECT_TRUE(conn.hasPiece(0));
    EXPECT_FALSE(conn.hasPiece(1));
}

// Keep-alives and fragmented frames must be handled transparently.
TEST(PeerConnectionTest, KeepAliveAndFragmentedMessages) {
    TorrentInfo torrent = makeTorrent();
    FakePeer peer;
    ASSERT_TRUE(peer.start());

    peer.serve([&](int fd) {
        std::vector<uint8_t> clientHs;
        ASSERT_TRUE(serverHandshake(fd, torrent.infoHash, clientHs));
        // Fragmented keep-alive, then fragmented have(3)
        sendServerKeepAlive(fd, /*split=*/true);
        std::vector<uint8_t> havePayload;
        pushBE32(havePayload, 3);
        sendServerMessage(fd, static_cast<uint8_t>(MessageId::Have), havePayload,
                          /*split=*/true);
        usleep(50000);  // keep the connection open until the client reads
    });

    PeerConnection conn("127.0.0.1", peer.port(), torrent, peerId20());
    ASSERT_TRUE(conn.connect());

    PeerMessage msg;
    ASSERT_TRUE(recvMsgWithRetry(conn, msg));
    EXPECT_EQ(msg.id, MessageId::KeepAlive);

    ASSERT_TRUE(recvMsgWithRetry(conn, msg));
    EXPECT_EQ(msg.id, MessageId::Have);
    EXPECT_TRUE(conn.hasPiece(3));
    peer.join();
}

// An absurd frame length must drop the connection instead of stalling.
TEST(PeerConnectionTest, OversizedFrameDisconnects) {
    TorrentInfo torrent = makeTorrent();
    FakePeer peer;
    ASSERT_TRUE(peer.start());

    peer.serve([&](int fd) {
        std::vector<uint8_t> clientHs;
        ASSERT_TRUE(serverHandshake(fd, torrent.infoHash, clientHs));
        // 16 MiB > 4 MiB cap
        const uint8_t huge[4] = {0x01, 0x00, 0x00, 0x00};
        ASSERT_TRUE(sendFull(fd, huge, 4));
        usleep(50000);
    });

    PeerConnection conn("127.0.0.1", peer.port(), torrent, peerId20());
    ASSERT_TRUE(conn.connect());

    // The server sends an absurd frame length; the client must drop the
    // connection once it sees it (poll until the frame arrives).
    bool disconnected = false;
    PeerMessage msg;
    for (int i = 0; i < 100 && !disconnected; i++) {
        conn.receiveMessageNonBlocking(msg);
        disconnected = !conn.isConnected();
        if (!disconnected) usleep(2000);
    }
    EXPECT_TRUE(disconnected);
    peer.join();
}

// Choke state and cancel encoding.
TEST(PeerConnectionTest, ChokeStateAndCancelEncoding) {
    TorrentInfo torrent = makeTorrent();
    FakePeer peer;
    ASSERT_TRUE(peer.start());

    uint8_t cancelId = 0;
    std::vector<uint8_t> cancelPayload;
    peer.serve([&](int fd) {
        std::vector<uint8_t> clientHs;
        ASSERT_TRUE(serverHandshake(fd, torrent.infoHash, clientHs));
        sendServerMessage(fd, static_cast<uint8_t>(MessageId::Choke), {},
                          /*split=*/true);
        ASSERT_TRUE(readClientMessage(fd, cancelId, cancelPayload));
    });

    PeerConnection conn("127.0.0.1", peer.port(), torrent, peerId20());
    ASSERT_TRUE(conn.connect());

    PeerMessage msg;
    ASSERT_TRUE(recvMsgWithRetry(conn, msg));
    EXPECT_EQ(msg.id, MessageId::Choke);
    EXPECT_TRUE(conn.peerChoking());

    ASSERT_TRUE(conn.sendCancel(1, 8192, 16384));
    peer.join();

    EXPECT_EQ(cancelId, static_cast<uint8_t>(MessageId::Cancel));
    ASSERT_EQ(cancelPayload.size(), 12u);
    EXPECT_EQ(readBE32(cancelPayload.data()), 1u);
    EXPECT_EQ(readBE32(cancelPayload.data() + 4), 8192u);
    EXPECT_EQ(readBE32(cancelPayload.data() + 8), 16384u);
}

// A block with a mismatched offset must be rejected by readBlock().
TEST(PeerConnectionTest, ReadBlockRejectsMismatchedOffset) {
    TorrentInfo torrent = makeTorrent();
    FakePeer peer;
    ASSERT_TRUE(peer.start());

    peer.serve([&](int fd) {
        std::vector<uint8_t> clientHs;
        ASSERT_TRUE(serverHandshake(fd, torrent.infoHash, clientHs));
        uint8_t id = 0;
        std::vector<uint8_t> payload;
        ASSERT_TRUE(readClientMessage(fd, id, payload));  // the request

        // Reply with a block whose offset differs from the request (100 != 0)
        std::vector<uint8_t> blockPayload;
        pushBE32(blockPayload, 0);    // piece
        pushBE32(blockPayload, 100);  // wrong offset
        blockPayload.insert(blockPayload.end(), 64, 'X');
        sendServerMessage(fd, static_cast<uint8_t>(MessageId::Block), blockPayload);
        usleep(50000);
    });

    PeerConnection conn("127.0.0.1", peer.port(), torrent, peerId20());
    ASSERT_TRUE(conn.connect());
    ASSERT_TRUE(conn.sendRequest(0, 0, 64));

    std::vector<uint8_t> data;
    EXPECT_FALSE(conn.readBlock(0, 0, 64, data));
    peer.join();
}
