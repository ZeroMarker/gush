#include "torrent.h"
#include "utils.h"
#include "bencode.h"
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>
#include <limits>

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

void validateTorrentInfo(const TorrentInfo& torrent) {
    if (torrent.name.empty()) {
        throw std::runtime_error("Invalid torrent: empty name");
    }
    if (torrent.pieceLength <= 0) {
        throw std::runtime_error("Invalid torrent: piece length must be positive");
    }
    if (torrent.pieces.size() % 20 != 0) {
        throw std::runtime_error("Invalid torrent: pieces length is not a multiple of 20");
    }

    int64_t total = 0;
    if (torrent.isMultiFile()) {
        for (const auto& entry : torrent.files) {
            if (entry.path.empty()) {
                throw std::runtime_error("Invalid torrent: empty file path");
            }
            if (entry.length < 0) {
                throw std::runtime_error("Invalid torrent: negative file length");
            }
            if (entry.length > std::numeric_limits<int64_t>::max() - total) {
                throw std::runtime_error("Invalid torrent: total length overflow");
            }
            total += entry.length;
        }
    } else {
        if (torrent.fileLength < 0) {
            throw std::runtime_error("Invalid torrent: negative file length");
        }
        total = torrent.fileLength;
    }

    const uint64_t expectedPieces = total == 0 ? 0 :
        1 + (static_cast<uint64_t>(total) - 1) /
                static_cast<uint64_t>(torrent.pieceLength);
    if (torrent.numPieces() != expectedPieces) {
        throw std::runtime_error("Invalid torrent: piece hash count does not match total length");
    }
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

// Try to extract the exact byte span of the root "info" dictionary from the raw
// torrent content. Re-encoding the parsed value is NOT byte-identical for
// non-canonical encodings (e.g. i-0e, leading zeros, non-minimal integers), which
// would change the info hash and break the torrent.
std::string extractInfoDictBytes(const std::string& content,
                                 const bencode::BencodeValue& parsedInfo) {
    size_t pos = 0;
    while ((pos = content.find("4:info", pos)) != std::string::npos) {
        // The key must sit at a dictionary-key position: preceded by 'd' (first
        // key) or by 'e' (end of the previous value).
        bool validStart = (pos == 0) || content[pos - 1] == 'd' || content[pos - 1] == 'e';
        if (validStart) {
            size_t endPos = pos + 6;  // Skip the "4:info" key
            try {
                bencode::BencodeValue v = bencode::parse(content, endPos);
                // Confirm this is the info dict (has the mandatory keys)
                if (v.isDict() &&
                    bencode::dictGet(v.asDict(), "pieces") &&
                    bencode::dictGet(v.asDict(), "piece length")) {
                    return content.substr(pos + 6, endPos - (pos + 6));
                }
            } catch (...) {
                // Not a valid value at this position; keep searching
            }
        }
        pos += 6;
    }
    // Fallback: canonical re-encode of the parsed info dict
    return bencode::encode(parsedInfo);
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
        if (files->asList().empty()) {
            throw std::runtime_error("Invalid torrent file: empty files list");
        }
        for (const auto& fileEntry : files->asList()) {
            if (!fileEntry.isDict()) {
                throw std::runtime_error("Invalid torrent file: file entry is not a dictionary");
            }
            
            const auto& fileDict = fileEntry.asDict();
            TorrentInfo::FileEntry entry;
            
            const auto* length = bencode::dictGet(fileDict, "length");
            if (!length || !length->isInt()) {
                throw std::runtime_error("Invalid torrent file: missing file length");
            }
            entry.length = length->asInt();
            
            const auto* path = bencode::dictGet(fileDict, "path");
            if (path && path->isList() && !path->asList().empty()) {
                std::ostringstream pathOss;
                const auto& pathList = path->asList();
                for (size_t i = 0; i < pathList.size(); i++) {
                    if (i > 0) pathOss << "/";
                    if (!pathList[i].isString() || pathList[i].asString().empty()) {
                        throw std::runtime_error("Invalid torrent file: invalid file path component");
                    }
                    pathOss << pathList[i].asString();
                }
                entry.path = pathOss.str();
            } else {
                throw std::runtime_error("Invalid torrent file: missing file path");
            }
            
            torrent.files.push_back(entry);
        }
    } else {
        // Single file mode
        const auto* length = bencode::dictGet(infoDict, "length");
        if (!length || !length->isInt()) {
            throw std::runtime_error("Invalid torrent file: missing length");
        }
        torrent.fileLength = length->asInt();
        torrent.fileName = torrent.name;
    }

    validateTorrentInfo(torrent);
    
    // Calculate info hash (SHA1 of the ORIGINAL bencoded info dict bytes)
    std::string infoEncoded = extractInfoDictBytes(content, *info);
    torrent.infoHash = utils::sha1(infoEncoded);
    
    return torrent;
}
