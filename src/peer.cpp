#include "peer.h"
#include "utils.h"
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>

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
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

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
    
    // Reserved bytes (all zeros)
    memset(&handshake[20], 0, 8);
    
    // Info hash
    memcpy(&handshake[28], torrent_.infoHash.c_str(), 20);
    
    // Peer ID
    memcpy(&handshake[48], peerId_.c_str(), 20);
    
    // Send handshake
    ssize_t sent = send(socket_, handshake.data(), handshake.size(), 0);
    if (sent != 68) {
        return false;
    }

    // Receive handshake with timeout
    std::vector<uint8_t> recvHandshake(68);
    size_t receivedTotal = 0;
    
    // Set receive timeout
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (receivedTotal < 68) {
        ssize_t received = recv(socket_, recvHandshake.data() + receivedTotal, 68 - receivedTotal, 0);
        if (received <= 0) {
            return false;
        }
        receivedTotal += received;
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
    
    // Get peer ID (optional, store if needed)

    // Send interested (to let peer know we want data)
    sendInterested();
    
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
    
    ssize_t sent = send(socket_, buffer.data(), buffer.size(), 0);
    return sent == static_cast<ssize_t>(buffer.size());
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

bool PeerConnection::receiveMessage(PeerMessage& msg) {
    if (!connected_) return false;

    // Read length (4 bytes)
    uint8_t lengthBuf[4];
    ssize_t received = recv(socket_, lengthBuf, 4, 0);
    if (received != 4) {
        return false;
    }

    msg.length = (static_cast<uint32_t>(lengthBuf[0]) << 24) |
                 (static_cast<uint32_t>(lengthBuf[1]) << 16) |
                 (static_cast<uint32_t>(lengthBuf[2]) << 8) |
                 static_cast<uint32_t>(lengthBuf[3]);

    // Keep-alive message (length = 0)
    if (msg.length == 0) {
        msg.id = MessageId::KeepAlive;
        return true;
    }

    // Read message ID
    uint8_t idBuf[1];
    received = recv(socket_, idBuf, 1, 0);
    if (received != 1) {
        return false;
    }
    msg.id = static_cast<MessageId>(idBuf[0]);

    // Read payload
    if (msg.length > 1) {
        msg.payload.resize(msg.length - 1);
        received = recv(socket_, msg.payload.data(), msg.payload.size(), 0);
        if (received != static_cast<ssize_t>(msg.payload.size())) {
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

    // Read length (4 bytes)
    uint8_t lengthBuf[4];
    ssize_t received = recv(socket_, lengthBuf, 4, MSG_DONTWAIT);
    if (received != 4) {
        return false;
    }

    msg.length = (static_cast<uint32_t>(lengthBuf[0]) << 24) |
                 (static_cast<uint32_t>(lengthBuf[1]) << 16) |
                 (static_cast<uint32_t>(lengthBuf[2]) << 8) |
                 static_cast<uint32_t>(lengthBuf[3]);

    // Keep-alive message (length = 0)
    if (msg.length == 0) {
        msg.id = MessageId::KeepAlive;
        return true;
    }

    // Read message ID
    uint8_t idBuf[1];
    received = recv(socket_, idBuf, 1, MSG_DONTWAIT);
    if (received != 1) {
        return false;
    }
    msg.id = static_cast<MessageId>(idBuf[0]);

    // Read payload
    if (msg.length > 1) {
        msg.payload.resize(msg.length - 1);
        received = recv(socket_, msg.payload.data(), msg.payload.size(), MSG_DONTWAIT);
        if (received != static_cast<ssize_t>(msg.payload.size())) {
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
        ssize_t received = recv(socket_, lengthBuf, 4, MSG_WAITALL);
        if (received != 4) return false;
        
        uint32_t msgLength = (static_cast<uint32_t>(lengthBuf[0]) << 24) |
                             (static_cast<uint32_t>(lengthBuf[1]) << 16) |
                             (static_cast<uint32_t>(lengthBuf[2]) << 8) |
                             static_cast<uint32_t>(lengthBuf[3]);
        
        // Read message ID
        uint8_t id;
        received = recv(socket_, &id, 1, MSG_WAITALL);
        if (received != 1) return false;
        
        if (static_cast<MessageId>(id) != MessageId::Block) {
            // Not a block message, skip payload
            std::vector<uint8_t> skip(msgLength - 1);
            recv(socket_, skip.data(), skip.size(), MSG_WAITALL);
            continue;
        }
        
        // Read piece index (4 bytes)
        uint8_t pieceBuf[4];
        received = recv(socket_, pieceBuf, 4, MSG_WAITALL);
        if (received != 4) return false;
        
        // Read offset (4 bytes)
        uint8_t offsetBuf[4];
        received = recv(socket_, offsetBuf, 4, MSG_WAITALL);
        if (received != 4) return false;
        
        // Read block data
        uint32_t blockLength = msgLength - 9;  // 1 (id) + 4 (piece) + 4 (offset)
        size_t remaining = length - totalRead;
        size_t toRead = std::min(blockLength, static_cast<uint32_t>(remaining));
        
        received = recv(socket_, data.data() + totalRead, toRead, MSG_WAITALL);
        if (received != static_cast<ssize_t>(toRead)) return false;
        
        totalRead += toRead;
        
        if (blockLength > toRead) {
            // Skip remaining data
            std::vector<uint8_t> skip(blockLength - toRead);
            recv(socket_, skip.data(), skip.size(), MSG_WAITALL);
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
