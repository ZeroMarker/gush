#ifndef PEER_H
#define PEER_H

#include "torrent.h"
#include "tracker.h"
#include <string>
#include <vector>
#include <cstdint>
#include <set>

// Peer wire protocol message IDs
enum class MessageId : uint8_t {
    Choke = 0,
    Unchoke = 1,
    Interested = 2,
    NotInterested = 3,
    Have = 4,
    Bitfield = 5,
    Request = 6,
    Block = 7,
    Cancel = 8,
    Port = 9,
    Extended = 20,
    KeepAlive = 255  // Special case: length 0
};

struct PeerMessage {
    uint32_t length;
    MessageId id;
    std::vector<uint8_t> payload;
    
    static PeerMessage createKeepAlive();
    static PeerMessage createChoke();
    static PeerMessage createUnchoke();
    static PeerMessage createInterested();
    static PeerMessage createHave(uint32_t pieceIndex);
    static PeerMessage createRequest(uint32_t piece, uint32_t offset, uint32_t length);
    static PeerMessage createCancel(uint32_t piece, uint32_t offset, uint32_t length);
};

class PeerConnection {
public:
    PeerConnection(const std::string& ip, uint16_t port, const TorrentInfo& torrent, const std::string& peerId);
    ~PeerConnection();
    
    bool connect();
    void disconnect();
    bool isConnected() const { return connected_; }
    
    // Send messages
    bool sendChoke();
    bool sendUnchoke();
    bool sendInterested();
    bool sendHave(uint32_t pieceIndex);
    bool sendRequest(uint32_t piece, uint32_t offset, uint32_t length);
    bool sendCancel(uint32_t piece, uint32_t offset, uint32_t length);
    
    // Receive messages
    bool receiveMessage(PeerMessage& msg);
    bool receiveMessageNonBlocking(PeerMessage& msg);

    // Return peers learned from ut_pex since the previous call.
    std::vector<Peer> takeDiscoveredPeers();

    // Read a block (piece data)
    bool readBlock(uint32_t piece, uint32_t offset, uint32_t length, std::vector<uint8_t>& data);
    
    // State
    bool amChoking() const { return amChoking_; }
    bool amInterested() const { return amInterested_; }
    bool peerChoking() const { return peerChoking_; }
    bool peerInterested() const { return peerInterested_; }
    
    const std::vector<bool>& bitfield() const { return bitfield_; }
    bool hasPiece(uint32_t index) const;
    
    void setAmChoking(bool choke) { amChoking_ = choke; }
    void setAmInterested(bool interested) { amInterested_ = interested; }
    
    const std::string& ip() const { return ip_; }
    uint16_t port() const { return port_; }
    
private:
    bool performHandshake();
    bool sendMessage(const PeerMessage& msg);
    bool sendExtensionHandshake();
    void processExtendedMessage(const std::vector<uint8_t>& payload);
    void updateBitfield(const std::vector<uint8_t>& data);
    
    std::string ip_;
    uint16_t port_;
    TorrentInfo torrent_;
    std::string peerId_;
    
    int socket_ = -1;
    bool connected_ = false;
    
    // Peer state
    bool amChoking_ = true;
    bool amInterested_ = false;
    bool peerChoking_ = true;
    bool peerInterested_ = false;
    bool peerSupportsExtensions_ = false;
    bool peerSupportsPex_ = false;
    
    std::vector<bool> bitfield_;
    std::vector<Peer> discoveredPeers_;
};

#endif // PEER_H
