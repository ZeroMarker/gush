#ifndef DHT_H
#define DHT_H

#include "tracker.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DhtEndpoint {
    std::string host;
    uint16_t port = 6881;
};

struct DhtOptions {
    int queryTimeoutMs = 1000;
    std::size_t maxQueries = 32;
    std::size_t maxPeers = 100;
};

// BEP 5 get_peers discovery. The implementation is intentionally bounded and
// IPv4-only; returned peers use the same compact representation as trackers.
std::vector<Peer> discoverDhtPeers(
    const std::string& infoHash,
    const std::vector<DhtEndpoint>& bootstrapNodes = {},
    const DhtOptions& options = DhtOptions());

#endif // DHT_H
