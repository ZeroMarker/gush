#include "torrent.h"
#include "tracker.h"
#include "downloader.h"
#include "magnet.h"
#include "metadata.h"
#include "utils.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <signal.h>
#include <iomanip>
#include <atomic>
#include <vector>
#include <cstdlib>

static Downloader* g_downloader = nullptr;
static std::atomic<bool> g_interrupt{false};

void signalHandler(int /*signum*/) {
    std::cout << "\nInterrupt received, stopping download..." << std::endl;
    g_interrupt = true;
    if (g_downloader) {
        // Only flip atomics here: joining the download thread from a signal
        // handler is not async-signal-safe and can deadlock.
        g_downloader->requestStop();
    }
}

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [options] <torrent_file|magnet_link> [output_directory]" << std::endl;
    std::cout << std::endl;
    std::cout << "Gush - A minimal BitTorrent client" << std::endl;
    std::cout << std::endl;
    std::cout << "Arguments:" << std::endl;
    std::cout << "  torrent_file      Path to the .torrent file" << std::endl;
    std::cout << "  magnet_link       Magnet link (magnet:?xt=urn:btih:...)" << std::endl;
    std::cout << "  output_directory  Directory to save downloaded files (default: current directory)" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -o, --download-dir <dir>   Directory to save downloaded files" << std::endl;
    std::cout << "      --max-peers <n>        Maximum concurrent peer connections (default: 30)" << std::endl;
    std::cout << "      --no-tracker-refresh   Do not fetch tracker lists from the internet" << std::endl;
    std::cout << "  -v, --verbose              Verbose logging (tracker/peer details)" << std::endl;
    std::cout << "  -h, --help                 Show this help" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << program << " ubuntu-22.04.torrent ~/Downloads" << std::endl;
    std::cout << "  " << program << " -o ~/Downloads --max-peers 50 \"magnet:?xt=urn:btih:HASH\"" << std::endl;
}

struct CliOptions {
    std::string input;
    std::string savePath = ".";
    int maxPeers = 30;
    bool refreshTrackers = true;
    bool verbose = false;
};

// Parse CLI arguments, supporting both legacy positional args and new options.
// Returns false on parse error.
bool parseArgs(int argc, char* argv[], CliOptions& opts) {
    std::vector<std::string> positional;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            exit(0);
        } else if (arg == "-o" || arg == "--download-dir") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a directory argument" << std::endl;
                return false;
            }
            opts.savePath = argv[++i];
        } else if (arg == "--max-peers") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --max-peers requires a number" << std::endl;
                return false;
            }
            try {
                opts.maxPeers = std::stoi(argv[++i]);
            } catch (...) {
                std::cerr << "Error: invalid --max-peers value: " << argv[i] << std::endl;
                return false;
            }
            if (opts.maxPeers < 1 || opts.maxPeers > 500) {
                std::cerr << "Error: --max-peers must be between 1 and 500" << std::endl;
                return false;
            }
        } else if (arg == "--no-tracker-refresh") {
            opts.refreshTrackers = false;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Error: unknown option: " << arg << std::endl;
            return false;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.empty()) {
        printUsage(argv[0]);
        return false;
    }

    opts.input = positional[0];
    // Backward-compatible second positional argument overrides -o/--download-dir
    if (positional.size() >= 2) {
        opts.savePath = positional[1];
    }
    if (positional.size() > 2) {
        std::cerr << "Error: too many arguments" << std::endl;
        return false;
    }
    return true;
}

