#include "dht.h"
#include "bencode.h"
#include "utils.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <deque>
#include <random>
#include <set>

namespace {

constexpr std::size_t NODE_COMPACT_SIZE = 26;
constexpr std::size_t MAX_DHT_PACKET_SIZE = 64 * 1024;

struct ResolvedNode {
    sockaddr_in address{};
    std::string key;
};

std::string randomBytes(std::size_t size) {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 255);
    std::string result(size, '\0');
    for (char& c : result) c = static_cast<char>(dist(rng));
    return result;
}

bool resolveNode(const DhtEndpoint& node, ResolvedNode& result) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* addresses = nullptr;
    const std::string port = std::to_string(node.port);
    if (getaddrinfo(node.host.c_str(), port.c_str(), &hints, &addresses) != 0) {
        return false;
    }

    bool ok = false;
    for (addrinfo* current = addresses; current; current = current->ai_next) {
        if (current->ai_addrlen != sizeof(sockaddr_in)) continue;
        std::memcpy(&result.address, current->ai_addr, sizeof(sockaddr_in));
        char ip[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &result.address.sin_addr, ip, sizeof(ip))) continue;
        result.key = std::string(ip) + ":" + std::to_string(ntohs(result.address.sin_port));
        ok = true;
        break;
    }
    freeaddrinfo(addresses);
    return ok;
}

std::string makeGetPeersQuery(const std::string& transaction,
                              const std::string& nodeId,
                              const std::string& infoHash) {
    bencode::BencodeDict args;
    args["id"] = nodeId;
    args["info_hash"] = infoHash;
    bencode::BencodeDict query;
    query["a"] = std::move(args);
    query["q"] = bencode::BencodeString("get_peers");
    query["t"] = transaction;
    query["y"] = bencode::BencodeString("q");
    return bencode::encode(bencode::BencodeValue(std::move(query)));
}

bool queryNode(int socketFd, const ResolvedNode& node, const std::string& query,
               const std::string& transaction, int timeoutMs,
               bencode::BencodeValue& response) {
    ssize_t sent = sendto(socketFd, query.data(), query.size(), 0,
                          reinterpret_cast<const sockaddr*>(&node.address),
                          sizeof(node.address));
    if (sent != static_cast<ssize_t>(query.size())) return false;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
        timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(socketFd, &fds);
        int ready = select(socketFd + 1, &fds, nullptr, nullptr, &timeout);
        if (ready <= 0) return false;

        std::array<char, MAX_DHT_PACKET_SIZE> buffer{};
        sockaddr_in sender{};
        socklen_t senderLength = sizeof(sender);
        ssize_t received = recvfrom(socketFd, buffer.data(), buffer.size(), 0,
                                    reinterpret_cast<sockaddr*>(&sender), &senderLength);
        if (received <= 0) continue;
        if (sender.sin_addr.s_addr != node.address.sin_addr.s_addr ||
            sender.sin_port != node.address.sin_port) {
            continue;
        }
        try {
            auto decoded = bencode::parse(std::string(buffer.data(), received));
            if (!decoded.isDict()) continue;
            const auto replyTransaction = bencode::dictGetString(decoded.asDict(), "t");
            const auto type = bencode::dictGetString(decoded.asDict(), "y");
            if (!replyTransaction || *replyTransaction != transaction || !type || *type != "r") {
                continue;
            }
            response = std::move(decoded);
            return true;
        } catch (...) {
            // Ignore malformed or unrelated UDP datagrams until timeout.
        }
    }
    return false;
}

void enqueueCompactNodes(const std::string& nodes, std::deque<ResolvedNode>& pending,
                         std::set<std::string>& known) {
    for (std::size_t offset = 0; offset + NODE_COMPACT_SIZE <= nodes.size();
         offset += NODE_COMPACT_SIZE) {
        ResolvedNode node;
        node.address.sin_family = AF_INET;
        std::memcpy(&node.address.sin_addr, nodes.data() + offset + 20, 4);
        uint16_t port = 0;
        std::memcpy(&port, nodes.data() + offset + 24, 2);
        node.address.sin_port = port;
        if (node.address.sin_addr.s_addr == 0 || node.address.sin_port == 0) continue;
        char ip[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &node.address.sin_addr, ip, sizeof(ip))) continue;
        node.key = std::string(ip) + ":" + std::to_string(ntohs(node.address.sin_port));
        if (known.insert(node.key).second) pending.push_back(std::move(node));
    }
}

void appendPeers(const bencode::BencodeValue& values, std::vector<Peer>& peers,
                 std::set<std::string>& knownPeers, std::size_t maxPeers) {
    if (!values.isList()) return;
    for (const auto& value : values.asList()) {
        if (!value.isString()) continue;
        for (const auto& peer : parseCompactPeers(value.asString())) {
            if (peer.port == 0) continue;
            const std::string key = peer.ip + ":" + std::to_string(peer.port);
            if (knownPeers.insert(key).second) peers.push_back(peer);
            if (peers.size() >= maxPeers) return;
        }
    }
}

} // namespace

std::vector<Peer> discoverDhtPeers(const std::string& infoHash,
                                   const std::vector<DhtEndpoint>& bootstrapNodes,
                                   const DhtOptions& options) {
    if (infoHash.size() != 20 || options.maxQueries == 0 || options.maxPeers == 0) return {};

    std::vector<DhtEndpoint> seeds = bootstrapNodes;
    if (seeds.empty()) {
        seeds = {{"router.bittorrent.com", 6881},
                 {"router.utorrent.com", 6881},
                 {"dht.transmissionbt.com", 6881}};
    }

    std::deque<ResolvedNode> pending;
    std::set<std::string> knownNodes;
    for (const auto& seed : seeds) {
        ResolvedNode node;
        if (resolveNode(seed, node) && knownNodes.insert(node.key).second) {
            pending.push_back(std::move(node));
        }
    }
    if (pending.empty()) return {};

    int socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd < 0) return {};

    const std::string nodeId = randomBytes(20);
    std::vector<Peer> peers;
    std::set<std::string> knownPeers;
    std::size_t queryCount = 0;
    while (!pending.empty() && queryCount < options.maxQueries &&
           peers.size() < options.maxPeers) {
        ResolvedNode node = std::move(pending.front());
        pending.pop_front();
        const std::string transaction = randomBytes(2);
        const std::string query = makeGetPeersQuery(transaction, nodeId, infoHash);
        bencode::BencodeValue response;
        ++queryCount;
        if (!queryNode(socketFd, node, query, transaction,
                       std::max(1, options.queryTimeoutMs), response)) continue;

        const auto* body = bencode::dictGet(response.asDict(), "r");
        if (!body || !body->isDict()) continue;
        const auto* values = bencode::dictGet(body->asDict(), "values");
        if (values) appendPeers(*values, peers, knownPeers, options.maxPeers);
        const auto nodes = bencode::dictGetString(body->asDict(), "nodes");
        if (nodes) enqueueCompactNodes(*nodes, pending, knownNodes);
    }

    close(socketFd);
    return peers;
}
