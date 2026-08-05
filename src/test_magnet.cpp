#include "magnet.h"
#include "utils.h"
#include <iostream>
#include <cassert>

void testParseHexMagnet() {
    std::string magnet = "magnet:?xt=urn:btih:66a9f29746ced3e9ea6e4e1a5e5e5e5e5e5e5e5e&dn=TestFile&tr=http://tracker.example.com/announce";
    
    MagnetLink magnetLink = parseMagnetLink(magnet);
    
    assert(magnetLink.isValid());
    assert(magnetLink.infoHash == "66a9f29746ced3e9ea6e4e1a5e5e5e5e5e5e5e5e");
    assert(magnetLink.displayName == "TestFile");
    assert(magnetLink.trackers.size() == 1);
    assert(magnetLink.trackers[0] == "http://tracker.example.com/announce");
    assert(magnetLink.infoHashRaw.size() == 20);
    
    std::cout << "✓ testParseHexMagnet passed" << std::endl;
}

void testParseBase32Magnet() {
    // Base32 encoded hash (32 chars): NZQWY4YTOBJZW3G2WQXGK5NVDZQWY4YT
    std::string magnet = "magnet:?xt=urn:btih:NZQWY4YTOBJZW3G2WQXGK5NVDZQWY4YT&dn=Base32Test";
    
    MagnetLink magnetLink = parseMagnetLink(magnet);
    
    assert(magnetLink.isValid());
    assert(magnetLink.displayName == "Base32Test");
    assert(magnetLink.infoHashRaw.size() == 20);
    
    std::cout << "✓ testParseBase32Magnet passed" << std::endl;
}

void testParseMultipleTrackers() {
    std::string magnet = "magnet:?xt=urn:btih:abcdef1234567890abcdef1234567890abcdef12"
                         "&tr=http://tracker1.com/announce"
                         "&tr=http://tracker2.com/announce"
                         "&tr=udp://tracker3.com:8080";
    
    MagnetLink magnetLink = parseMagnetLink(magnet);
    
    assert(magnetLink.isValid());
    assert(magnetLink.trackers.size() == 3);
    
    std::cout << "✓ testParseMultipleTrackers passed" << std::endl;
}

void testParseWithSize() {
    std::string magnet = "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678"
                         "&dn=SizeTest&xl=1048576";
    
    MagnetLink magnetLink = parseMagnetLink(magnet);
    
    assert(magnetLink.isValid());
    assert(magnetLink.displayName == "SizeTest");
    assert(magnetLink.size == 1048576);
    
    std::cout << "✓ testParseWithSize passed" << std::endl;
}

void testGenerateMagnet() {
    std::string infoHashRaw = "\x66\xa9\xf2\x97\x46\xce\xd3\xe9\xea\x6e\x4e\x1a\x5e\x5e\x5e\x5e\x5e\x5e\x5e\x5e";
    std::string displayName = "GeneratedTest";
    std::vector<std::string> trackers = {
        "http://tracker1.com/announce",
        "udp://tracker2.com:8080"
    };
    
    std::string magnet = generateMagnetLink(infoHashRaw, displayName, trackers);
    
    assert(magnet.rfind("magnet:?xt=urn:btih:", 0) == 0);
    assert(magnet.find("xt=urn:btih:66a9f29746ced3e9ea6e4e1a5e5e5e5e5e5e5e5e") != std::string::npos);
    assert(magnet.find("dn=GeneratedTest") != std::string::npos);
    
    std::cout << "Generated magnet: " << magnet << std::endl;
    std::cout << "✓ testGenerateMagnet passed" << std::endl;
}

void testInvalidMagnet() {
    std::string invalid = "http://example.com/torrent";
    MagnetLink magnetLink = parseMagnetLink(invalid);
    assert(!magnetLink.isValid());
    
    std::cout << "✓ testInvalidMagnet passed" << std::endl;
}

void testParseUbuntuMagnet() {
    // Real-world style Ubuntu magnet link
    std::string magnet = "magnet:?xt=urn:btih:66a9f29746ced3e9ea6e4e1a5e5e5e5e5e5e5e5e"
                         "&dn=ubuntu-22.04-desktop-amd64.iso"
                         "&tr=http://tracker.ubuntu.com:80/announce"
                         "&xl=3579270144";
    
    MagnetLink magnetLink = parseMagnetLink(magnet);
    
    assert(magnetLink.isValid());
    assert(magnetLink.displayName == "ubuntu-22.04-desktop-amd64.iso");
    assert(magnetLink.size == 3579270144);
    assert(magnetLink.trackers.size() == 1);
    
    std::cout << "\nUbuntu magnet link test:" << std::endl;
    std::cout << "  Name:       " << magnetLink.displayName << std::endl;
    std::cout << "  Size:       " << utils::formatBytes(magnetLink.size) << std::endl;
    std::cout << "  Info Hash:  " << magnetLink.infoHash << std::endl;
    std::cout << "  Trackers:   " << magnetLink.trackers.size() << std::endl;
    std::cout << "✓ testParseUbuntuMagnet passed" << std::endl;
}

int main() {
    std::cout << "=== Magnet Link Tests ===" << std::endl << std::endl;
    
    testParseHexMagnet();
    testParseBase32Magnet();
    testParseMultipleTrackers();
    testParseWithSize();
    testGenerateMagnet();
    testInvalidMagnet();
    testParseUbuntuMagnet();
    
    std::cout << "\n=== All tests passed! ===" << std::endl;
    return 0;
}
