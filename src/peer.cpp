#include "peer.h"
#include "bencode.h"
#include "utils.h"
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/ioctl.h>

namespace {
constexpr uint32_t MAX_PEER_MESSAGE_SIZE = 4 * 1024 * 1024;
constexpr size_t MAX_PEX_PEERS_PER_MESSAGE = 200;

bool sendAll(int socket, const uint8_t* data, size_t size) {
    size_t totalSent = 0;
    while (totalSent < size) {
        // MSG_NOSIGNAL: a peer disconnect must not kill the process via SIGPIPE
        ssize_t sent = send(socket, data + totalSent, size - totalSent, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        totalSent += static_cast<size_t>(sent);
    }
    return true;
}

bool recvAll(int socket, uint8_t* data, size_t size, int flags) {
    size_t totalReceived = 0;
    while (totalReceived < size) {
        ssize_t received = recv(socket, data + totalReceived, size - totalReceived, flags);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        totalReceived += static_cast<size_t>(received);
    }
    return true;
}
}

PeerMessage PeerMessage::createKeepAlive() {
    PeerMessage msg;
    msg.length = 0;
    msg.id = MessageId::KeepAlive;
    return msg;
}

PeerMessage PeerMessage::createChoke() {
    PeerMessage msg;
    msg.length = 1;
    msg.id = MessageId::Choke;
    return msg;
}

PeerMessage PeerMessage::createUnchoke() {
    PeerMessage msg;
    msg.length = 1;
    msg.id = MessageId::Unchoke;
    return msg;
}

PeerMessage PeerMessage::createInterested() {
    PeerMessage msg;
    msg.length = 1;
    msg.id = MessageId::Interested;
    return msg;
}

PeerMessage PeerMessage::createHave(uint32_t pieceIndex) {
    PeerMessage msg;
    msg.length = 5;
    msg.id = MessageId::Have;
    std::string bytes = utils::intToBytes(pieceIndex);
    msg.payload.assign(bytes.begin(), bytes.end());
    return msg;
}

PeerMessage PeerMessage::createRequest(uint32_t piece, uint32_t offset, uint32_t length) {
    PeerMessage msg;
    msg.length = 13;
    msg.id = MessageId::Request;
    msg.payload.resize(12);
    std::string pieceBytes = utils::intToBytes(piece);
    std::string offsetBytes = utils::intToBytes(offset);
    std::string lengthBytes = utils::intToBytes(length);
    memcpy(msg.payload.data(), pieceBytes.c_str(), 4);
    memcpy(msg.payload.data() + 4, offsetBytes.c_str(), 4);
    memcpy(msg.payload.data() + 8, lengthBytes.c_str(), 4);
    return msg;
}

PeerMessage PeerMessage::createCancel(uint32_t piece, uint32_t offset, uint32_t length) {
    PeerMessage msg;
    msg.length = 13;
    msg.id = MessageId::Cancel;
    msg.payload.resize(12);
    std::string pieceBytes = utils::intToBytes(piece);
    std::string offsetBytes = utils::intToBytes(offset);
    std::string lengthBytes = utils::intToBytes(length);
    memcpy(msg.payload.data(), pieceBytes.c_str(), 4);
    memcpy(msg.payload.data() + 4, offsetBytes.c_str(), 4);
    memcpy(msg.payload.data() + 8, lengthBytes.c_str(), 4);
    return msg;
}

PeerConnection::PeerConnection(
    const std::string& ip, 
    uint16_t port, 
    const TorrentInfo& torrent, 
    const std::string& peerId
) : ip_(ip), port_(port), torrent_(torrent), peerId_(peerId) {
}

PeerConnection::~PeerConnection() {
    disconnect();
}

bool PeerConnection::connect() {
    if (connected_) {
        return true;
    }
    
    // Create socket
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        return false;
    }
    
    // Set non-blocking
    int flags = fcntl(socket_, F_GETFL, 0);
    fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
    
