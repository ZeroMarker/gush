#include <gtest/gtest.h>
#include "torrent.h"
#include "bencode.h"
#include "utils.h"
#include <fstream>
#include <cstring>

namespace {

// Build a minimal single-file torrent via the library's own bencode encoder,
// guaranteeing a well-formed structure.
std::string buildSingleTorrent(const std::string& name = "test.txt",
                               int64_t length = 1024,
                               const std::string& pieces = std::string(20, 'a')) {
    bencode::BencodeDict info;
    info["length"] = length;
    info["name"] = bencode::BencodeString(name);
    info["piece length"] = int64_t(16384);
    info["pieces"] = bencode::BencodeString(pieces);

    bencode::BencodeDict root;
    root["announce"] = bencode::BencodeString("http://tracker.example.com/announce");
    root["info"] = std::move(info);
    return bencode::encode(bencode::BencodeValue(std::move(root)));
}

// Build a minimal multi-file torrent via the bencode encoder.
std::string buildMultiTorrent() {
    bencode::BencodeDict info;
    info["name"] = bencode::BencodeString("multifile");
    info["piece length"] = int64_t(16384);
    info["pieces"] = bencode::BencodeString(std::string(20, 'b'));

    bencode::BencodeList files;

    bencode::BencodeDict f1;
    f1["length"] = int64_t(100);
    bencode::BencodeList p1;
    p1.push_back(bencode::BencodeString("file1.txt"));
    f1["path"] = std::move(p1);
    files.push_back(std::move(f1));

    bencode::BencodeDict f2;
    f2["length"] = int64_t(200);
    bencode::BencodeList p2;
    p2.push_back(bencode::BencodeString("file2.txt"));
    f2["path"] = std::move(p2);
    files.push_back(std::move(f2));

    info["files"] = std::move(files);

    bencode::BencodeDict root;
    root["announce"] = bencode::BencodeString("http://tracker.example.com/announce");
    root["info"] = std::move(info);
    return bencode::encode(bencode::BencodeValue(std::move(root)));
}

void writeTempFile(const std::string& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    file.write(content.c_str(), static_cast<std::streamsize>(content.size()));
}

} // namespace

// Test generatePeerId
TEST(TorrentTest, GeneratePeerId) {
    std::string peerId1 = generatePeerId();
    std::string peerId2 = generatePeerId();

    // Should start with -GT0001-
    EXPECT_EQ(peerId1.substr(0, 8), "-GT0001-");
    EXPECT_EQ(peerId2.substr(0, 8), "-GT0001-");

    // Should be unique
    EXPECT_NE(peerId1, peerId2);

    // Should be 20 characters total
    EXPECT_EQ(peerId1.size(), 20u);
    EXPECT_EQ(peerId2.size(), 20u);
}

// Test loadTorrent with single file
TEST(TorrentTest, LoadTorrentSingleFile) {
    std::string testFile = "/tmp/test_single.torrent";
    writeTempFile(testFile, buildSingleTorrent());

    TorrentInfo torrent = loadTorrent(testFile);

    EXPECT_EQ(torrent.name, "test.txt");
    EXPECT_EQ(torrent.fileLength, 1024);
    EXPECT_EQ(torrent.pieceLength, 16384);
    EXPECT_EQ(torrent.pieces.size(), 20u);  // 1 piece * 20 bytes
    EXPECT_FALSE(torrent.isMultiFile());
    EXPECT_EQ(torrent.announceList.size(), 1u);
    EXPECT_EQ(torrent.announceList[0], "http://tracker.example.com/announce");

    // Clean up
    std::remove(testFile.c_str());
}

