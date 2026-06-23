#include <gtest/gtest.h>
#include "utils.h"
#include <cstring>
#include <filesystem>

// Test SHA1 hashing
TEST(UtilsTest, SHA1Empty) {
    std::string hash = utils::sha1("");
    // SHA1 of empty string is da39a3ee5e6b4b0d3255bfef95601890afd80709
    EXPECT_EQ(utils::toHex(hash), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(UtilsTest, SHA1Simple) {
    std::string hash = utils::sha1("hello");
    // SHA1 of "hello" is aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d
    EXPECT_EQ(utils::toHex(hash), "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d");
}

TEST(UtilsTest, SHA1WithData) {
    std::string hash = utils::sha1("The quick brown fox jumps over the lazy dog");
    // Known SHA1 hash
    EXPECT_EQ(utils::toHex(hash), "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST(UtilsTest, SHA1RawData) {
    const uint8_t data[] = {'T', 'h', 'e', ' ', 'q', 'u', 'i', 'c', 'k', ' ', 
                            'b', 'r', 'o', 'w', 'n', ' ', 'f', 'o', 'x', ' ',
                            'j', 'u', 'm', 'p', 's', ' ', 'o', 'v', 'e', 'r',
                            ' ', 't', 'h', 'e', ' ', 'l', 'a', 'z', 'y', ' ',
                            'd', 'o', 'g'};  // "The quick brown fox jumps over the lazy dog"
    std::string hash = utils::sha1(data, 43);
    EXPECT_EQ(utils::toHex(hash), "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

// Test hex encoding/decoding
TEST(UtilsTest, ToHex) {
    EXPECT_EQ(utils::toHex(std::string("\x48\x65\x6c\x6c\x6f", 5)), "48656c6c6f"); // "Hello"
    EXPECT_EQ(utils::toHex(std::string("\x00\xff", 2)), "00ff");
    EXPECT_EQ(utils::toHex(""), "");
}

TEST(UtilsTest, FromHex) {
    EXPECT_EQ(utils::fromHex("48656c6c6f"), "Hello");
    EXPECT_EQ(utils::fromHex("00ff"), std::string("\x00\xff", 2));
    EXPECT_EQ(utils::fromHex(""), "");
}

TEST(UtilsTest, FromHexInvalid) {
    EXPECT_EQ(utils::fromHex("f"), "");  // Incomplete
    // Test with invalid hex chars - should handle gracefully
    std::string result = utils::fromHex("xyzw");
    // Implementation may throw or return empty for invalid input
    EXPECT_TRUE(result.empty() || result.size() == 0);
}

TEST(UtilsTest, HexRoundTrip) {
    std::string original = "Hello, World!";
    std::string hex = utils::toHex(original);
    std::string decoded = utils::fromHex(hex);
    EXPECT_EQ(decoded, original);
}

// Test URL encoding
TEST(UtilsTest, UrlEncode) {
    EXPECT_EQ(utils::urlEncode("hello"), "hello");
    EXPECT_EQ(utils::urlEncode("hello world"), "hello%20world");
    EXPECT_EQ(utils::urlEncode("a+b"), "a%2Bb");
    EXPECT_EQ(utils::urlEncode("test@example.com"), "test%40example.com");
    EXPECT_EQ(utils::urlEncode("~user-name"), "~user-name");
}

TEST(UtilsTest, UrlEncodeSpecialChars) {
    EXPECT_EQ(utils::urlEncode("<>&\"'"), "%3C%3E%26%22%27");
}

// Test integer to bytes conversion
TEST(UtilsTest, IntToBytes) {
    std::string bytes = utils::intToBytes(0x12345678);
    EXPECT_EQ(bytes.size(), 4u);
    EXPECT_EQ(static_cast<uint8_t>(bytes[0]), 0x12);
    EXPECT_EQ(static_cast<uint8_t>(bytes[1]), 0x34);
    EXPECT_EQ(static_cast<uint8_t>(bytes[2]), 0x56);
    EXPECT_EQ(static_cast<uint8_t>(bytes[3]), 0x78);
}

TEST(UtilsTest, IntToBytesZero) {
    std::string bytes = utils::intToBytes(0);
    EXPECT_EQ(bytes.size(), 4u);
    EXPECT_EQ(bytes, std::string("\x00\x00\x00\x00", 4));
}

TEST(UtilsTest, IntToBytesMax) {
    std::string bytes = utils::intToBytes(0xFFFFFFFF);
    EXPECT_EQ(bytes.size(), 4u);
    EXPECT_EQ(static_cast<uint8_t>(bytes[0]), 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(bytes[1]), 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(bytes[2]), 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(bytes[3]), 0xFF);
}

TEST(UtilsTest, Int64ToBytes) {
    std::string bytes = utils::int64ToBytes(0x123456789ABCDEF0);
    EXPECT_EQ(bytes.size(), 8u);
    EXPECT_EQ(static_cast<uint8_t>(bytes[0]), 0x12);
    EXPECT_EQ(static_cast<uint8_t>(bytes[7]), 0xF0);
}

// Test bytes to integer conversion
TEST(UtilsTest, BytesToInt) {
    uint8_t bytes[] = {0x12, 0x34, 0x56, 0x78};
    EXPECT_EQ(utils::bytesToInt(bytes), 0x12345678);
}

TEST(UtilsTest, BytesToIntZero) {
    uint8_t bytes[] = {0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(utils::bytesToInt(bytes), 0u);
}

TEST(UtilsTest, BytesToIntStringView) {
    std::string bytes = "\x12\x34\x56\x78";
    EXPECT_EQ(utils::bytesToInt(std::string_view(bytes)), 0x12345678);
}

TEST(UtilsTest, BytesToIntStringViewShort) {
    std::string bytes = "\x12\x34";
    EXPECT_EQ(utils::bytesToInt(std::string_view(bytes)), 0u);  // Too short
}

// Test timestamp
TEST(UtilsTest, Timestamp) {
    int64_t ts = utils::timestamp();
    EXPECT_GT(ts, 0);
    // Should be reasonable (after year 2000)
    EXPECT_GT(ts, 946684800);  // 2000-01-01
}

// Test formatBytes
TEST(UtilsTest, FormatBytes) {
    EXPECT_EQ(utils::formatBytes(0), "0.00 B");
    EXPECT_EQ(utils::formatBytes(1023), "1023.00 B");
    EXPECT_EQ(utils::formatBytes(1024), "1.00 KB");
    EXPECT_EQ(utils::formatBytes(1536), "1.50 KB");
    EXPECT_EQ(utils::formatBytes(1048576), "1.00 MB");
    EXPECT_EQ(utils::formatBytes(1073741824), "1.00 GB");
    EXPECT_EQ(utils::formatBytes(1099511627776), "1.00 TB");
}

// Test formatTime
TEST(UtilsTest, FormatTime) {
    EXPECT_EQ(utils::formatTime(0), "0s");
    EXPECT_EQ(utils::formatTime(59), "59s");
    EXPECT_EQ(utils::formatTime(60), "1m 0s");
    EXPECT_EQ(utils::formatTime(125), "2m 5s");
    EXPECT_EQ(utils::formatTime(3600), "1h 0m 0s");
    EXPECT_EQ(utils::formatTime(3661), "1h 1m 1s");
    EXPECT_EQ(utils::formatTime(90061), "25h 1m 1s");
}

// Test fileExists
TEST(UtilsTest, FileExists) {
    const auto sourceDir = std::filesystem::path(GUSH_SOURCE_DIR);
    const auto existingFile = sourceDir / "CMakeLists.txt";
    const auto missingFile = sourceDir / "nonexistent_file_xyz.txt";

    EXPECT_TRUE(utils::fileExists(existingFile.string()));
    
    EXPECT_FALSE(utils::fileExists(missingFile.string()));
}

// Test getFileSize
TEST(UtilsTest, GetFileSize) {
    const auto sourceDir = std::filesystem::path(GUSH_SOURCE_DIR);
    const auto existingFile = sourceDir / "CMakeLists.txt";
    const auto missingFile = sourceDir / "nonexistent_file_xyz.txt";

    int64_t size = utils::getFileSize(existingFile.string());
    EXPECT_GT(size, 0);
    
    int64_t missingSize = utils::getFileSize(missingFile.string());
    EXPECT_EQ(missingSize, -1);
}

// Test createDirectory
TEST(UtilsTest, CreateDirectory) {
    const auto testDir = std::filesystem::temp_directory_path() / "gush_test_dir";
    
    std::filesystem::remove_all(testDir);
    
    // Create directory
    bool result = utils::createDirectory(testDir.string());
    EXPECT_TRUE(result);
    
    // Verify it exists
    EXPECT_TRUE(utils::fileExists(testDir.string()));
    
    // Create again (should succeed because it already exists)
    result = utils::createDirectory(testDir.string());
    EXPECT_TRUE(result);
    
    std::filesystem::remove_all(testDir);
}
