#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include "torrent.h"
#include "peer.h"
#include "tracker_manager.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <map>
#include <thread>

// Tunable download behaviour (set from CLI options, todo P3)
struct DownloadOptions {
    int maxPeers = 30;              // hard cap on concurrent peer connections
    int maxRequestsPerPeer = 5;     // pipelined block requests per peer
    bool refreshTrackers = true;    // fetch fresh tracker lists from the internet
    bool verbose = false;
    bool overwrite = false;         // discard existing output instead of resuming
};

struct DownloadStats {
    int64_t downloaded;
    int64_t uploaded;
    int64_t totalLength;
    double downloadSpeed;  // bytes/sec
    double currentSpeed;   // current instantaneous speed
    int connectedPeers;
    int activePeers;
    double progress;  // 0.0 - 1.0
};

// Pending block request
struct BlockRequest {
    uint32_t pieceIndex;
    uint32_t offset;
    uint32_t length;
    PeerConnection* peer;
    std::chrono::steady_clock::time_point requestTime;
};

// Download state per piece
struct PieceState {
    std::vector<bool> blocksRequested;
    std::vector<bool> blocksReceived;
    int totalBlocks;
    int receivedBlocks;
    
    PieceState(int blocks = 0) : totalBlocks(blocks), receivedBlocks(0) {
        blocksRequested.resize(blocks, false);
        blocksReceived.resize(blocks, false);
    }
};

class Downloader {
public:
    Downloader(const TorrentInfo& torrent, const std::string& savePath,
               const DownloadOptions& options = DownloadOptions());
    ~Downloader();

    // Start download
    void start();

    // Stop download
    void stop();

    // Request stop without joining the download thread (safe from signal handlers)
    void requestStop() { stopRequested_ = true; running_ = false; }

    // Check if downloading
    bool isRunning() const { return running_; }

    // Get stats
    DownloadStats getStats() const;

    // Get progress
    double getProgress() const;

    // Check if complete
    bool isComplete() const;

    // Add peers
    void addPeers(const std::vector<Peer>& peers);

    // Fetch latest trackers from online sources
    void fetchLatestTrackers();

private:
    void downloadLoop();
    bool connectToPeers();
    void requestPieces();
    void requestBlocksFromPeer(PeerConnection* peer);
    void processPeerMessages();
    bool verifyPiece(uint32_t index);
    void writePiece(uint32_t index, uint32_t offset, const std::vector<uint8_t>& data);
    bool readPieceFromDisk(uint32_t index, std::vector<uint8_t>& data);
    bool hasPiece(uint32_t index) const;
    uint32_t selectNextPiece();
    void updateSpeedStats();
    int calculateBlocksForPiece(uint32_t pieceIndex) const;
    int64_t pieceSize(uint32_t pieceIndex) const;
    void cleanupTimedOutRequests();
    void cancelRequestsForPeer(PeerConnection* peer);
    void cancelDuplicateRequests(uint32_t pieceIndex, uint32_t offset, PeerConnection* keep);
    void pruneBadPeers();
    bool isPeerRejected(const std::string& ip, uint16_t port) const;
    void rejectPeer(const std::string& ip, uint16_t port);

    // Output file management (single- and multi-file torrents)
    struct OutputFile {
        std::string path;        // Full path on disk
        int64_t startOffset = 0; // Absolute byte offset within the torrent
        int64_t length = 0;      // Size of this file
        FILE* handle = nullptr;
    };
    bool openOutputFiles();
    void restoreCompletedPieces();
    void closeOutputFiles();
    static bool resizeFile(FILE* f, int64_t size);

    TorrentInfo torrent_;
    std::string savePath_;
    std::string peerId_;
    DownloadOptions options_;
    TrackerManager trackerManager_;

    std::vector<std::unique_ptr<PeerConnection>> peers_;
    std::vector<bool> piecesCompleted_;
    std::vector<PieceState> pieceStates_;
    std::size_t piecesCompletedCount_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread downloadThread_;

    int64_t downloadedBytes_ = 0;
    int64_t uploadedBytes_ = 0;
    
    // Speed tracking
    int64_t lastDownloadedBytes_ = 0;
    std::chrono::steady_clock::time_point lastSpeedUpdateTime_;
    double currentSpeed_ = 0.0;

    // Pending requests tracking
    std::vector<BlockRequest> pendingRequests_;
    static const int MAX_PENDING_REQUESTS = 100;  // Reduced to prevent overwhelming peers
    static const int REQUEST_TIMEOUT_MS = 15000;  // Increased timeout for slower peers

    // Peer speed tracking for peer scoring
    struct PeerSpeedStats {
        int64_t bytesDownloaded = 0;
        std::chrono::steady_clock::time_point lastUpdateTime;
        std::chrono::steady_clock::time_point connectedAt;
        double avgSpeed = 0.0;  // bytes/sec
        int successfulRequests = 0;
        int failedRequests = 0;
        
        void updateSpeed(int64_t bytes) {
            bytesDownloaded += bytes;
            successfulRequests++;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - lastUpdateTime).count();
            if (elapsed > 0) {
                avgSpeed = static_cast<double>(bytesDownloaded) / elapsed;
            }
        }
    };
    std::map<PeerConnection*, PeerSpeedStats> peerSpeedStats_;

    // Recently rejected peers (ip:port) so we do not hot-reconnect bad peers
    mutable std::map<std::string, std::chrono::steady_clock::time_point> recentlyRejected_;
    static constexpr int REJECT_COOLDOWN_SECONDS = 300;

    // Endgame mode: near completion, duplicate block requests are allowed
    bool endgame_ = false;
    static constexpr int ENDGAME_BLOCK_THRESHOLD = 64;

    // Output file handles (single- or multi-file)
    std::vector<OutputFile> outputFiles_;
    
    // Mutex for thread safety
    mutable std::mutex mutex_;
};

#endif // DOWNLOADER_H
