#include <gtest/gtest.h>
#include "dht.h"
#include "bencode.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

namespace {

class UdpNode {
public:
    bool start(std::function<std::string(const bencode::BencodeValue&)> responder) {
        socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ < 0) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            return false;
        }
        socklen_t length = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            return false;
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this, responder = std::move(responder)] {
            char buffer[4096];
            sockaddr_in client{};
            socklen_t clientLength = sizeof(client);
            ssize_t size = recvfrom(socket_, buffer, sizeof(buffer), 0,
                                    reinterpret_cast<sockaddr*>(&client), &clientLength);
            if (size <= 0) return;
            try {
                auto request = bencode::parse(std::string(buffer, size));
                std::string response = responder(request);
                sendto(socket_, response.data(), response.size(), 0,
                       reinterpret_cast<sockaddr*>(&client), clientLength);
                handled_ = true;
            } catch (...) {
            }
        });
        return true;
    }

    uint16_t port() const { return port_; }
    bool handled() const { return handled_; }

    void join() {
        if (thread_.joinable()) thread_.join();
        if (socket_ >= 0) {
            close(socket_);
            socket_ = -1;
        }
    }

    ~UdpNode() { join(); }

private:
    int socket_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> handled_{false};
    std::thread thread_;
};

std::string transactionFrom(const bencode::BencodeValue& request) {
    if (!request.isDict()) return {};
    auto transaction = bencode::dictGetString(request.asDict(), "t");
    return transaction.value_or("");
}

std::string responseWith(const std::string& transaction,
                         bencode::BencodeDict responseBody) {
    responseBody["id"] = std::string(20, 'R');
    bencode::BencodeDict root;
    root["r"] = std::move(responseBody);
    root["t"] = transaction;
    root["y"] = bencode::BencodeString("r");
    return bencode::encode(bencode::BencodeValue(std::move(root)));
}

std::string compactNode(uint16_t port) {
    std::string result(20, 'N');
    const uint8_t address[] = {127, 0, 0, 1,
                               static_cast<uint8_t>(port >> 8),
                               static_cast<uint8_t>(port & 0xff)};
    result.append(reinterpret_cast<const char*>(address), sizeof(address));
    return result;
}

std::string compactPeer(const char a, const char b, const char c, const char d,
                        uint16_t port) {
    const uint8_t bytes[] = {static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                             static_cast<uint8_t>(c), static_cast<uint8_t>(d),
                             static_cast<uint8_t>(port >> 8),
                             static_cast<uint8_t>(port & 0xff)};
    return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

} // namespace

TEST(DhtTest, FollowsCompactNodesAndReturnsPeers) {
    const std::string infoHash(20, 'H');
    UdpNode leaf;
    ASSERT_TRUE(leaf.start([&](const bencode::BencodeValue& request) {
        const auto* args = bencode::dictGet(request.asDict(), "a");
        EXPECT_NE(args, nullptr);
        EXPECT_TRUE(args && args->isDict());
        if (args && args->isDict()) {
            EXPECT_EQ(bencode::dictGetString(args->asDict(), "info_hash"), infoHash);
        }

        bencode::BencodeList values;
        values.emplace_back(compactPeer(10, 20, 30, 40, 6881));
        values.emplace_back(compactPeer(10, 20, 30, 40, 6881)); // duplicate
        values.emplace_back(compactPeer(50, 60, 70, 80, 51413));
        bencode::BencodeDict body;
        body["values"] = std::move(values);
        return responseWith(transactionFrom(request), std::move(body));
    }));

    UdpNode bootstrap;
    ASSERT_TRUE(bootstrap.start([&](const bencode::BencodeValue& request) {
        EXPECT_EQ(bencode::dictGetString(request.asDict(), "q"), "get_peers");
        bencode::BencodeDict body;
        body["nodes"] = compactNode(leaf.port());
        return responseWith(transactionFrom(request), std::move(body));
    }));

    DhtOptions options;
    options.queryTimeoutMs = 500;
    options.maxQueries = 4;
    options.maxPeers = 10;
    auto peers = discoverDhtPeers(infoHash, {{"127.0.0.1", bootstrap.port()}}, options);

    bootstrap.join();
    leaf.join();
    EXPECT_TRUE(bootstrap.handled());
    EXPECT_TRUE(leaf.handled());
    ASSERT_EQ(peers.size(), 2u);
    EXPECT_EQ(peers[0].ip, "10.20.30.40");
    EXPECT_EQ(peers[0].port, 6881);
    EXPECT_EQ(peers[1].ip, "50.60.70.80");
    EXPECT_EQ(peers[1].port, 51413);
}

TEST(DhtTest, RejectsInvalidInfoHashWithoutNetworkAccess) {
    EXPECT_TRUE(discoverDhtPeers("too short", {{"127.0.0.1", 1}}).empty());
}
