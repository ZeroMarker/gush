#include "magnet.h"
#include "utils.h"
#include <algorithm>
#include <sstream>

namespace {

// URL decode a string
std::string urlDecode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int value;
            std::istringstream iss(str.substr(i + 1, 2));
            if (iss >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
                continue;
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

// Check if string is valid hex
bool isValidHex(const std::string& str) {
    if (str.size() != 40) return false;
    for (char c : str) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// Check if string is valid base32
bool isValidBase32(const std::string& str) {
    if (str.size() != 32) return false;
    for (char c : str) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return false;
    }
    return true;
}

} // anonymous namespace

std::string hexToRaw(const std::string& hex) {
    return utils::fromHex(hex);
}

std::string base32ToRaw(const std::string& base32) {
    // Base32 decoding for info hash
    static const int decodeTable[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,26,27,28,29,30,31,-1,-1,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    
    std::string result;
    uint32_t buffer = 0;
    int bitsLeft = 0;
    
    for (char c : base32) {
        if (c == '=') break;
        int val = decodeTable[static_cast<unsigned char>(c)];
        if (val < 0) continue;
        
        buffer = (buffer << 5) | val;
        bitsLeft += 5;
        
        if (bitsLeft >= 8) {
            bitsLeft -= 8;
            result += static_cast<char>((buffer >> bitsLeft) & 0xFF);
        }
    }
    
    return result;
}

MagnetLink parseMagnetLink(const std::string& magnetUrl) {
    MagnetLink magnet;

    // Check if it's a valid magnet link
    if (magnetUrl.substr(0, 8) != "magnet:?") {
        return magnet;
    }

    // Parse query parameters (start after "magnet:?")
    std::string query = magnetUrl.substr(8);
    
    size_t pos = 0;
    while (pos < query.size()) {
        // Find parameter name and value
        size_t eq = query.find('=', pos);
        if (eq == std::string::npos) break;
        
        std::string name = query.substr(pos, eq - pos);
        
        size_t next = query.find('&', eq + 1);
        std::string value;
        if (next == std::string::npos) {
            value = query.substr(eq + 1);
            pos = query.size();
        } else {
            value = query.substr(eq + 1, next - eq - 1);
            pos = next + 1;
        }
        
        // URL decode value
        value = urlDecode(value);
        
        // Process parameters
        if (name == "xt" && value.substr(0, 9) == "urn:btih:") {
            // Exact topic - contains info hash
            std::string hash = value.substr(9);
            
            if (isValidHex(hash)) {
                magnet.infoHash = hash;
                magnet.infoHashRaw = hexToRaw(hash);
            } else if (isValidBase32(hash)) {
                magnet.infoHashRaw = base32ToRaw(hash);
                magnet.infoHash = utils::toHex(magnet.infoHashRaw);
            }
        }
        else if (name == "dn") {
            // Display name
            magnet.displayName = value;
        }
        else if (name == "xl") {
            // Exact length (file size)
            try {
                magnet.size = std::stoll(value);
            } catch (...) {}
        }
        else if (name == "tr") {
            // Tracker URL
            magnet.trackers.push_back(value);
        }
        else if (name == "xs") {
            // Exact source
            magnet.exactSource = value;
        }
        else if (name == "xt" && value.substr(0, 9) != "urn:btih:") {
            // Other exact topic
            magnet.exactTopic = value;
        }
    }
    
    return magnet;
}

std::string generateMagnetLink(const std::string& infoHashRaw,
                                const std::string& displayName,
                                const std::vector<std::string>& trackers) {
    std::ostringstream oss;
    oss << "magnet:?xt=urn:btih:" << utils::toHex(infoHashRaw);
    
    if (!displayName.empty()) {
        oss << "&dn=" << utils::urlEncode(displayName);
    }
    
    for (const auto& tracker : trackers) {
        oss << "&tr=" << utils::urlEncode(tracker);
    }
    
    return oss.str();
}
