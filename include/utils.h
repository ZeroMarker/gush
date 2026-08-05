#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

namespace utils {

// ---- Simple leveled logging (thread-safe) --------------------------------
// Consolidates scattered std::cout/std::cerr calls. Default level: Info.
enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void setLogLevel(LogLevel level);
LogLevel logLevel();
void log(LogLevel level, std::string_view message);

inline void logError(std::string_view m) { log(LogLevel::Error, m); }
inline void logWarn(std::string_view m) { log(LogLevel::Warn, m); }
inline void logInfo(std::string_view m) { log(LogLevel::Info, m); }
inline void logDebug(std::string_view m) { log(LogLevel::Debug, m); }

// SHA1 hash
std::string sha1(std::string_view data);
std::string sha1(const uint8_t* data, size_t len);

// Hex encoding/decoding
std::string toHex(std::string_view data);
std::string fromHex(std::string_view hex);

// URL encoding
std::string urlEncode(std::string_view str);

// Convert integer to big-endian bytes
std::string intToBytes(uint32_t value);
std::string int64ToBytes(int64_t value);

// Convert big-endian bytes to integer
uint32_t bytesToInt(const uint8_t* bytes);
uint32_t bytesToInt(std::string_view bytes);

// Get current timestamp in seconds
int64_t timestamp() noexcept;

// Format bytes to human readable
std::string formatBytes(int64_t bytes);

// Format seconds to human readable time
std::string formatTime(int64_t seconds);

// Create directory if not exists
bool createDirectory(std::string_view path);

// Recursively create directories (like mkdir -p)
bool createDirectories(std::string_view path);

// Sanitize a single file/directory name (no separators, no traversal, printable only)
std::string sanitizeFileName(std::string_view name);

// Sanitize a relative path (may contain '/'), rejecting '..', absolute and empty components
std::string sanitizePath(std::string_view path);

// Get file size
int64_t getFileSize(std::string_view path);

// Check if file exists
bool fileExists(std::string_view path) noexcept;

} // namespace utils

#endif // UTILS_H
