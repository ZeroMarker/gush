#include "magnet.h"
#include "utils.h"
#include <iostream>

int main() {
    std::string magnet = "magnet:?xt=urn:btih:66a9f29746ced3e9ea6e4e1a5e5e5e5e5e5e5e5e&dn=TestFile&tr=http://tracker.example.com/announce";
    
    std::cout << "Input magnet: " << magnet << std::endl;
    std::cout << "Starts with magnet:? : " << (magnet.substr(0, 9) == "magnet:?") << std::endl;
    
    MagnetLink magnetLink = parseMagnetLink(magnet);
    
    std::cout << "\nParsed result:" << std::endl;
    std::cout << "  isValid:      " << magnetLink.isValid() << std::endl;
    std::cout << "  infoHash:     " << magnetLink.infoHash << std::endl;
    std::cout << "  infoHashRaw:  " << magnetLink.infoHashRaw.size() << " bytes" << std::endl;
    std::cout << "  displayName:  " << magnetLink.displayName << std::endl;
    std::cout << "  trackers:     " << magnetLink.trackers.size() << std::endl;
    
    // Test hex validation
    std::string hash = "66a9f29746ced3e9ea6e4e1a5e5e5e5e5e5e5e5e";
    std::cout << "\nHex validation for '" << hash << "':" << std::endl;
    std::cout << "  Length: " << hash.size() << std::endl;
    
    bool allHex = true;
    for (char c : hash) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            allHex = false;
            break;
        }
    }
    std::cout << "  All hex: " << allHex << std::endl;
    
    // Test hex to raw conversion
    std::string raw = utils::fromHex(hash);
    std::cout << "  Raw size: " << raw.size() << std::endl;
    std::cout << "  Raw hex:  " << utils::toHex(raw) << std::endl;
    
    return 0;
}
