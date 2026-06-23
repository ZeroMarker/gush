#include "torrent.h"
#include "utils.h"
#include "bencode.h"
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>

int64_t TorrentInfo::totalLength() const {
    if (isMultiFile()) {
        int64_t total = 0;
        for (const auto& file : files) {
            total += file.length;
        }
        return total;
    }
    return fileLength;
}

std::size_t TorrentInfo::numPieces() const {
    return pieces.size() / 20;  // Each hash is 20 bytes (SHA1)
}

std::string generatePeerId() {
    // Generate a unique peer ID
    // Format: -GT0001-<random bytes>
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    std::ostringstream oss;
    oss << "-GT0001-";
    for (int i = 0; i < 12; i++) {
        char c = static_cast<char>(dis(gen));
        // Ensure printable ASCII
        c = 'A' + (c % 26);
        oss << c;
    }
    return oss.str();
}

TorrentInfo loadTorrent(const std::string& filename) {
    // Read torrent file
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open torrent file: " + filename);
    }
    
    std::ostringstream oss;
    oss << file.rdbuf();
    std::string content = oss.str();
    
    // Parse bencode
    bencode::BencodeValue root = bencode::parse(content);
    if (!root.isDict()) {
        throw std::runtime_error("Invalid torrent file: root is not a dictionary");
    }
    
    const auto& dict = root.asDict();
    TorrentInfo torrent;
    torrent.peerId = generatePeerId();
    
    // Get announce
    const auto* announce = bencode::dictGet(dict, "announce");
    if (announce && announce->isString()) {
        torrent.announceList.push_back(std::get<bencode::BencodeString>(announce->data));
    }

    // Get announce-list (if present)
    const auto* announceList = bencode::dictGet(dict, "announce-list");
    if (announceList && announceList->isList()) {
        for (const auto& tier : announceList->asList()) {
            if (tier.isList()) {
                for (const auto& tracker : tier.asList()) {
                    if (tracker.isString()) {
                        torrent.announceList.push_back(std::get<bencode::BencodeString>(tracker.data));
                    }
                }
            }
        }
    }

    // Get optional fields
    const auto* comment = bencode::dictGet(dict, "comment");
    if (comment && comment->isString()) {
        torrent.comment = std::get<bencode::BencodeString>(comment->data);
    }

    const auto* createdBy = bencode::dictGet(dict, "created by");
    if (createdBy && createdBy->isString()) {
        torrent.createdBy = std::get<bencode::BencodeString>(createdBy->data);
    }
    
    // Parse info dictionary
    const auto* info = bencode::dictGet(dict, "info");
    if (!info || !info->isDict()) {
        throw std::runtime_error("Invalid torrent file: missing info dictionary");
    }
    
    const auto& infoDict = info->asDict();
    
    // Get piece length
    const auto* pieceLength = bencode::dictGet(infoDict, "piece length");
    if (!pieceLength || !pieceLength->isInt()) {
        throw std::runtime_error("Invalid torrent file: missing piece length");
    }
    torrent.pieceLength = pieceLength->asInt();
    
    // Get pieces
    const auto* pieces = bencode::dictGet(infoDict, "pieces");
    if (!pieces || !pieces->isString()) {
        throw std::runtime_error("Invalid torrent file: missing pieces");
    }
    torrent.pieces = std::get<bencode::BencodeString>(pieces->data);

    // Get name
    const auto* name = bencode::dictGet(infoDict, "name");
    if (!name || !name->isString()) {
        throw std::runtime_error("Invalid torrent file: missing name");
    }
    torrent.name = std::get<bencode::BencodeString>(name->data);
    
    // Check if single file or multi-file
    const auto* files = bencode::dictGet(infoDict, "files");
    if (files && files->isList()) {
        // Multi-file mode
        for (const auto& fileEntry : files->asList()) {
            if (!fileEntry.isDict()) continue;
            
            const auto& fileDict = fileEntry.asDict();
            TorrentInfo::FileEntry entry;
            
            const auto* length = bencode::dictGet(fileDict, "length");
            if (length && length->isInt()) {
                entry.length = length->asInt();
            }
            
            const auto* path = bencode::dictGet(fileDict, "path");
            if (path && path->isList()) {
                std::ostringstream pathOss;
                const auto& pathList = path->asList();
                for (size_t i = 0; i < pathList.size(); i++) {
                    if (i > 0) pathOss << "/";
                    if (pathList[i].isString()) {
                        pathOss << std::get<bencode::BencodeString>(pathList[i].data);
                    }
                }
                entry.path = pathOss.str();
            }
            
            torrent.files.push_back(entry);
        }
    } else {
        // Single file mode
        const auto* length = bencode::dictGet(infoDict, "length");
        if (length && length->isInt()) {
            torrent.fileLength = length->asInt();
        }
        torrent.fileName = torrent.name;
    }
    
    // Calculate info hash (SHA1 of bencoded info dict)
    std::string infoEncoded = bencode::encode(*info);
    torrent.infoHash = utils::sha1(infoEncoded);
    
    return torrent;
}