// Test loadTorrent with multi-file
TEST(TorrentTest, LoadTorrentMultiFile) {
    std::string testFile = "/tmp/test_multi.torrent";
    writeTempFile(testFile, buildMultiTorrent());

    TorrentInfo torrent = loadTorrent(testFile);

    EXPECT_EQ(torrent.name, "multifile");
    EXPECT_TRUE(torrent.isMultiFile());
    EXPECT_EQ(torrent.files.size(), 2u);
    EXPECT_EQ(torrent.files[0].path, "file1.txt");
    EXPECT_EQ(torrent.files[0].length, 100);
    EXPECT_EQ(torrent.files[1].path, "file2.txt");
    EXPECT_EQ(torrent.files[1].length, 200);

    // Clean up
    std::remove(testFile.c_str());
}

// Test totalLength
TEST(TorrentTest, TotalLengthSingleFile) {
    std::string testFile = "/tmp/test_single.torrent";
    writeTempFile(testFile, buildSingleTorrent());

    TorrentInfo torrent = loadTorrent(testFile);
    EXPECT_EQ(torrent.totalLength(), 1024);

    std::remove(testFile.c_str());
}

TEST(TorrentTest, TotalLengthMultiFile) {
    std::string testFile = "/tmp/test_multi.torrent";
    writeTempFile(testFile, buildMultiTorrent());

    TorrentInfo torrent = loadTorrent(testFile);
    EXPECT_EQ(torrent.totalLength(), 300);  // 100 + 200

    std::remove(testFile.c_str());
}

// Test numPieces
TEST(TorrentTest, NumPieces) {
    std::string testFile = "/tmp/test_single.torrent";
    writeTempFile(testFile, buildSingleTorrent());

    TorrentInfo torrent = loadTorrent(testFile);
    EXPECT_EQ(torrent.numPieces(), 1);

    std::remove(testFile.c_str());
}

// Test infoHash calculation
TEST(TorrentTest, InfoHashCalculation) {
    std::string testFile = "/tmp/test_single.torrent";
    writeTempFile(testFile, buildSingleTorrent());

    TorrentInfo torrent = loadTorrent(testFile);

    // Info hash should be 20 bytes
    EXPECT_EQ(torrent.infoHash.size(), 20u);

    // Verify by recalculating over the canonical info dict
    bencode::BencodeDict info;
    info["length"] = int64_t(1024);
    info["name"] = bencode::BencodeString("test.txt");
    info["piece length"] = int64_t(16384);
    info["pieces"] = bencode::BencodeString(std::string(20, 'a'));
    std::string infoDict = bencode::encode(bencode::BencodeValue(std::move(info)));
    std::string expectedHash = utils::sha1(infoDict);
    EXPECT_EQ(torrent.infoHash, expectedHash);

    std::remove(testFile.c_str());
}

// Test info hash is computed from the original bytes even for non-canonical
// integer encodings (re-encoding would change the hash and break the torrent).
TEST(TorrentTest, InfoHashNonCanonicalEncoding) {
    // Same info dict as buildSingleTorrent but with a non-canonical length
    // encoding (leading zeros). The info hash must match the raw bytes.
    std::string content =
        "d8:announce35:http://tracker.example.com/announce"
        "4:infod6:lengthi001024e4:name8:test.txt12:piece lengthi16384e"
        "6:pieces20:aaaaaaaaaaaaaaaaaaaae"
        "e"
        "e";

    std::string testFile = "/tmp/test_noncanonical.torrent";
    writeTempFile(testFile, content);

    TorrentInfo torrent = loadTorrent(testFile);
    EXPECT_EQ(torrent.fileLength, 1024);
    EXPECT_EQ(torrent.infoHash.size(), 20u);

    // Expected hash is over the exact bytes in the file ("i001024e")
    std::string infoDict = "d6:lengthi001024e4:name8:test.txt12:piece lengthi16384e6:pieces20:aaaaaaaaaaaaaaaaaaaae";
    EXPECT_EQ(torrent.infoHash, utils::sha1(infoDict));

    std::remove(testFile.c_str());
}

