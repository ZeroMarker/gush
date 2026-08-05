#include "tracker.h"
#include "utils.h"
#include "bencode.h"
#include <curl/curl.h>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <random>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>

// Byte order conversion macros (for compatibility)
#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#define htobe64(x) htonll(x)
#define be64toh(x) ntohll(x)
#define htobe32(x) htonl(x)
#define be32toh(x) ntohl(x)
#define htobe16(x) htons(x)
#define be16toh(x) ntohs(x)
#else
#include <endian.h>
#endif

// UDP Tracker Protocol (BEP 15)
namespace udp_tracker {

#pragma pack(push, 1)
struct ConnectRequest {
    uint64_t connection_id;
    uint32_t action;
    uint32_t transaction_id;
};

struct ConnectResponse {
    uint32_t action;
    uint32_t transaction_id;
    uint64_t connection_id;
};

struct AnnounceRequest {
    uint64_t connection_id;
    uint32_t action;
    uint32_t transaction_id;
    uint8_t info_hash[20];
    uint8_t peer_id[20];
    uint64_t downloaded;
    uint64_t left;
    uint64_t uploaded;
    uint32_t event;
    uint32_t ip;
    uint32_t key;
    uint32_t num_want;
    uint16_t port;
};

struct AnnounceResponse {
    uint32_t action;
    uint32_t transaction_id;
    uint32_t interval;
    uint32_t leechers;
    uint32_t seeders;
};
#pragma pack(pop)

static const uint64_t CONNECTION_ID = 0x41727101980ULL;

} // namespace udp_tracker

static size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t totalSize = size * nmemb;
    userp->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::vector<Peer> parseCompactPeers(const std::string& data) {
    std::vector<Peer> peers;
    
    // Compact format: 6 bytes per peer (4 bytes IP + 2 bytes port)
    for (size_t i = 0; i + 6 <= data.size(); i += 6) {
        Peer peer;
        
        // IP address (4 bytes, big-endian)
        uint8_t ip[4];
        for (int j = 0; j < 4; j++) {
            ip[j] = static_cast<uint8_t>(data[i + j]);
        }
        peer.ip = std::to_string(ip[0]) + "." + 
                  std::to_string(ip[1]) + "." + 
                  std::to_string(ip[2]) + "." + 
                  std::to_string(ip[3]);
        
        // Port (2 bytes, big-endian)
        peer.port = (static_cast<uint8_t>(data[i + 4]) << 8) | 
                     static_cast<uint8_t>(data[i + 5]);
        
        peers.push_back(peer);
    }
    
    return peers;
}

std::vector<Peer> parsePeers(const bencode::BencodeValue& peers) {
    std::vector<Peer> result;
    
    if (peers.isList()) {
        for (const auto& peerVal : peers.asList()) {
            if (!peerVal.isDict()) continue;
            
            const auto& peerDict = peerVal.asDict();
            Peer peer;
            
            const auto* ip = bencode::dictGet(peerDict, "ip");
            if (ip && ip->isString()) {
                peer.ip = std::get<bencode::BencodeString>(ip->data);
            }
            
            const auto* port = bencode::dictGet(peerDict, "port");
            if (port && port->isInt()) {
                peer.port = static_cast<uint16_t>(port->asInt());
            }
            
            const auto* peerId = bencode::dictGet(peerDict, "peer id");
            if (peerId && peerId->isString()) {
                peer.peerId = std::get<bencode::BencodeString>(peerId->data);
            }
            
            if (!peer.ip.empty() && peer.port > 0) {
                result.push_back(peer);
            }
        }
    }
    
    return result;
}

