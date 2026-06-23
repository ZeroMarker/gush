#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include "torrent.h"
#include "peer.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstddef>
#include <thread>

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
    Downloader(const TorrentInfo& torrent, const std::string& savePath);
    ~Downloader();

    // Start download
    void start();

    // Stop download
    void stop();

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
    void verifyPiece(uint32_t index, const std::vector<uint8_t>& data);
    void writePiece(uint32_t index, uint32_t offset, const std::vector<uint8_t>& data);
    bool hasPiece(uint32_t index) const;
    uint32_t selectNextPiece();
    void updateSpeedStats();
    int calculateBlocksForPiece(uint32_t pieceIndex) const;
    void cleanupTimedOutRequests();
    void cancelRequestsForPeer(PeerConnection* peer);

    TorrentInfo torrent_;
    std::string savePath_;
    std::string peerId_;

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
    static const int MAX_REQUESTS_PER_PEER = 5;   // Reduced to avoid flooding peers

    // Peer speed tracking for peer scoring
    struct PeerSpeedStats {
        int64_t bytesDownloaded = 0;
        std::chrono::steady_clock::time_point lastUpdateTime;
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

    // File handle for writing
    FILE* outputFile_ = nullptr;
    
    // Mutex for thread safety
    mutable std::mutex mutex_;
};

#endif // DOWNLOADER_H
