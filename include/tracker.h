#ifndef TRACKER_H
#define TRACKER_H

#include "torrent.h"
#include <string>
#include <vector>
#include <cstdint>

struct Peer {
    std::string ip;
    uint16_t port;
    std::string peerId;

    bool operator==(const Peer& other) const {
        return ip == other.ip && port == other.port;
    }

    bool operator!=(const Peer& other) const {
        return !(*this == other);
    }
};

struct TrackerResponse {
    int interval = 0;
    int minInterval = 0;
    int64_t complete = 0;  // Seeders
    int64_t incomplete = 0;  // Leechers
    std::string warning;
    std::string failure;
    std::vector<Peer> peers;

    bool ok() const { return failure.empty(); }
};

// Contact tracker and get peer list
TrackerResponse contactTracker(
    const std::string& trackerUrl,
    const TorrentInfo& torrent,
    const std::string& peerId,
    int64_t downloaded = 0,
    int64_t uploaded = 0,
    int64_t left = 0,
    const std::string& event = ""
);

// Parse compact peer list (binary format)
std::vector<Peer> parseCompactPeers(const std::string& data);

// Parse non-compact peer list (bencoded)
std::vector<Peer> parsePeers(const bencode::BencodeValue& peers);

#endif // TRACKER_H