// UDP Tracker support (BEP 15)
namespace {

// Return the raw 20-byte info hash, accepting both conventions:
//  - raw 20-byte SHA1 (as produced by TorrentInfo after loadTorrent)
//  - 40-char hex string (as produced by magnet parsing / tests)
std::string rawInfoHash(const TorrentInfo& torrent) {
    if (torrent.infoHash.size() == 40) {
        std::string raw = utils::fromHex(torrent.infoHash);
        if (raw.size() == 20) return raw;
    }
    if (torrent.infoHash.size() == 20) return torrent.infoHash;
    // Last resort: attempt hex decoding (empty on failure)
    return utils::fromHex(torrent.infoHash);
}

bool parseUdpUrl(const std::string& url, std::string& host, uint16_t& port) {
    // Format: udp://host:port/path
    if (url.substr(0, 6) != "udp://") return false;
    
    size_t portStart = url.find(':', 6);
    if (portStart == std::string::npos) return false;
    
    host = url.substr(6, portStart - 6);
    
    size_t pathStart = url.find('/', portStart + 1);
    std::string portStr = (pathStart == std::string::npos) ? 
                          url.substr(portStart + 1) : 
                          url.substr(portStart + 1, pathStart - portStart - 1);
    
    try {
        port = static_cast<uint16_t>(std::stoi(portStr));
    } catch (...) {
        return false;
    }
    
    return true;
}

TrackerResponse contactUdpTracker(
    const std::string& trackerUrl,
    const TorrentInfo& torrent,
    const std::string& peerId,
    int64_t downloaded,
    int64_t uploaded,
    int64_t left
) {
    TrackerResponse response;
    response.interval = 1800;
    
    std::string host;
    uint16_t port;
    if (!parseUdpUrl(trackerUrl, host, port)) {
        response.failure = "Invalid UDP tracker URL";
        return response;
    }
    
    // Resolve hostname
    struct addrinfo hints = {}, *result;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0) {
        response.failure = "Failed to resolve tracker hostname";
        return response;
    }
    
    struct sockaddr_in addr = *reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
    freeaddrinfo(result);
    
    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        response.failure = "Failed to create socket";
        return response;
    }
    
    // Set timeout
    struct timeval tv;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    std::random_device rd;
    uint32_t transactionId = rd();
    
    // Send connect request
    udp_tracker::ConnectRequest connectReq;
    connectReq.connection_id = htobe64(udp_tracker::CONNECTION_ID);
    connectReq.action = htobe32(0);  // Connect
    connectReq.transaction_id = htobe32(transactionId);
    
    if (sendto(sock, &connectReq, sizeof(connectReq), 0, 
               reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != sizeof(connectReq)) {
        close(sock);
        response.failure = "Failed to send connect request";
        return response;
    }
    
    // Receive connect response
    udp_tracker::ConnectResponse connectResp;
    if (recv(sock, &connectResp, sizeof(connectResp), 0) != sizeof(connectResp)) {
        close(sock);
        response.failure = "Failed to receive connect response";
        return response;
    }
    
    if (be32toh(connectResp.action) != 0 || 
        be32toh(connectResp.transaction_id) != transactionId) {
        close(sock);
        response.failure = "Invalid connect response";
        return response;
    }
    
    uint64_t connectionId = be64toh(connectResp.connection_id);

    // Send announce request
    udp_tracker::AnnounceRequest announceReq;
    announceReq.connection_id = htobe64(connectionId);
    announceReq.action = htobe32(1);  // Announce
    announceReq.transaction_id = htobe32(transactionId);
    const std::string infoHash = rawInfoHash(torrent);
    if (infoHash.size() != 20 || peerId.size() != 20) {
        close(sock);
        response.failure = "Invalid info hash or peer ID";
        return response;
    }
    memcpy(announceReq.info_hash, infoHash.c_str(), 20);
    memcpy(announceReq.peer_id, peerId.c_str(), 20);
    announceReq.downloaded = htobe64(downloaded);
    announceReq.left = htobe64(left);
    announceReq.uploaded = htobe64(uploaded);
    announceReq.event = htobe32(0);  // None
    announceReq.ip = 0;
    announceReq.key = htobe32(rd());
    announceReq.num_want = htobe32(200);
    announceReq.port = htobe16(6881);
    
    if (sendto(sock, &announceReq, sizeof(announceReq), 0,
               reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != sizeof(announceReq)) {
        close(sock);
        response.failure = "Failed to send announce";
        return response;
    }
    
    // Receive announce response
    udp_tracker::AnnounceResponse announceResp;
    if (recv(sock, &announceResp, sizeof(announceResp), 0) != sizeof(announceResp)) {
        close(sock);
        response.failure = "Failed to receive announce response";
        return response;
    }
    
    if (be32toh(announceResp.action) != 1 ||
        be32toh(announceResp.transaction_id) != transactionId) {
        close(sock);
        response.failure = "Invalid announce response";
        return response;
    }
    
    response.interval = be32toh(announceResp.interval);
    response.incomplete = be32toh(announceResp.leechers);
    response.complete = be32toh(announceResp.seeders);
    
    // Read peers (6 bytes each: 4 IP + 2 port)
    std::vector<uint8_t> peerData(6 * 200);  // Max 200 peers
    ssize_t peerBytes = recv(sock, peerData.data(), peerData.size(), MSG_DONTWAIT);
    
    for (ssize_t i = 0; i + 6 <= peerBytes; i += 6) {
        Peer peer;
        peer.ip = std::to_string(peerData[i]) + "." +
                  std::to_string(peerData[i+1]) + "." +
                  std::to_string(peerData[i+2]) + "." +
                  std::to_string(peerData[i+3]);
        peer.port = (peerData[i+4] << 8) | peerData[i+5];
        if (peer.port > 0) {
            response.peers.push_back(peer);
        }
    }
    
    close(sock);
    return response;
}

} // anonymous namespace

