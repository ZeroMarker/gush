#ifndef METADATA_H
#define METADATA_H

#include "magnet.h"
#include "peer.h"
#include <string>
#include <vector>

// BEP 9 - Metadata Extension
// Extension message ID for ut_metadata
const int METADATA_EXTENSION_ID = 1;

// Metadata message types
enum class MetadataMsgType : uint8_t {
    Request = 0,
    Data = 1,
    Reject = 2
};

struct MetadataMessage {
    MetadataMsgType type;
    int piece;
    std::vector<uint8_t> data;
    
    static MetadataMessage createRequest(int pieceIndex);
    std::vector<uint8_t> encode() const;
    static MetadataMessage decode(const std::vector<uint8_t>& data);
};

class MetadataDownloader {
public:
    MetadataDownloader(const MagnetLink& magnet);
    
    // Start metadata download
    bool start();
    
    // Stop metadata download
    void stop();
    
    // Check if complete
    bool isComplete() const { return complete_; }
    
    // Get torrent info (after download)
    std::string getTorrentData() const { return metadata_; }
    
    // Add peer for metadata download
    void addPeer(const std::string& ip, uint16_t port);
    
private:
    bool downloadFromPeer(const std::string& ip, uint16_t port);
    bool performHandshake(int socket);
    bool exchangeExtensions(int socket, int& metadataExtId);
    bool requestMetadata(int socket, int metadataExtId);
    
    MagnetLink magnet_;
    std::string metadata_;
    bool complete_ = false;
    
    int metadataSize_ = 0;
    int numPieces_ = 0;
    std::vector<std::vector<uint8_t>> pieces_;
};

// Download metadata from magnet link
std::string downloadMetadata(const MagnetLink& magnet);

#endif // METADATA_H