    // Set up address
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1) {
        // Not a valid IPv4 address (e.g. a hostname from a non-compact tracker)
        disconnect();
        return false;
    }

    // Set socket options for keep-alive
    int keepAlive = 1;
    setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(keepAlive));

    // Connect
    int result = ::connect(socket_, (struct sockaddr*)&addr, sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        disconnect();
        return false;
    }
    
    // Wait for connection (with timeout)
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(socket_, &fds);
    
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    
    result = select(socket_ + 1, nullptr, &fds, nullptr, &tv);
    if (result <= 0) {
        disconnect();
        return false;
    }
    
    // Check for connection error
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(socket_, SOL_SOCKET, SO_ERROR, &error, &len);
    if (error != 0) {
        disconnect();
        return false;
    }
    
    // Set blocking mode
    fcntl(socket_, F_SETFL, flags);
    
    connected_ = true;
    
    // Perform handshake
    if (!performHandshake()) {
        disconnect();
        return false;
    }
    
    return true;
}

void PeerConnection::disconnect() {
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
    connected_ = false;
}

bool PeerConnection::performHandshake() {
    // Build handshake message
    // Protocol length (1 byte) + Protocol string (19 bytes) + 
    // Reserved (8 bytes) + Info hash (20 bytes) + Peer ID (20 bytes)
    
    std::vector<uint8_t> handshake(68);
    
    // Protocol length
    handshake[0] = 19;
    
    // Protocol string: "BitTorrent protocol"
    memcpy(&handshake[1], "BitTorrent protocol", 19);
    
    // Reserved bytes. Advertise BEP 10 extension protocol support.
    memset(&handshake[20], 0, 8);
    handshake[25] |= 0x10;
    
    // Info hash
    memcpy(&handshake[28], torrent_.infoHash.c_str(), 20);
    
    // Peer ID
    memcpy(&handshake[48], peerId_.c_str(), 20);
    
    // Send handshake
    if (!sendAll(socket_, handshake.data(), handshake.size())) {
        return false;
    }

    // Receive handshake with timeout
    std::vector<uint8_t> recvHandshake(68);
    // Set receive timeout
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (!recvAll(socket_, recvHandshake.data(), recvHandshake.size(), 0)) {
        return false;
    }
    
    // Reset to blocking mode
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    // Verify protocol string
    if (recvHandshake[0] != 19 || 
        memcmp(&recvHandshake[1], "BitTorrent protocol", 19) != 0) {
        return false;
    }
    
    // Verify info hash
    if (memcmp(&recvHandshake[28], torrent_.infoHash.c_str(), 20) != 0) {
        return false;
    }

    peerSupportsExtensions_ = (recvHandshake[25] & 0x10) != 0;
    
    // Get peer ID (optional, store if needed)

    // BEP 3: the bitfield message is optional - peers that omit it announce
    // availability via have messages instead. Initialise the bitfield so those
    // have messages are not silently dropped.
    bitfield_.assign(torrent_.numPieces(), false);

    // Send interested (to let peer know we want data)
    if (!sendInterested()) return false;
    if (peerSupportsExtensions_ && !sendExtensionHandshake()) return false;
    
    // Note: We are now interested, but peer is still choking us
    // We need to wait for peer to unchoke before requesting blocks
    // The peer will send an Unchoke message when ready

    return true;
}

bool PeerConnection::sendMessage(const PeerMessage& msg) {
    if (!connected_) return false;
    
    std::vector<uint8_t> buffer(4);
    
    // Length (4 bytes, big-endian)
    uint32_t length = msg.length;
    buffer[0] = (length >> 24) & 0xFF;
    buffer[1] = (length >> 16) & 0xFF;
    buffer[2] = (length >> 8) & 0xFF;
    buffer[3] = length & 0xFF;
    
    // Message ID (if not keep-alive)
    if (msg.length > 0) {
        buffer.push_back(static_cast<uint8_t>(msg.id));
        buffer.insert(buffer.end(), msg.payload.begin(), msg.payload.end());
    }
    
    if (!sendAll(socket_, buffer.data(), buffer.size())) {
        // Broken pipe / peer gone: reflect it in our state so the download
        // loop stops scheduling requests against this connection.
        disconnect();
        return false;
    }
    return true;
}