// Test loadTorrent with comment and created by
TEST(TorrentTest, LoadTorrentWithComment) {
    bencode::BencodeDict root;
    root["announce"] = bencode::BencodeString("http://tracker.example.com/announce");
    root["comment"] = bencode::BencodeString("Test comment!");
    root["created by"] = bencode::BencodeString("TestCreator");

    bencode::BencodeDict info;
    info["length"] = int64_t(1024);
    info["name"] = bencode::BencodeString("test.txt");
    info["piece length"] = int64_t(16384);
    info["pieces"] = bencode::BencodeString(std::string(20, 'a'));
    root["info"] = std::move(info);

    std::string testFile = "/tmp/test_comment.torrent";
    writeTempFile(testFile, bencode::encode(bencode::BencodeValue(std::move(root))));

    TorrentInfo torrent = loadTorrent(testFile);

    EXPECT_EQ(torrent.comment, "Test comment!");
    EXPECT_EQ(torrent.createdBy, "TestCreator");

    std::remove(testFile.c_str());
}

// Test loadTorrent with announce-list
TEST(TorrentTest, LoadTorrentWithAnnounceList) {
    bencode::BencodeDict root;
    root["announce"] = bencode::BencodeString("http://tracker.example.com/announce");

    bencode::BencodeList announceList;
    bencode::BencodeList tier1;
    tier1.push_back(bencode::BencodeString("http://tracker1.example.com/announce"));
    announceList.push_back(std::move(tier1));
    bencode::BencodeList tier2;
    tier2.push_back(bencode::BencodeString("http://tracker2.example.com/announce"));
    announceList.push_back(std::move(tier2));
    root["announce-list"] = std::move(announceList);

    bencode::BencodeDict info;
    info["length"] = int64_t(1024);
    info["name"] = bencode::BencodeString("test.txt");
    info["piece length"] = int64_t(16384);
    info["pieces"] = bencode::BencodeString(std::string(20, 'a'));
    root["info"] = std::move(info);

    std::string testFile = "/tmp/test_announce.torrent";
    writeTempFile(testFile, bencode::encode(bencode::BencodeValue(std::move(root))));

    TorrentInfo torrent = loadTorrent(testFile);

    // Should have at least the trackers from announce-list plus the announce key
    EXPECT_GE(torrent.announceList.size(), 3u);
    EXPECT_EQ(torrent.announceList[1], "http://tracker1.example.com/announce");
    EXPECT_EQ(torrent.announceList[2], "http://tracker2.example.com/announce");

    std::remove(testFile.c_str());
}

// Test loadTorrent invalid file
TEST(TorrentTest, LoadTorrentInvalidFile) {
    EXPECT_THROW(loadTorrent("/nonexistent/path/file.torrent"), std::runtime_error);
}

// Test loadTorrent invalid bencode
TEST(TorrentTest, LoadTorrentInvalidBencode) {
    std::string testFile = "/tmp/test_invalid.torrent";
    std::ofstream file(testFile, std::ios::binary);
    file.write("invalid bencode content", 23);
    file.close();

    // Should throw some exception (runtime_error or invalid_argument)
    EXPECT_ANY_THROW(loadTorrent(testFile));

    std::remove(testFile.c_str());
}

// Test loadTorrent missing info
TEST(TorrentTest, LoadTorrentMissingInfo) {
    std::string content = "d8:announce35:http://tracker.example.com/announcee";

    std::string testFile = "/tmp/test_noinfo.torrent";
    writeTempFile(testFile, content);

    EXPECT_THROW(loadTorrent(testFile), std::runtime_error);

    std::remove(testFile.c_str());
}

// Test TorrentInfo copy/move
TEST(TorrentTest, TorrentInfoMove) {
    std::string testFile = "/tmp/test_single.torrent";
    writeTempFile(testFile, buildSingleTorrent());

    TorrentInfo torrent1 = loadTorrent(testFile);
    std::string name1 = torrent1.name;

    TorrentInfo torrent2 = std::move(torrent1);
    EXPECT_EQ(torrent2.name, name1);

    std::remove(testFile.c_str());
}
