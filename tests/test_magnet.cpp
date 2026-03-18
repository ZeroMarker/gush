#include <gtest/gtest.h>
#include "magnet.h"
#include "utils.h"

// Test hexToRaw conversion
TEST(MagnetTest, HexToRaw) {
    std::string hex = "0123456789abcdef0123456789abcdef01234567";
    std::string raw = hexToRaw(hex);
    EXPECT_EQ(raw.size(), 20u);
    EXPECT_EQ(utils::toHex(raw), hex);
}

TEST(MagnetTest, HexToRawEmpty) {
    std::string raw = hexToRaw("");
    EXPECT_TRUE(raw.empty());
}

// Test base32ToRaw conversion
TEST(MagnetTest, Base32ToRaw) {
    // Base32 encoded info hash (32 chars -> 20 bytes)
    std::string base32 = "MXRUWI4X2KPEO6XT6T2Z5QXZ3YHGZ3A2";
    std::string raw = base32ToRaw(base32);
    EXPECT_EQ(raw.size(), 20u);
}

TEST(MagnetTest, Base32ToRawKnown) {
    // Test with known base32 value - "Hello!" is 6 bytes, needs padding
    // JBSWY3DPEHPK3PXP decodes to "Hello!" but base32 works in 5-byte blocks
    std::string base32 = "JBSWY3DPEHPK3PXP";  
    std::string raw = base32ToRaw(base32);
    // The base32 decoder processes the input and returns decoded bytes
    EXPECT_GE(raw.size(), 6u);  // At least "Hello!" (6 chars)
    EXPECT_EQ(raw.substr(0, 6), "Hello!");
}

// Test parseMagnetLink with hex hash
TEST(MagnetTest, ParseMagnetHex) {
    std::string magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567";
    MagnetLink result = parseMagnetLink(magnet);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_EQ(result.infoHash, "0123456789abcdef0123456789abcdef01234567");
    EXPECT_EQ(result.infoHashRaw.size(), 20u);
}

// Test parseMagnetLink with display name
TEST(MagnetTest, ParseMagnetWithDisplayName) {
    std::string magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Test+File";
    MagnetLink result = parseMagnetLink(magnet);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_EQ(result.displayName, "Test File");
}

// Test parseMagnetLink with trackers
TEST(MagnetTest, ParseMagnetWithTrackers) {
    std::string magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"
                         "&tr=http://tracker1.com/announce"
                         "&tr=http://tracker2.com/announce";
    MagnetLink result = parseMagnetLink(magnet);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_EQ(result.trackers.size(), 2u);
    EXPECT_EQ(result.trackers[0], "http://tracker1.com/announce");
    EXPECT_EQ(result.trackers[1], "http://tracker2.com/announce");
}

// Test parseMagnetLink with file size
TEST(MagnetTest, ParseMagnetWithSize) {
    std::string magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&xl=1048576";
    MagnetLink result = parseMagnetLink(magnet);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_EQ(result.size, 1048576);  // 1 MB
}

// Test parseMagnetLink with base32 hash
TEST(MagnetTest, ParseMagnetBase32) {
    // 32 character base32 hash
    std::string magnet = "magnet:?xt=urn:btih:MXRUWI4X2KPEO6XT6T2Z5QXZ3YHGZ3A2";
    MagnetLink result = parseMagnetLink(magnet);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_EQ(result.infoHashRaw.size(), 20u);
}

// Test parseMagnetLink with all parameters
TEST(MagnetTest, ParseMagnetComplete) {
    std::string magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"
                         "&dn=Complete+Test"
                         "&xl=2097152"
                         "&tr=http://tracker.com/announce"
                         "&xs=http://example.com/file.torrent";
    MagnetLink result = parseMagnetLink(magnet);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_EQ(result.infoHash, "0123456789abcdef0123456789abcdef01234567");
    EXPECT_EQ(result.displayName, "Complete Test");
    EXPECT_EQ(result.size, 2097152);
    EXPECT_EQ(result.trackers.size(), 1u);
    EXPECT_EQ(result.exactSource, "http://example.com/file.torrent");
}

// Test parseMagnetLink invalid
TEST(MagnetTest, ParseMagnetInvalid) {
    // Not a magnet link
    MagnetLink result1 = parseMagnetLink("http://example.com/file.torrent");
    EXPECT_FALSE(result1.isValid());
    
    // Missing xt parameter
    MagnetLink result2 = parseMagnetLink("magnet:?dn=NoHash");
    EXPECT_FALSE(result2.isValid());
    
    // Invalid hash format
    MagnetLink result3 = parseMagnetLink("magnet:?xt=urn:btih:invalidhash");
    EXPECT_FALSE(result3.isValid());
}

// Test parseMagnetLink with URL encoding
TEST(MagnetTest, ParseMagnetUrlEncoded) {
    std::string magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"
                         "&dn=Hello%20World%21";
    MagnetLink result = parseMagnetLink(magnet);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_EQ(result.displayName, "Hello World!");
}

// Test generateMagnetLink
TEST(MagnetTest, GenerateMagnetLink) {
    std::string infoHashRaw = utils::fromHex("0123456789abcdef0123456789abcdef01234567");
    std::string result = generateMagnetLink(infoHashRaw, "Test File", {});

    EXPECT_TRUE(result.find("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567") != std::string::npos);
}

TEST(MagnetTest, GenerateMagnetLinkWithDisplayName) {
    std::string infoHashRaw = utils::fromHex("0123456789abcdef0123456789abcdef01234567");
    std::string result = generateMagnetLink(infoHashRaw, "Test File", {});

    EXPECT_TRUE(result.find("&dn=Test%20File") != std::string::npos);
}

TEST(MagnetTest, GenerateMagnetLinkWithTrackers) {
    std::string infoHashRaw = utils::fromHex("0123456789abcdef0123456789abcdef01234567");
    std::vector<std::string> trackers = {
        "http://tracker1.com/announce",
        "http://tracker2.com/announce"
    };
    std::string result = generateMagnetLink(infoHashRaw, "", trackers);

    // Trackers are URL-encoded in the magnet link
    EXPECT_TRUE(result.find("&tr=http%3A%2F%2Ftracker1.com%2Fannounce") != std::string::npos);
    EXPECT_TRUE(result.find("&tr=http%3A%2F%2Ftracker2.com%2Fannounce") != std::string::npos);
}

// Test generateMagnetLink round-trip
TEST(MagnetTest, MagnetLinkRoundTrip) {
    std::string originalHash = "0123456789abcdef0123456789abcdef01234567";
    std::string infoHashRaw = utils::fromHex(originalHash);
    
    MagnetLink parsed = parseMagnetLink(
        "magnet:?xt=urn:btih:" + originalHash + "&dn=Test"
    );
    
    std::string generated = generateMagnetLink(parsed.infoHashRaw, parsed.displayName, {});
    MagnetLink reparsed = parseMagnetLink(generated);
    
    EXPECT_EQ(reparsed.infoHash, originalHash);
    EXPECT_EQ(reparsed.displayName, "Test");
}
