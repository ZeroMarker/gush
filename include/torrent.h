#ifndef TORRENT_H
#define TORRENT_H

#include "bencode.h"
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

struct TorrentInfo {
    std::string name;
    std::string comment;
    std::string createdBy;
    int64_t pieceLength = 0;
    std::string pieces;  // Raw SHA1 hashes concatenated
    std::vector<std::string> announceList;
    std::string infoHash;  // SHA1 of bencoded info dict
    std::string peerId;
    
    // Single file mode
    std::string fileName;
    int64_t fileLength = 0;
    
    // Multi file mode
    struct FileEntry {
        std::string path;
        int64_t length = 0;
    };
    std::vector<FileEntry> files;
    
    bool isMultiFile() const { return !files.empty(); }
    int64_t totalLength() const;
    std::size_t numPieces() const;
};

// Load torrent from file
TorrentInfo loadTorrent(const std::string& filename);

// Validate invariants shared by .torrent files and magnet metadata.
void validateTorrentInfo(const TorrentInfo& torrent);

// Generate peer ID
std::string generatePeerId();

#endif // TORRENT_H