TrackerResponse contactTracker(
    const std::string& trackerUrl,
    const TorrentInfo& torrent,
    const std::string& peerId,
    int64_t downloaded,
    int64_t uploaded,
    int64_t left,
    const std::string& event
) {
    TrackerResponse response;
    response.interval = 1800;  // Default 30 minutes
    response.minInterval = 900;  // Default 15 minutes

    // Check if UDP tracker
    if (trackerUrl.substr(0, 6) == "udp://") {
        return contactUdpTracker(trackerUrl, torrent, peerId, downloaded, uploaded, left);
    }

    // HTTP/HTTPS tracker
    // Build tracker URL
    std::ostringstream url;
    url << trackerUrl;

    // Check if URL already has query parameters
    bool hasQuery = trackerUrl.find('?') != std::string::npos;
    url << (hasQuery ? '&' : '?');

    // Add required parameters
    // Note: info_hash must be the raw 20-byte binary data, URL-encoded
    std::string infoHashBytes = rawInfoHash(torrent);
    if (infoHashBytes.size() != 20 || peerId.size() != 20) {
        response.failure = "Invalid info hash or peer ID";
        return response;
    }
    url << "info_hash=" << utils::urlEncode(infoHashBytes);
    url << "&peer_id=" << utils::urlEncode(peerId);
    url << "&port=6881";
    url << "&uploaded=" << uploaded;
    url << "&downloaded=" << downloaded;
    url << "&left=" << left;
    url << "&compact=1";
    url << "&numwant=500";  // Increased to get more peers from tracker

    if (!event.empty()) {
        url << "&event=" << utils::urlEncode(event);
    }

    // Initialize CURL
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.failure = "Failed to initialize CURL";
        return response;
    }

    std::string responseData;

    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);  // Increased redirect limit
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);    // Bounded so shutdown stays responsive
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);  // Connection timeout
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseData);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Gush/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // Skip SSL verification for trackers

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        response.failure = std::string("Tracker request failed: ") + curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return response;
    }

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (httpCode != 200) {
        response.failure = "HTTP error: " + std::to_string(httpCode);
        return response;
    }

    // Parse response
    try {
        bencode::BencodeValue root = bencode::parse(responseData);
        if (!root.isDict()) {
            response.failure = "Invalid tracker response: not a dictionary";
            return response;
        }

        const auto& dict = root.asDict();
        
        // Check for failure
        const auto* failure = bencode::dictGet(dict, "failure reason");
        if (failure && failure->isString()) {
            response.failure = std::get<bencode::BencodeString>(failure->data);
            return response;
        }
        
        // Get warning (optional)
        const auto* warning = bencode::dictGet(dict, "warning message");
        if (warning && warning->isString()) {
            response.warning = std::get<bencode::BencodeString>(warning->data);
        }
        
        // Get interval
        const auto* interval = bencode::dictGet(dict, "interval");
        if (interval && interval->isInt()) {
            response.interval = static_cast<int>(interval->asInt());
        }
        
        // Get min interval
        const auto* minInterval = bencode::dictGet(dict, "min interval");
        if (minInterval && minInterval->isInt()) {
            response.minInterval = static_cast<int>(minInterval->asInt());
        }
        
        // Get complete (seeders)
        const auto* complete = bencode::dictGet(dict, "complete");
        if (complete && complete->isInt()) {
            response.complete = complete->asInt();
        }
        
        // Get incomplete (leechers)
        const auto* incomplete = bencode::dictGet(dict, "incomplete");
        if (incomplete && incomplete->isInt()) {
            response.incomplete = incomplete->asInt();
        }
        
        // Get peers
        const auto* peersVal = bencode::dictGet(dict, "peers");
        if (peersVal) {
            if (peersVal->isString()) {
                // Compact format
                response.peers = parseCompactPeers(std::get<bencode::BencodeString>(peersVal->data));
            } else if (peersVal->isList()) {
                // Non-compact format
                response.peers = parsePeers(*peersVal);
            }
        }
        
    } catch (const std::exception& e) {
        response.failure = std::string("Failed to parse tracker response: ") + e.what();
    }
    
    return response;
}