bool PeerConnection::sendChoke() {
    setAmChoking(true);
    return sendMessage(PeerMessage::createChoke());
}

bool PeerConnection::sendUnchoke() {
    setAmChoking(false);
    return sendMessage(PeerMessage::createUnchoke());
}

bool PeerConnection::sendInterested() {
    setAmInterested(true);
    return sendMessage(PeerMessage::createInterested());
}

bool PeerConnection::sendHave(uint32_t pieceIndex) {
    return sendMessage(PeerMessage::createHave(pieceIndex));
}

bool PeerConnection::sendRequest(uint32_t piece, uint32_t offset, uint32_t length) {
    return sendMessage(PeerMessage::createRequest(piece, offset, length));
}

bool PeerConnection::sendCancel(uint32_t piece, uint32_t offset, uint32_t length) {
    return sendMessage(PeerMessage::createCancel(piece, offset, length));
}

bool PeerConnection::sendExtensionHandshake() {
    bencode::BencodeDict extensions;
    extensions["ut_pex"] = int64_t(1);
    bencode::BencodeDict handshake;
    handshake["m"] = std::move(extensions);
    std::string encoded = bencode::encode(bencode::BencodeValue(std::move(handshake)));

    PeerMessage msg;
    msg.id = MessageId::Extended;
    msg.payload.reserve(encoded.size() + 1);
    msg.payload.push_back(0);  // extended handshake
    msg.payload.insert(msg.payload.end(), encoded.begin(), encoded.end());
    msg.length = static_cast<uint32_t>(1 + msg.payload.size());
    return sendMessage(msg);
}

void PeerConnection::processExtendedMessage(const std::vector<uint8_t>& payload) {
    if (payload.size() < 2) return;

    const uint8_t extensionId = payload[0];
    const std::string encoded(payload.begin() + 1, payload.end());
    try {
        const auto value = bencode::parse(encoded);
        if (!value.isDict()) return;

        if (extensionId == 0) {
            const auto* mapping = bencode::dictGet(value.asDict(), "m");
            if (mapping && mapping->isDict()) {
                const auto id = bencode::dictGetInt(mapping->asDict(), "ut_pex");
                peerSupportsPex_ = id && *id > 0 && *id <= 255;
            }
            return;
        }

        // We advertised ut_pex as local extension ID 1. Incoming PEX messages
        // therefore use ID 1, regardless of the ID chosen by the remote peer.
        if (extensionId != 1 || !peerSupportsPex_) return;
        const auto compact = bencode::dictGetString(value.asDict(), "added");
        if (!compact) return;

        for (auto& peer : parseCompactPeers(*compact)) {
            if (discoveredPeers_.size() >= MAX_PEX_PEERS_PER_MESSAGE) break;
            if (peer.port == 0 || (peer.ip == ip_ && peer.port == port_)) continue;
            bool duplicate = false;
            for (const auto& existing : discoveredPeers_) {
                if (existing == peer) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) discoveredPeers_.push_back(std::move(peer));
        }
    } catch (...) {
        // Extension messages are untrusted; malformed PEX data is ignored.
    }
}

std::vector<Peer> PeerConnection::takeDiscoveredPeers() {
    std::vector<Peer> peers;
    peers.swap(discoveredPeers_);
    return peers;
}

