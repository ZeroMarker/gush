#ifndef MAGNET_H
#define MAGNET_H

#include <string>
#include <vector>

struct MagnetLink {
    std::string infoHash;        // Info hash (hex or base32)
    std::string infoHashRaw;     // Raw info hash (20 bytes)
    std::string displayName;     // Display name (dn parameter)
    int64_t size = 0;            // File size (xl parameter)
    std::string exactTopic;      // Exact topic (xt parameter)
    std::string exactSource;     // Exact source (xs parameter)
    std::vector<std::string> trackers;  // Tracker list (tr parameter)
    
    bool isValid() const { return !infoHashRaw.empty(); }
};

// Parse magnet link
MagnetLink parseMagnetLink(const std::string& magnetUrl);

// Convert hex string to raw bytes
std::string hexToRaw(const std::string& hex);

// Convert base32 string to raw bytes
std::string base32ToRaw(const std::string& base32);

// Generate magnet link from info hash
std::string generateMagnetLink(const std::string& infoHashRaw, 
                                const std::string& displayName = "",
                                const std::vector<std::string>& trackers = {});

#endif // MAGNET_H
