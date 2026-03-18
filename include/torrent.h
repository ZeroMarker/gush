#ifndef TORRENT_H
#define TORRENT_H

#include "bencode.h"
#include <string>
#include <vector>
#include <cstdint>

struct TorrentInfo {
    std::string name;
    std::string comment;
    std::string createdBy;
    int64_t pieceLength;
    std::string pieces;  // Raw SHA1 hashes concatenated
    std::vector<std::string> announceList;
    std::string infoHash;  // SHA1 of bencoded info dict
    std::string peerId;
    
    // Single file mode
    std::string fileName;
    int64_t fileLength;
    
    // Multi file mode
    struct FileEntry {
        std::string path;
        int64_t length;
    };
    std::vector<FileEntry> files;
    
    bool isMultiFile() const { return !files.empty(); }
    int64_t totalLength() const;
    int numPieces() const;
};

// Load torrent from file
TorrentInfo loadTorrent(const std::string& filename);

// Generate peer ID
std::string generatePeerId();

#endif // TORRENT_H