bool PeerConnection::receiveMessage(PeerMessage& msg) {
    if (!connected_) return false;

    // Read length (4 bytes)
    uint8_t lengthBuf[4];
    if (!recvAll(socket_, lengthBuf, 4, MSG_WAITALL)) {
        return false;
    }

    msg.length = (static_cast<uint32_t>(lengthBuf[0]) << 24) |
                 (static_cast<uint32_t>(lengthBuf[1]) << 16) |
                 (static_cast<uint32_t>(lengthBuf[2]) << 8) |
                 static_cast<uint32_t>(lengthBuf[3]);

    if (msg.length > MAX_PEER_MESSAGE_SIZE) {
        // A peer that sends an absurd frame length is either broken or hostile;
        // leave the socket buffer clean by dropping the connection instead of
        // returning false forever with the frame still queued.
        disconnect();
        return false;
    }

    // Keep-alive message (length = 0)
    if (msg.length == 0) {
        msg.id = MessageId::KeepAlive;
        return true;
    }

    // Read message ID
    uint8_t idBuf[1];
    if (!recvAll(socket_, idBuf, 1, MSG_WAITALL)) {
        return false;
    }
    msg.id = static_cast<MessageId>(idBuf[0]);

    // Read payload
    if (msg.length > 1) {
        msg.payload.resize(msg.length - 1);
        if (!recvAll(socket_, msg.payload.data(), msg.payload.size(), MSG_WAITALL)) {
            return false;
        }
    }

    // Update state based on message
    switch (msg.id) {
        case MessageId::Choke:
            peerChoking_ = true;
            break;
        case MessageId::Unchoke:
            peerChoking_ = false;
            break;
        case MessageId::Interested:
            peerInterested_ = true;
            break;
        case MessageId::NotInterested:
            peerInterested_ = false;
            break;
        case MessageId::Have:
            if (msg.payload.size() >= 4) {
                uint32_t pieceIndex = utils::bytesToInt(msg.payload.data());
                if (pieceIndex < bitfield_.size()) {
                    bitfield_[pieceIndex] = true;
                }
            }
            break;
        case MessageId::Bitfield:
            updateBitfield(msg.payload);
            break;
        case MessageId::Extended:
            processExtendedMessage(msg.payload);
            break;
        default:
            break;
    }

    return true;
}

