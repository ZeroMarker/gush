#include "metadata.h"
#include "tracker.h"
#include "tracker_list.h"
#include "bencode.h"
#include "utils.h"
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <cerrno>

// BEP 10: the extension protocol uses peer wire message ID 20
static const uint8_t EXTENDED_MESSAGE_ID = 20;
// BEP 10: the extended handshake itself uses extended message ID 0
static const uint8_t EXTENDED_HANDSHAKE_ID = 0;
// BEP 9/10: ut_metadata block size
static const int METADATA_PIECE_SIZE = 16384;

namespace {

// Robust full send/recv: stream sockets may return partial reads/writes.
bool sendFull(int sock, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        // MSG_NOSIGNAL: a peer disconnect must not kill the process via SIGPIPE
        ssize_t n = send(sock, data + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recvFull(int sock, uint8_t* data, size_t size) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = recv(sock, data + got, size - got, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

// Read the next extended message with the given extension ID, transparently
// skipping keep-alives and non-extended messages (choke/unchoke/bitfield/have...).
// Returns false on socket error, or if no matching message arrives in time
// (SO_RCVTIMEO still applies to the underlying recv).
bool readExtendedMessage(int sock, uint8_t expectedExtId, std::vector<uint8_t>& payload) {
    for (int i = 0; i < 4096; ++i) {
        uint8_t lenBuf[4];
        if (!recvFull(sock, lenBuf, 4)) return false;

        uint32_t len = (static_cast<uint32_t>(lenBuf[0]) << 24) |
                       (static_cast<uint32_t>(lenBuf[1]) << 16) |
                       (static_cast<uint32_t>(lenBuf[2]) << 8) |
                       static_cast<uint32_t>(lenBuf[3]);

        if (len == 0) {
            continue;  // keep-alive
        }
        if (len > 4 * 1024 * 1024) {
            return false;  // absurd message size
        }

        uint8_t id = 0;
        if (!recvFull(sock, &id, 1)) return false;

        if (id != EXTENDED_MESSAGE_ID) {
            // Non-extended message: skip its payload
            std::vector<uint8_t> skip(len - 1);
            if (!recvFull(sock, skip.data(), skip.size())) return false;
            continue;
        }

        uint8_t extId = 0;
        if (!recvFull(sock, &extId, 1)) return false;

        payload.resize(len - 2);
        if (!recvFull(sock, payload.data(), payload.size())) return false;

        if (extId == expectedExtId) {
            return true;
        }
        // Extended message for another extension: already consumed, keep looking
    }
    return false;  // Gave up after too many unrelated messages
}

} // anonymous namespace

MetadataMessage MetadataMessage::createRequest(int pieceIndex) {
    MetadataMessage msg;
    msg.type = MetadataMsgType::Request;
    msg.piece = pieceIndex;
    return msg;
}

std::vector<uint8_t> MetadataMessage::encode() const {
    // Create bencoded dictionary
    bencode::BencodeDict dict;
    dict["msg_type"] = static_cast<int64_t>(static_cast<uint8_t>(type));
    dict["piece"] = static_cast<int64_t>(piece);

    std::string bencoded = bencode::encode(bencode::BencodeValue(dict));

    std::vector<uint8_t> result(bencoded.size());
    std::copy(bencoded.begin(), bencoded.end(), result.begin());
    return result;
}

MetadataMessage MetadataMessage::decode(const std::vector<uint8_t>& data) {
    MetadataMessage msg;
    msg.type = MetadataMsgType::Request;
    msg.piece = 0;

    try {
        std::string bencoded(data.begin(), data.end());
        bencode::BencodeValue value = bencode::parse(bencoded);

        if (value.isDict()) {
            const auto& dict = value.asDict();
            const auto* msgType = bencode::dictGet(dict, "msg_type");
            const auto* pieceVal = bencode::dictGet(dict, "piece");

            if (msgType && msgType->isInt()) {
                msg.type = static_cast<MetadataMsgType>(static_cast<uint8_t>(msgType->asInt()));
            }
            if (pieceVal && pieceVal->isInt()) {
                msg.piece = static_cast<int>(pieceVal->asInt());
            }
        }
    } catch (...) {
        // Ignore parse errors
    }

    return msg;
}

MetadataDownloader::MetadataDownloader(const MagnetLink& magnet)
    : magnet_(magnet) {
}

// Fetch built-in tracker list from online sources
static std::vector<std::string> getBuiltinTrackers() {
    std::cout << "Fetching latest tracker list from online sources..." << std::endl;

    // Fetch from multiple sources
    std::vector<std::string> trackers = TrackerList::fetchTrackersFromMultipleSources({
        TrackerList::Source::TRACKERSLIST_ALL,
        TrackerList::Source::NGOSANG_ALL
    });

    // If fetching failed, fall back to hardcoded list
    if (trackers.empty()) {
        std::cerr << "Warning: Failed to fetch online tracker list, using fallback list" << std::endl;
        return {
            "udp://tracker.opentrackr.org:1337/announce",
            "udp://open.tracker.cl:1337/announce",
            "udp://tracker.openbittorrent.com:6969/announce",
            "udp://tracker.torrent.eu.org:451/announce",
            "udp://tracker.bittor.pw:1337/announce",
            "udp://tracker.dler.org:6969/announce",
            "udp://tracker.moeking.me:6969/announce",
            "udp://tracker.tiny-vps.com:6969/announce",
            "udp://tracker.port443.xyz:6969/announce",
            "udp://open.stealth.si:80/announce",
            "udp://exodus.desync.com:6969/announce",
            "udp://tracker.cyberia.is:6969/announce",
            "udp://tracker.internetwarriors.net:1337/announce",
            "udp://tracker.skynetcloud.site:6969/announce",
            "http://tracker.opentrackr.org:1337/announce",
            "http://tracker.openbittorrent.com:6969/announce",
            "http://tracker.tfile.co/announce",
            "http://bt1.archive.org:6969/announce",
            "http://bt2.archive.org:6969/announce",
        };
    }

    std::cout << "Loaded " << trackers.size() << " trackers from online sources" << std::endl;
    return trackers;
}

bool MetadataDownloader::start() {
    // Use provided trackers or fall back to built-in trackers
    std::vector<std::string> trackersToUse = magnet_.trackers;

    if (trackersToUse.empty()) {
        std::cout << "No trackers in magnet link, fetching latest tracker list" << std::endl;
        trackersToUse = getBuiltinTrackers();
    }

    // Prefer UDP trackers: they fail fast and are usually more reliable for metadata
    std::stable_partition(trackersToUse.begin(), trackersToUse.end(),
        [](const std::string& url) { return url.rfind("udp://", 0) == 0; });

    // Create a temporary TorrentInfo for tracker contact
    TorrentInfo tempTorrent;
    tempTorrent.infoHash = magnet_.infoHashRaw;
    tempTorrent.announceList = trackersToUse;
    tempTorrent.peerId = "-GT0001-" + std::string(12, 'M');

    // Contact each tracker to get peers (bounded: avoid spending minutes on dead trackers)
    std::vector<Peer> allPeers;
    const size_t MAX_TRACKERS_TO_TRY = 25;
    const size_t MAX_PEERS = 50;
    for (size_t i = 0; i < trackersToUse.size() && i < MAX_TRACKERS_TO_TRY; ++i) {
        const std::string& trackerUrl = trackersToUse[i];
        std::cout << "Contacting tracker: " << trackerUrl << std::endl;
        TrackerResponse response = contactTracker(trackerUrl, tempTorrent, tempTorrent.peerId);

        if (response.ok()) {
            std::cout << "  Got " << response.peers.size() << " peers from tracker" << std::endl;
            allPeers.insert(allPeers.end(), response.peers.begin(), response.peers.end());
        } else {
            std::cout << "  Tracker failed: " << response.failure << std::endl;
        }

        // Stop if we have enough peers
        if (allPeers.size() >= MAX_PEERS) break;
    }

    if (allPeers.empty()) {
        std::cerr << "No peers available from any tracker" << std::endl;
        std::cerr << "Note: Pure hash magnet links may need DHT for peer discovery" << std::endl;
        return false;
    }

    std::cout << "Total peers available: " << allPeers.size() << std::endl;

    // Try to download metadata from each peer
    for (const auto& peer : allPeers) {
        std::cout << "Trying peer: " << peer.ip << ":" << peer.port << std::endl;
        if (downloadFromPeer(peer.ip, peer.port)) {
            std::cout << "Successfully downloaded metadata from peer" << std::endl;
            if (complete_) break;
        }
    }

    if (!complete_) {
        std::cerr << "Failed to download complete metadata from all peers" << std::endl;
        return false;
    }

    return true;
}

void MetadataDownloader::stop() {
    complete_ = false;
}

void MetadataDownloader::addPeer(const std::string& ip, uint16_t port) {
    if (!complete_) {
        downloadFromPeer(ip, port);
    }
}

bool MetadataDownloader::downloadFromPeer(const std::string& ip, uint16_t port) {
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    // Set timeout
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Connect
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        close(sock);
        return false;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    // Perform BitTorrent handshake
    if (!performHandshake(sock)) {
        close(sock);
        return false;
    }

    // Exchange extensions
    int metadataExtId = -1;
    if (!exchangeExtensions(sock, metadataExtId)) {
        close(sock);
        return false;
    }

    if (metadataExtId < 0) {
        close(sock);
        return false;  // Peer doesn't support metadata extension
    }

    // Request metadata
    bool success = requestMetadata(sock, metadataExtId);

    close(sock);
    return success;
}

bool MetadataDownloader::performHandshake(int sock) {
    // Build handshake
    std::vector<uint8_t> handshake(68);

    // Protocol length + string
    handshake[0] = 19;
    memcpy(&handshake[1], "BitTorrent protocol", 19);

    // Reserved bytes: set the BEP 10 extended-protocol bit (0x10 in the last
    // reserved byte, i.e. byte index 27).
    memset(&handshake[20], 0, 8);
    handshake[27] = 0x10;

    // Info hash
    memcpy(&handshake[28], magnet_.infoHashRaw.c_str(), 20);

    // Peer ID
    std::string peerId = "-GT0001-" + std::string(12, 'M');
    memcpy(&handshake[48], peerId.c_str(), 20);

    // Send handshake (handle partial writes)
    if (!sendFull(sock, handshake.data(), handshake.size())) {
        return false;
    }

    // Receive handshake (handle partial reads)
    std::vector<uint8_t> recvHandshake(68);
    if (!recvFull(sock, recvHandshake.data(), recvHandshake.size())) {
        return false;
    }

    // Verify protocol
    if (recvHandshake[0] != 19 ||
        memcmp(&recvHandshake[1], "BitTorrent protocol", 19) != 0) {
        return false;
    }

    // Verify info hash
    if (memcmp(&recvHandshake[28], magnet_.infoHashRaw.c_str(), 20) != 0) {
        return false;
    }

    return true;
}

bool MetadataDownloader::exchangeExtensions(int sock, int& metadataExtId) {
    // Send extended handshake (BEP 10)
    // We don't know metadata_size yet, so we just announce ut_metadata support
    std::string clientMessage = "d1:md2:ut_metadatai1ee";

    // Wire format: <len><20><ext-id=0><bencoded dict>
    uint32_t length = 1 + 1 + static_cast<uint32_t>(clientMessage.size());
    std::vector<uint8_t> msg(4 + length);
    msg[0] = (length >> 24) & 0xFF;
    msg[1] = (length >> 16) & 0xFF;
    msg[2] = (length >> 8) & 0xFF;
    msg[3] = length & 0xFF;
    msg[4] = EXTENDED_MESSAGE_ID;              // 20 = extension protocol
    msg[5] = EXTENDED_HANDSHAKE_ID;            // 0 = extended handshake
    std::copy(clientMessage.begin(), clientMessage.end(), msg.begin() + 6);

    if (!sendFull(sock, msg.data(), msg.size())) {
        return false;
    }

    // Read the peer's extended handshake, skipping bitfield/have/keep-alive etc.
    std::vector<uint8_t> extData;
    if (!readExtendedMessage(sock, EXTENDED_HANDSHAKE_ID, extData)) {
        return false;
    }

    // Parse extended handshake
    try {
        std::string extStr(extData.begin(), extData.end());
        bencode::BencodeValue value = bencode::parse(extStr);

        if (value.isDict()) {
            const auto& dict = value.asDict();

            // Get metadata_size
            const auto* metadataSizeVal = bencode::dictGet(dict, "metadata_size");
            if (metadataSizeVal && metadataSizeVal->isInt()) {
                metadataSize_ = static_cast<int>(metadataSizeVal->asInt());
                numPieces_ = (metadataSize_ + METADATA_PIECE_SIZE - 1) / METADATA_PIECE_SIZE;
                pieces_.resize(numPieces_);
                std::cout << "Metadata size: " << metadataSize_ << " bytes, "
                          << numPieces_ << " pieces" << std::endl;
            }

            // Get ut_metadata extension ID from 'm' dictionary
            const auto* mDict = bencode::dictGet(dict, "m");
            if (mDict && mDict->isDict()) {
                const auto& m = mDict->asDict();
                const auto* utMetadata = bencode::dictGet(m, "ut_metadata");
                if (utMetadata && utMetadata->isInt()) {
                    metadataExtId = static_cast<int>(utMetadata->asInt());
                    std::cout << "Peer supports ut_metadata extension (ID: "
                              << metadataExtId << ")" << std::endl;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse extended handshake: " << e.what() << std::endl;
        return false;
    }

    return metadataExtId > 0 && metadataSize_ > 0;
}

bool MetadataDownloader::requestMetadata(int sock, int metadataExtId) {
    // Request each piece
    for (int i = 0; i < numPieces_; i++) {
        // BEP 9 request: bencoded dict with msg_type=0 and piece=<i>
        std::ostringstream reqOss;
        reqOss << "d8:msg_typei0e5:piecei" << i << "ee";
        std::string reqDict = reqOss.str();

        // Wire format: <len><20><ut_metadata id><bencoded dict>
        uint32_t length = 1 + 1 + static_cast<uint32_t>(reqDict.size());
        std::vector<uint8_t> msg(4 + length);
        msg[0] = (length >> 24) & 0xFF;
        msg[1] = (length >> 16) & 0xFF;
        msg[2] = (length >> 8) & 0xFF;
        msg[3] = length & 0xFF;
        msg[4] = EXTENDED_MESSAGE_ID;          // 20 = extension protocol
        msg[5] = static_cast<uint8_t>(metadataExtId);  // ut_metadata extension ID
        std::copy(reqDict.begin(), reqDict.end(), msg.begin() + 6);

        if (!sendFull(sock, msg.data(), msg.size())) {
            return false;
        }

        // Read the matching extended message, skipping unrelated messages
        std::vector<uint8_t> payload;
        if (!readExtendedMessage(sock, static_cast<uint8_t>(metadataExtId), payload)) {
            return false;
        }

        // The payload is a bencoded dict (msg_type, piece, total_size) followed
        // by the raw metadata piece data. Use the position-tracking parser so we
        // stop exactly after the dict instead of scanning (raw data may contain
        // 'd'/'e' bytes).
        std::string dictStr(payload.begin(), payload.end());
        size_t pos = 0;
        int msgType = -1;

        try {
            bencode::BencodeValue value = bencode::parse(dictStr, pos);
            if (!value.isDict()) return false;

            const auto& dict = value.asDict();
            const auto* msgTypeVal = bencode::dictGet(dict, "msg_type");
            if (msgTypeVal && msgTypeVal->isInt()) {
                msgType = static_cast<int>(msgTypeVal->asInt());
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to parse metadata response: " << e.what() << std::endl;
            return false;
        }

        if (msgType == 1) {  // Data response
            // Data follows the dictionary
            if (pos < payload.size()) {
                pieces_[i].assign(payload.begin() + static_cast<std::ptrdiff_t>(pos), payload.end());
            }
        } else if (msgType == 2) {  // Reject
            std::cerr << "Peer rejected metadata request for piece " << i << std::endl;
            return false;
        } else {
            std::cerr << "Unexpected metadata response type for piece " << i << std::endl;
            return false;
        }
    }

    // Combine pieces
    metadata_.clear();
    for (const auto& piece : pieces_) {
        metadata_.append(piece.begin(), piece.end());
    }

    // Verify metadata size
    if (static_cast<int>(metadata_.size()) != metadataSize_) {
        std::cerr << "Metadata size mismatch: expected " << metadataSize_
                  << ", got " << metadata_.size() << std::endl;
        metadata_.clear();
        return false;
    }

    // Verify metadata hash
    std::string hash = utils::sha1(metadata_);
    if (hash != magnet_.infoHashRaw) {
        std::cerr << "Metadata hash mismatch" << std::endl;
        metadata_.clear();
        return false;
    }

    complete_ = true;
    std::cout << "Metadata verified successfully, size: " << metadata_.size() << " bytes" << std::endl;
    return true;
}

std::string downloadMetadata(const MagnetLink& magnet) {
    MetadataDownloader downloader(magnet);

    if (downloader.start()) {
        return downloader.getTorrentData();
    }

    return "";
}