void printInfo(const TorrentInfo& torrent) {
    std::cout << "Torrent Information:" << std::endl;
    std::cout << "  Name:         " << torrent.name << std::endl;
    std::cout << "  Size:         " << utils::formatBytes(torrent.totalLength()) << std::endl;
    std::cout << "  Pieces:       " << torrent.numPieces()
              << " x " << utils::formatBytes(torrent.pieceLength) << std::endl;
    std::cout << "  Files:        " << (torrent.isMultiFile() ? torrent.files.size() : 1) << std::endl;
    std::cout << "  Trackers:     " << torrent.announceList.size() << std::endl;
    std::cout << "  Info Hash:    " << utils::toHex(torrent.infoHash) << std::endl;

    if (!torrent.comment.empty()) {
        std::cout << "  Comment:      " << torrent.comment << std::endl;
    }
    if (!torrent.createdBy.empty()) {
        std::cout << "  Created By:   " << torrent.createdBy << std::endl;
    }
    std::cout << std::endl;
}

// Create TorrentInfo from magnet link and downloaded metadata
TorrentInfo createTorrentFromMetadata(const MagnetLink& magnet, const std::string& metadata) {
    TorrentInfo torrent;
    torrent.infoHash = magnet.infoHashRaw;  // Raw 20-byte SHA1, not hex string
    torrent.peerId = generatePeerId();
    torrent.announceList = magnet.trackers;

    // Parse the metadata (bencoded info dictionary)
    bencode::BencodeValue root = bencode::parse(metadata);
    if (!root.isDict()) {
        throw std::runtime_error("Invalid metadata: root is not a dictionary");
    }

    const auto& infoDict = root.asDict();

    // Get piece length
    const auto* pieceLength = bencode::dictGet(infoDict, "piece length");
    if (!pieceLength || !pieceLength->isInt()) {
        throw std::runtime_error("Invalid metadata: missing piece length");
    }
    torrent.pieceLength = pieceLength->asInt();

    // Get pieces
    const auto* pieces = bencode::dictGet(infoDict, "pieces");
    if (!pieces || !pieces->isString()) {
        throw std::runtime_error("Invalid metadata: missing pieces");
    }
    torrent.pieces = std::get<bencode::BencodeString>(pieces->data);

    // Get name
    const auto* name = bencode::dictGet(infoDict, "name");
    if (!name || !name->isString()) {
        throw std::runtime_error("Invalid metadata: missing name");
    }
    torrent.name = std::get<bencode::BencodeString>(name->data);

    // Check if single file or multi-file
    const auto* files = bencode::dictGet(infoDict, "files");
    if (files && files->isList()) {
        // Multi-file mode
        for (const auto& fileEntry : files->asList()) {
            if (!fileEntry.isDict()) continue;

            const auto& fileDict = fileEntry.asDict();
            TorrentInfo::FileEntry entry;

            const auto* length = bencode::dictGet(fileDict, "length");
            if (length && length->isInt()) {
                entry.length = length->asInt();
            }

            const auto* path = bencode::dictGet(fileDict, "path");
            if (path && path->isList()) {
                std::ostringstream pathOss;
                const auto& pathList = path->asList();
                for (size_t i = 0; i < pathList.size(); i++) {
                    if (i > 0) pathOss << "/";
                    if (pathList[i].isString()) {
                        pathOss << std::get<bencode::BencodeString>(pathList[i].data);
                    }
                }
                entry.path = pathOss.str();
            }

            torrent.files.push_back(entry);
        }
    } else {
        // Single file mode
        const auto* length = bencode::dictGet(infoDict, "length");
        if (length && length->isInt()) {
            torrent.fileLength = length->asInt();
        }
        torrent.fileName = torrent.name;
    }

    // Use display name from magnet if metadata name is empty
    if (torrent.name.empty() && !magnet.displayName.empty()) {
        torrent.name = magnet.displayName;
    }

    return torrent;
}

// Check if input is a magnet link
bool isMagnetLink(const std::string& input) {
    return input.size() >= 8 && input.substr(0, 8) == "magnet:?";
}

