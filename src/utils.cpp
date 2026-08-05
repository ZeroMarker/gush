#include "utils.h"
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>

namespace utils {

std::string sha1(std::string_view data) {
    return sha1(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string sha1(const uint8_t* data, size_t len) {
    uint8_t hash[SHA_DIGEST_LENGTH];
    SHA1(data, len, hash);
    return std::string(reinterpret_cast<char*>(hash), SHA_DIGEST_LENGTH);
}

std::string toHex(std::string_view data) {
    std::ostringstream oss;
    for (unsigned char c : data) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);
    }
    return oss.str();
}

std::string fromHex(std::string_view hex) {
    std::string result;
    if (hex.size() < 2 || hex.size() % 2 != 0) return result;
    result.reserve(hex.size() / 2);
    
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        char c1 = hex[i];
        char c2 = hex[i + 1];
        
        // Validate hex characters
        if (!std::isxdigit(static_cast<unsigned char>(c1)) || 
            !std::isxdigit(static_cast<unsigned char>(c2))) {
            return "";  // Return empty for invalid input
        }
        
        std::string byteStr = std::string(hex.substr(i, 2));
        unsigned char byte = static_cast<unsigned char>(std::stoul(byteStr, nullptr, 16));
        result.push_back(byte);
    }
    return result;
}

std::string urlEncode(std::string_view str) {
    std::ostringstream oss;
    for (unsigned char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            oss << c;
        } else {
            // Use uppercase hex with leading zero
            oss << '%' << std::uppercase << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);
        }
    }
    return oss.str();
}

std::string intToBytes(uint32_t value) {
    std::string result(4, '\0');
    result[0] = (value >> 24) & 0xFF;
    result[1] = (value >> 16) & 0xFF;
    result[2] = (value >> 8) & 0xFF;
    result[3] = value & 0xFF;
    return result;
}

std::string int64ToBytes(int64_t value) {
    std::string result(8, '\0');
    for (int i = 7; i >= 0; i--) {
        result[i] = value & 0xFF;
        value >>= 8;
    }
    return result;
}

uint32_t bytesToInt(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

uint32_t bytesToInt(std::string_view bytes) {
    if (bytes.size() < 4) return 0;
    return (static_cast<uint32_t>(static_cast<uint8_t>(bytes[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(bytes[3]));
}

int64_t timestamp() noexcept {
    return std::time(nullptr);
}

std::string formatBytes(int64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unitIndex < 4) {
        size /= 1024.0;
        unitIndex++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
    return oss.str();
}

std::string formatTime(int64_t seconds) {
    int64_t hrs = seconds / 3600;
    int64_t mins = (seconds % 3600) / 60;
    int64_t secs = seconds % 60;

    std::ostringstream oss;
    if (hrs > 0) {
        oss << hrs << "h ";
    }
    if (mins > 0 || hrs > 0) {
        oss << mins << "m ";
    }
    oss << secs << "s";
    return oss.str();
}

bool createDirectory(std::string_view path) {
    return createDirectories(path);
}

bool createDirectories(std::string_view path) {
    if (path.empty()) return false;

    std::string p(path);
    // Strip trailing slashes
    while (!p.empty() && p.back() == '/') {
        p.pop_back();
    }
    if (p.empty()) return false;

    struct stat st;
    if (stat(p.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    // Create each intermediate component
    size_t pos = p.find('/');
    while (pos != std::string::npos) {
        std::string part = p.substr(0, pos);
        if (!part.empty()) {
            if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
        pos = p.find('/', pos + 1);
    }

    if (mkdir(p.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string sanitizeFileName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        // Replace separators, NUL and control characters
        if (c == '/' || c == '\\' || c == '\0' || uc < 0x20) {
            out += '_';
        } else {
            out += c;
        }
    }
    return out;  // Caller decides how to handle "", "." and ".."
}

std::string sanitizePath(std::string_view path) {
    std::ostringstream oss;
    size_t pos = 0;
    bool first = true;

    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        std::string_view comp = (slash == std::string_view::npos)
            ? path.substr(pos)
            : path.substr(pos, slash - pos);

        std::string safe = sanitizeFileName(comp);
        // Drop empty, '.' and '..' components (also blocks absolute paths)
        if (!safe.empty() && safe != "." && safe != "..") {
            if (!first) oss << '/';
            oss << safe;
            first = false;
        }

        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }

    std::string result = oss.str();
    return result.empty() ? "_" : result;
}

int64_t getFileSize(std::string_view path) {
    const std::string pathStr(path);
    struct stat st;
    if (stat(pathStr.c_str(), &st) != 0) {
        return -1;
    }
    return st.st_size;
}

bool fileExists(std::string_view path) noexcept {
    const std::string pathStr(path);
    struct stat st;
    return stat(pathStr.c_str(), &st) == 0;
}

} // namespace utils
