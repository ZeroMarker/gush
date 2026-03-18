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

    // Create a temporary TorrentInfo for tracker contact
    TorrentInfo tempTorrent;
    tempTorrent.infoHash = magnet_.infoHashRaw;
    tempTorrent.announceList = trackersToUse;
    tempTorrent.peerId = "-GT0001-" + std::string(12, 'M');

    // Contact each tracker to get peers
    std::vector<Peer> allPeers;
    for (const auto& trackerUrl : trackersToUse) {
        std::cout << "Contacting tracker: " << trackerUrl << std::endl;
        TrackerResponse response = contactTracker(trackerUrl, tempTorrent, tempTorrent.peerId);

        if (response.ok()) {
            std::cout << "  Got " << response.peers.size() << " peers from tracker" << std::endl;
            allPeers.insert(allPeers.end(), response.peers.begin(), response.peers.end());
        } else {
            std::cout << "  Tracker failed: " << response.failure << std::endl;
        }
        
        // Stop if we have enough peers
        if (allPeers.size() >= 50) break;
    }

    if (allPeers.empty()) {
        std::cerr << "No peers available from any tracker" << std::endl;
        std::cerr << "Note: Pure hash magnet links may need DHT for peer discovery" << std::endl;
        return false;
    }

    std::cout << "Total peers available: " << allPeers.size() << std::endl;

    // Try to download metadata from each peer
    int successCount = 0;
    for (const auto& peer : allPeers) {
        std::cout << "Trying peer: " << peer.ip << ":" << peer.port << std::endl;
        if (downloadFromPeer(peer.ip, peer.port)) {
            std::cout << "Successfully downloaded metadata from peer" << std::endl;
            successCount++;
            // Continue to get more pieces if needed
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
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    
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
    
    // Extended handshake flags (support extensions)
    memset(&handshake[20], 0, 8);
    handshake[20] = 0x01;  // Support extended messages
    
    // Info hash
    memcpy(&handshake[28], magnet_.infoHashRaw.c_str(), 20);
    
    // Peer ID
    std::string peerId = "-GT0001-" + std::string(12, 'M');
    memcpy(&handshake[48], peerId.c_str(), 20);
    
    // Send handshake
    if (send(sock, handshake.data(), handshake.size(), 0) != 68) {
        return false;
    }
    
    // Receive handshake
    std::vector<uint8_t> recvHandshake(68);
    if (recv(sock, recvHandshake.data(), recvHandshake.size(), 0) != 68) {
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

    std::vector<uint8_t> msg(6 + clientMessage.size());

    // Length (4 bytes) - 1 for extended type + 1 for ext id + dict size
    uint32_t length = 1 + 1 + clientMessage.size();
    msg[0] = (length >> 24) & 0xFF;
    msg[1] = (length >> 16) & 0xFF;
    msg[2] = (length >> 8) & 0xFF;
    msg[3] = length & 0xFF;

    // Extended message type (255 = extended handshake)
    msg[4] = 255;

    // ut_metadata extension ID (we use 1 as placeholder, actual ID is in peer's response)
    msg[5] = 1;

    // Extended handshake data
    std::copy(clientMessage.begin(), clientMessage.end(), msg.begin() + 6);

    if (send(sock, msg.data(), msg.size(), 0) != static_cast<ssize_t>(msg.size())) {
        return false;
    }

    // Read response
    uint8_t lenBuf[4];
    if (recv(sock, lenBuf, 4, 0) != 4) {
        return false;
    }

    uint32_t respLen = (static_cast<uint32_t>(lenBuf[0]) << 24) |
                       (static_cast<uint32_t>(lenBuf[1]) << 16) |
                       (static_cast<uint32_t>(lenBuf[2]) << 8) |
                       static_cast<uint32_t>(lenBuf[3]);

    if (respLen < 2) {
        return false;
    }

    uint8_t msgHeader[2];
    if (recv(sock, msgHeader, 2, 0) != 2) {
        return false;
    }

    if (msgHeader[0] != 255) {
        return false;  // Not an extended message
    }

    // Read extended handshake data
    std::vector<uint8_t> extData(respLen - 2);
    if (recv(sock, extData.data(), extData.size(), 0) != static_cast<ssize_t>(extData.size())) {
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
                numPieces_ = (metadataSize_ + 16383) / 16384;  // 16KB pieces
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
        // Create bencoded request dictionary
        std::ostringstream reqOss;
        reqOss << "d8:msg_typei0e5:piecei" << i << "e4:ut_metadatoi" << metadataExtId << "ee";
        std::string reqDict = reqOss.str();

        // Send extended message: length(4) + ext_msg_id(1) + ut_metadata_msg_id(1) + bencoded_dict
        std::vector<uint8_t> msg(6 + reqDict.size());

        uint32_t length = 1 + 1 + reqDict.size();  // ext msg type(1) + ut_metadata_id(1) + dict
        msg[0] = (length >> 24) & 0xFF;
        msg[1] = (length >> 16) & 0xFF;
        msg[2] = (length >> 8) & 0xFF;
        msg[3] = length & 0xFF;
        msg[4] = 255;  // Extended message type
        msg[5] = static_cast<uint8_t>(metadataExtId);  // ut_metadata extension ID
        std::copy(reqDict.begin(), reqDict.end(), msg.begin() + 6);

        if (send(sock, msg.data(), msg.size(), 0) != static_cast<ssize_t>(msg.size())) {
            return false;
        }

        // Read response header
        uint8_t lenBuf[4];
        if (recv(sock, lenBuf, 4, 0) != 4) {
            return false;
        }

        uint32_t respLen = (static_cast<uint32_t>(lenBuf[0]) << 24) |
                           (static_cast<uint32_t>(lenBuf[1]) << 16) |
                           (static_cast<uint32_t>(lenBuf[2]) << 8) |
                           static_cast<uint32_t>(lenBuf[3]);

        if (respLen < 2) {
            return false;
        }

        // Read extended message type and ut_metadata extension ID
        uint8_t header[2];
        if (recv(sock, header, 2, 0) != 2) {
            return false;
        }

        if (header[0] != 255 || header[1] != static_cast<uint8_t>(metadataExtId)) {
            return false;
        }

        // Read bencoded dictionary and optional data
        std::vector<uint8_t> payload(respLen - 2);
        if (recv(sock, payload.data(), payload.size(), 0) != static_cast<ssize_t>(payload.size())) {
            return false;
        }

        // Parse bencoded dictionary to find msg_type and data
        std::string dictStr(payload.begin(), payload.end());
        size_t dictEnd = dictStr.find('e');
        if (dictEnd == std::string::npos) {
            return false;
        }
        
        // Find the end of the dictionary (might be nested)
        int braceCount = 0;
        bool inDict = false;
        for (size_t j = 0; j < dictStr.size(); j++) {
            if (dictStr[j] == 'd') {
                braceCount++;
                inDict = true;
            } else if (dictStr[j] == 'e') {
                braceCount--;
                if (inDict && braceCount == 0) {
                    dictEnd = j;
                    break;
                }
            }
        }
        
        std::string bencodedDict = dictStr.substr(0, dictEnd + 1);
        
        try {
            bencode::BencodeValue value = bencode::parse(bencodedDict);
            
            if (value.isDict()) {
                const auto& dict = value.asDict();
                const auto* msgTypeVal = bencode::dictGet(dict, "msg_type");
                
                int msgType = -1;
                if (msgTypeVal && msgTypeVal->isInt()) {
                    msgType = static_cast<int>(msgTypeVal->asInt());
                }
                
                if (msgType == 1) {  // Data response
                    // Data follows the dictionary
                    size_t dataStart = dictEnd + 1;
                    if (dataStart < payload.size()) {
                        pieces_[i].assign(payload.begin() + dataStart, payload.end());
                    }
                } else if (msgType == 2) {  // Reject
                    std::cerr << "Peer rejected metadata request for piece " << i << std::endl;
                    return false;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to parse metadata response: " << e.what() << std::endl;
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