int main(int argc, char* argv[]) {
    CliOptions opts;
    if (!parseArgs(argc, argv, opts)) {
        return 1;
    }

    // Configure logging
    utils::setLogLevel(opts.verbose ? utils::LogLevel::Debug : utils::LogLevel::Info);

    std::string input = opts.input;
    std::string savePath = opts.savePath;

    // Set up signal handler
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << "Gush - BitTorrent Client MVP" << std::endl;
    std::cout << "=============================" << std::endl;
    std::cout << std::endl;

    try {
        TorrentInfo torrent;

        // Check if input is magnet link or torrent file
        if (isMagnetLink(input)) {
            std::cout << "Parsing magnet link: " << input << std::endl;
            
            // Parse magnet link
            MagnetLink magnet = parseMagnetLink(input);
            if (!magnet.isValid()) {
                utils::logError("Invalid magnet link");
                return 1;
            }

            std::cout << "Magnet link parsed successfully" << std::endl;
            std::cout << "  Info Hash: " << magnet.infoHash << std::endl;
            if (!magnet.displayName.empty()) {
                std::cout << "  Name:      " << magnet.displayName << std::endl;
            }
            if (!magnet.trackers.empty()) {
                std::cout << "  Trackers:  " << magnet.trackers.size() << std::endl;
            }
            std::cout << std::endl;

            // Download metadata using BEP 9
            std::cout << "Downloading metadata from peers (BEP 9)..." << std::endl;
            std::string metadata = downloadMetadata(magnet);

            if (metadata.empty()) {
                utils::logError("Failed to download metadata from peers");
                utils::logError("No usable peers were found through trackers or DHT.");
                return 1;
            }

            std::cout << "Metadata downloaded successfully (" 
                      << utils::formatBytes(metadata.size()) << ")" << std::endl;
            std::cout << std::endl;

            // Create TorrentInfo from metadata
            torrent = createTorrentFromMetadata(magnet, metadata);
        } else {
            // Load torrent file
            std::cout << "Loading torrent: " << input << std::endl;
            torrent = loadTorrent(input);
        }

        // Print torrent info
        printInfo(torrent);

        // Create output directory if needed
        utils::createDirectory(savePath);

        // Build download options from CLI
        DownloadOptions options;
        options.maxPeers = opts.maxPeers;
        options.refreshTrackers = opts.refreshTrackers;
        options.verbose = opts.verbose;

        // Create downloader
        Downloader downloader(torrent, savePath, options);
        g_downloader = &downloader;

        // Start download
        std::cout << "Starting download to: " << savePath << std::endl;
        std::cout << "Connecting to tracker and peers..." << std::endl;
        std::cout << std::endl;

        downloader.start();

        // Show progress
        auto lastUpdate = std::chrono::steady_clock::now();
        while (downloader.isRunning() && !g_interrupt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastUpdate).count();

            if (elapsed >= 1) {
                DownloadStats stats = downloader.getStats();

                // Clear line and print status
                std::cout << "\r";
                std::cout << "Progress: " << std::fixed << std::setprecision(1)
                          << (stats.progress * 100.0) << "% ";
                std::cout << "Downloaded: " << utils::formatBytes(stats.downloaded);
                std::cout << " / " << utils::formatBytes(stats.totalLength) << " ";
                if (stats.currentSpeed > 0) {
                    std::cout << "Speed: " << utils::formatBytes(
                        static_cast<int64_t>(stats.currentSpeed)) << "/s ";
                } else {
                    std::cout << "Speed: stalled ";
                }
                std::cout << "Peers: " << stats.activePeers << "/" << stats.connectedPeers;
                std::cout.flush();

                lastUpdate = now;
            }

            if (downloader.isComplete()) {
                break;
            }
        }

        // Join the download thread from the main thread (never from a signal handler)
        downloader.stop();

        std::cout << std::endl;

        if (downloader.isComplete()) {
            std::cout << "Download completed successfully!" << std::endl;
            std::cout << "Files saved to: " << savePath << std::endl;
        } else {
            std::cout << "Download stopped." << std::endl;
        }

        g_downloader = nullptr;
        return 0;

    } catch (const std::exception& e) {
        utils::logError(std::string("Error: ") + e.what());
        return 1;
    }
}