bool PeerConnection::receiveMessageNonBlocking(PeerMessage& msg) {
    if (!connected_) return false;

    // Check if data is available
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(socket_, &readfds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;  // Non-blocking

    int ret = select(socket_ + 1, &readfds, nullptr, nullptr, &tv);
    if (ret <= 0 || !FD_ISSET(socket_, &readfds)) {
        return false;  // No data available
    }

    int available = 0;
    if (ioctl(socket_, FIONREAD, &available) != 0 || available < 4) {
        return false;
    }

    // Peek length (4 bytes) so partial messages stay in the socket buffer.
    uint8_t lengthBuf[4];
    if (recv(socket_, lengthBuf, 4, MSG_PEEK | MSG_DONTWAIT) != 4) {
        return false;
    }

    msg.length = (static_cast<uint32_t>(lengthBuf[0]) << 24) |
                 (static_cast<uint32_t>(lengthBuf[1]) << 16) |
                 (static_cast<uint32_t>(lengthBuf[2]) << 8) |
                 static_cast<uint32_t>(lengthBuf[3]);

    msg.length = (static_cast<uint32_t>(lengthBuf[0]) << 24) |
                 (static_cast<uint32_t>(lengthBuf[1]) << 16) |
                 (static_cast<uint32_t>(lengthBuf[2]) << 8) |
                 static_cast<uint32_t>(lengthBuf[3]);

    if (msg.length > MAX_PEER_MESSAGE_SIZE) {
        // Same as receiveMessage(): drop the connection on an absurd frame.
        disconnect();
        return false;
    }

    if (available < static_cast<int64_t>(4 + msg.length)) {
        return false;  // Partial frame: keep it queued, retry next poll
    }

    if (!recvAll(socket_, lengthBuf, 4, MSG_DONTWAIT)) {
        return false;
    }

    // Keep-alive message (length = 0)
    if (msg.length == 0) {
        msg.id = MessageId::KeepAlive;
        return true;
    }

    // Read message ID
    uint8_t idBuf[1];
    if (!recvAll(socket_, idBuf, 1, MSG_DONTWAIT)) {
        return false;
    }
    msg.id = static_cast<MessageId>(idBuf[0]);

    // Read payload
    if (msg.length > 1) {
        msg.payload.resize(msg.length - 1);
        if (!recvAll(socket_, msg.payload.data(), msg.payload.size(), MSG_DONTWAIT)) {
            return false;
        }
    }

    // Update state based on message
    switch (msg.id) {
        case MessageId::Choke:
            peerChoking_ = true;
            break;
        case MessageId::Unchoke:
            peerChoking_ = false;
            break;
        case MessageId::Interested:
            peerInterested_ = true;
            break;
        case MessageId::NotInterested:
            peerInterested_ = false;
            break;
        case MessageId::Have:
            if (msg.payload.size() >= 4) {
                uint32_t pieceIndex = utils::bytesToInt(msg.payload.data());
                if (pieceIndex < bitfield_.size()) {
                    bitfield_[pieceIndex] = true;
                }
            }
            break;
        case MessageId::Bitfield:
            updateBitfield(msg.payload);
            break;
        case MessageId::Block:
            // Block message - payload will be processed by downloader
            break;
        case MessageId::Extended:
            processExtendedMessage(msg.payload);
            break;
        default:
            break;
    }

    return true;
}

bool PeerConnection::readBlock(uint32_t piece, uint32_t offset, uint32_t length, 
                                std::vector<uint8_t>& data) {
    if (!connected_) return false;
    
    data.resize(length);
    size_t totalRead = 0;
    
    while (totalRead < length) {
        // Read message length
        uint8_t lengthBuf[4];
        if (!recvAll(socket_, lengthBuf, 4, MSG_WAITALL)) return false;
        
        uint32_t msgLength = (static_cast<uint32_t>(lengthBuf[0]) << 24) |
                             (static_cast<uint32_t>(lengthBuf[1]) << 16) |
                             (static_cast<uint32_t>(lengthBuf[2]) << 8) |
                             static_cast<uint32_t>(lengthBuf[3]);

        if (msgLength == 0) {
            continue;
        }

        if (msgLength > MAX_PEER_MESSAGE_SIZE) {
            return false;
        }
        
        // Read message ID
        uint8_t id;
        if (!recvAll(socket_, &id, 1, MSG_WAITALL)) return false;
        
        if (static_cast<MessageId>(id) != MessageId::Block) {
            // Not a block message, skip payload
            std::vector<uint8_t> skip(msgLength - 1);
            if (!recvAll(socket_, skip.data(), skip.size(), MSG_WAITALL)) return false;
            continue;
        }

        if (msgLength < 9) {
            return false;
        }
        
        // Read piece index (4 bytes)
        uint8_t pieceBuf[4];
        if (!recvAll(socket_, pieceBuf, 4, MSG_WAITALL)) return false;
        uint32_t responsePiece = utils::bytesToInt(pieceBuf);
        
        // Read offset (4 bytes)
        uint8_t offsetBuf[4];
        if (!recvAll(socket_, offsetBuf, 4, MSG_WAITALL)) return false;
        uint32_t responseOffset = utils::bytesToInt(offsetBuf);

        if (responsePiece != piece || responseOffset != offset + totalRead) {
            uint32_t blockLength = msgLength - 9;
            std::vector<uint8_t> skip(blockLength);
            if (!recvAll(socket_, skip.data(), skip.size(), MSG_WAITALL)) return false;
            return false;
        }
        
        // Read block data
        uint32_t blockLength = msgLength - 9;  // 1 (id) + 4 (piece) + 4 (offset)
        size_t remaining = length - totalRead;
        size_t toRead = std::min(blockLength, static_cast<uint32_t>(remaining));
        
        if (!recvAll(socket_, data.data() + totalRead, toRead, MSG_WAITALL)) return false;
        
        totalRead += toRead;
        
        if (blockLength > toRead) {
            // Skip remaining data
            std::vector<uint8_t> skip(blockLength - toRead);
            if (!recvAll(socket_, skip.data(), skip.size(), MSG_WAITALL)) return false;
        }
    }
    
    return true;
}

void PeerConnection::updateBitfield(const std::vector<uint8_t>& data) {
    bitfield_.clear();
    bitfield_.resize(torrent_.numPieces());
    
    for (size_t i = 0; i < data.size() && i * 8 < bitfield_.size(); i++) {
        for (int j = 0; j < 8 && (i * 8 + j) < bitfield_.size(); j++) {
            if (data[i] & (0x80 >> j)) {
                bitfield_[i * 8 + j] = true;
            }
        }
    }
}

bool PeerConnection::hasPiece(uint32_t index) const {
    if (index >= bitfield_.size()) return false;
    return bitfield_[index];
}
