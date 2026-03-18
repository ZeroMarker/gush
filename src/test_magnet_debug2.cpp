#include <iostream>
#include <string>

int main() {
    std::string magnet = "magnet:?xt=urn:btih:66a9f29746ced3e9ea6e4e1a5e5e5e5e5e5e5e5e&dn=TestFile";
    
    std::cout << "First 9 chars: '";
    for (int i = 0; i < 9 && i < magnet.size(); i++) {
        std::cout << magnet[i];
    }
    std::cout << "'" << std::endl;
    
    std::cout << "magnet.substr(0, 9): '" << magnet.substr(0, 9) << "'" << std::endl;
    std::cout << "Comparison with \"magnet:?\": " << (magnet.substr(0, 9) == "magnet:?") << std::endl;
    std::cout << "magnet[8] = '" << magnet[8] << "' (ASCII: " << (int)magnet[8] << ")" << std::endl;
    
    return 0;
}
