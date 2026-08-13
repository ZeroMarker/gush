#include "downloader.h"
#include "tracker.h"
#include "tracker_list.h"
#include "dht.h"
#include "utils.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <queue>
#include <random>
#include <mutex>
#include <condition_variable>
#include <future>
#include <map>
#include <set>
#include <sstream>

Downloader::Downloader(const TorrentInfo& torrent, const std::string& savePath,
                       const DownloadOptions& options)
    : torrent_(torrent), savePath_(savePath), peerId_(torrent.peerId),
      options_(options), trackerManager_(torrent.announceList),
      lastSpeedUpdateTime_(std::chrono::steady_clock::now()) {

    piecesCompleted_.resize(torrent.numPieces(), false);
    pieceStates_.reserve(torrent.numPieces());
    for (std::size_t i = 0; i < torrent.numPieces(); i++) {
        pieceStates_.emplace_back(calculateBlocksForPiece(static_cast<uint32_t>(i)));
    }
}

Downloader::~Downloader() {
    stop();
    closeOutputFiles();
}

void Downloader::start() {
    if (running_) return;

    running_ = true;
    stopRequested_ = false;

    // Create the save directory if it does not exist yet
    if (!utils::createDirectories(savePath_)) {
        utils::logError("Failed to create output directory: " + savePath_);
        running_ = false;
        return;
    }

    // Open output file(s): single file or one per FileEntry
    if (!openOutputFiles()) {
        utils::logError("Failed to create output files under: " + savePath_);
        closeOutputFiles();
        running_ = false;
        return;
    }

    // Start download loop in separate thread
    downloadThread_ = std::thread(&Downloader::downloadLoop, this);
}

bool Downloader::preallocateFile(FILE* f, int64_t size) {
    if (size <= 0) return true;
    return fseeko(f, size - 1, SEEK_SET) == 0 &&
           fputc(0, f) != EOF &&
           fflush(f) == 0 &&
           fseeko(f, 0, SEEK_SET) == 0;
}

bool Downloader::openOutputFiles() {
    closeOutputFiles();

    const int64_t total = torrent_.totalLength();
    if (total < 0) return false;

    if (!torrent_.isMultiFile()) {
        // Single file mode: savePath/name
        const std::string safeName = utils::sanitizeFileName(
            torrent_.name.empty() ? torrent_.fileName : torrent_.name);
        const std::string outputPath = savePath_ + "/" + safeName;

        OutputFile f;
        f.path = outputPath;
        f.startOffset = 0;
        f.length = total;
        // "w+b": read/write mode. verifyPiece() re-reads each piece from this
        // same FILE* to check its SHA1, so a write-only "wb" handle would make
        // every verification fail (fread on a write-only stream is UB).
        f.handle = fopen(outputPath.c_str(), "w+b");
        if (!f.handle) return false;
        if (!preallocateFile(f.handle, f.length)) {
            fclose(f.handle);
            return false;
        }
        outputFiles_.push_back(f);
        return true;
    }

    // Multi-file mode: create directories and one file per FileEntry
    int64_t runningOffset = 0;
    for (const auto& fileEntry : torrent_.files) {
        const std::string relPath = utils::sanitizePath(fileEntry.path);
        const std::string outputPath = savePath_ + "/" + relPath;

        // Create parent directories (may be nested)
        const size_t slash = outputPath.find_last_of('/');
        if (slash != std::string::npos && !utils::createDirectories(outputPath.substr(0, slash))) {
            return false;
        }

        OutputFile f;
        f.path = outputPath;
        f.startOffset = runningOffset;
        f.length = fileEntry.length;
        // "w+b": read/write mode so verifyPiece() can re-read the piece.
        f.handle = fopen(outputPath.c_str(), "w+b");
        if (!f.handle) return false;
        if (!preallocateFile(f.handle, f.length)) {
            fclose(f.handle);
            return false;
        }
        outputFiles_.push_back(f);
        runningOffset += fileEntry.length;
    }
    return true;
}

void Downloader::closeOutputFiles() {
    for (auto& f : outputFiles_) {
        if (f.handle) {
            fclose(f.handle);
            f.handle = nullptr;
        }
    }
    outputFiles_.clear();
}

void Downloader::stop() {
    stopRequested_ = true;
    running_ = false;

    // Disconnect all peers
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& peer : peers_) {
            if (peer) {
                peer->disconnect();
            }
        }
    }

    if (downloadThread_.joinable() &&
        downloadThread_.get_id() != std::this_thread::get_id()) {
        downloadThread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        peers_.clear();
    }
}

void Downloader::downloadLoop() {
    auto lastTime = std::chrono::steady_clock::now();
    auto lastTrackerFetch = std::chrono::steady_clock::now();
    auto lastPrune = std::chrono::steady_clock::now();
    const int TRACKER_FETCH_INTERVAL = 300;  // Fetch new trackers every 5 minutes
    const int PRUNE_INTERVAL = 10;           // Peer health check every 10 seconds

    // Fetch latest trackers from online sources
    if (options_.refreshTrackers) {
        fetchLatestTrackers();
    }

    // Contact tracker initially
    utils::logInfo("Contacting tracker...");
    TrackerResponse trackerResp = trackerManager_.contact(
        torrent_, peerId_, downloadedBytes_, uploadedBytes_,
        torrent_.totalLength() - downloadedBytes_, "started", 3);

    if (trackerResp.ok()) {
        utils::logInfo("Tracker response: " + std::to_string(trackerResp.peers.size()) +
                       " peers, " + std::to_string(trackerResp.complete) +
                       " seeders, " + std::to_string(trackerResp.incomplete) + " leechers");
        addPeers(trackerResp.peers);
    } else {
        utils::logWarn("Tracker error: " + trackerResp.failure);
    }

    // Fall back to BEP 5 when neither callers nor trackers supplied a peer.
    // This keeps pure-hash magnets usable after their metadata has been fetched.
    bool haveKnownPeers = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        haveKnownPeers = !peers_.empty();
    }
    if (!haveKnownPeers) {
        DhtOptions dhtOptions;
        dhtOptions.queryTimeoutMs = 600;
        dhtOptions.maxQueries = 16;
        dhtOptions.maxPeers = static_cast<std::size_t>(options_.maxPeers) * 2;
        auto dhtPeers = discoverDhtPeers(torrent_.infoHash, {}, dhtOptions);
        if (!dhtPeers.empty()) {
            utils::logInfo("DHT discovered " + std::to_string(dhtPeers.size()) + " peers");
            addPeers(dhtPeers);
        }
    }

    int idleCounter = 0;
    // Idle detection uses its own snapshot so it does not clobber the
    // speed-stats sample in updateSpeedStats() (lastDownloadedBytes_).
    int64_t loopSampleDownloaded = downloadedBytes_;

    while (running_ && !stopRequested_) {
        // Connect to peers
        connectToPeers();

        // Process incoming messages from all peers (non-blocking)
        processPeerMessages();

        // Request blocks from peers that are not choking
        requestPieces();

        // Clean up timed-out requests
        cleanupTimedOutRequests();

        // Update speed stats
        updateSpeedStats();

        // Endgame mode: once few blocks remain, allow duplicate requests so
        // the slowest peer cannot stall the final stretch (BEP 3 endgame)
        {
            int remainingBlocks = 0;
            for (std::size_t i = 0; i < torrent_.numPieces(); i++) {
                if (piecesCompleted_[i]) continue;
                remainingBlocks += pieceStates_[i].totalBlocks - pieceStates_[i].receivedBlocks;
            }
            endgame_ = remainingBlocks <= ENDGAME_BLOCK_THRESHOLD;
        }

        // Periodically evict unhealthy peers
        auto nowPrune = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(nowPrune - lastPrune).count() >= PRUNE_INTERVAL) {
            pruneBadPeers();
            lastPrune = nowPrune;
        }

        // Check if complete
        if (isComplete()) {
            utils::logInfo("Download complete!");
            break;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime).count();
        auto elapsedSinceFetch = std::chrono::duration_cast<std::chrono::seconds>(now - lastTrackerFetch).count();

        int activePeerCount = 0;
        for (const auto& peer : peers_) {
            if (peer && peer->isConnected()) activePeerCount++;
        }

        // Fetch new trackers periodically
        if (options_.refreshTrackers && elapsedSinceFetch >= TRACKER_FETCH_INTERVAL) {
            utils::logInfo("Refreshing tracker list...");
            fetchLatestTrackers();
            lastTrackerFetch = now;
        }

        // Re-contact tracker if interval elapsed or we need more peers
        bool needMorePeers = activePeerCount < 10;
        bool trackerDue = (elapsed >= trackerResp.interval && trackerResp.ok()) ||
                          (needMorePeers && elapsed >= 30) || idleCounter >= 10;
        if (trackerDue) {
            trackerResp = trackerManager_.contact(
                torrent_, peerId_, downloadedBytes_, uploadedBytes_,
                torrent_.totalLength() - downloadedBytes_, "", 3);
            if (trackerResp.ok()) {
                utils::logInfo("Tracker update: " + std::to_string(trackerResp.peers.size()) + " new peers");
                addPeers(trackerResp.peers);
            }
            lastTime = now;
            idleCounter = 0;
        }

        // Check for idle state (no progress) - if tracker re-contact above did not
        // help, keep trying through the manager's backoff schedule
        if (downloadedBytes_ == loopSampleDownloaded) {
            idleCounter++;
        } else {
            idleCounter = 0;
        }
        loopSampleDownloaded = downloadedBytes_;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Increased from 20ms to reduce CPU usage
    }

    // Send stopped event to tracker (best effort, no backoff)
    if (!torrent_.announceList.empty()) {
        for (const auto& url : torrent_.announceList) {
            contactTracker(url, torrent_, peerId_, downloadedBytes_, uploadedBytes_,
                           torrent_.totalLength() - downloadedBytes_, "stopped");
        }
    }

    running_ = false;
}

bool Downloader::connectToPeers() {
    // Limit concurrent connections - reduced for better stability
    const int maxPeers = options_.maxPeers;
    const int minPeers = 5;   // Reduced minimum active connections

    int connectedCount = 0;
    for (const auto& peer : peers_) {
        if (peer && peer->isConnected()) {
            connectedCount++;
        }
    }

    // Sort peers by speed score (prefer faster peers)
    std::sort(peers_.begin(), peers_.end(),
        [this](const std::unique_ptr<PeerConnection>& a, const std::unique_ptr<PeerConnection>& b) {
            auto itA = peerSpeedStats_.find(a.get());
            auto itB = peerSpeedStats_.find(b.get());
            double speedA = (itA != peerSpeedStats_.end()) ? itA->second.avgSpeed : 0.0;
            double speedB = (itB != peerSpeedStats_.end()) ? itB->second.avgSpeed : 0.0;
            return speedA > speedB;  // Faster peers first
        });

    // Connect to more peers if we have fewer than minimum
    for (const auto& peer : peers_) {
        if (!peer || peer->isConnected()) continue;
        if (connectedCount >= maxPeers) break;

        // Skip peers we recently rejected (avoid hot reconnection loops)
        if (isPeerRejected(peer->ip(), peer->port())) {
            continue;
        }

        if (peer->connect()) {
            connectedCount++;
            // Send optimistic unchoke to new peer (BEP 3)
            // This tells the peer we're willing to send data (if we had any)
            peer->sendUnchoke();
            // Start tracking connection time for health checks
            peerSpeedStats_[peer.get()].connectedAt = std::chrono::steady_clock::now();
            peerSpeedStats_[peer.get()].lastUpdateTime = std::chrono::steady_clock::now();
            utils::logDebug("Connected to peer " + peer->ip() + ":" + std::to_string(peer->port()));
        } else {
            // Peer failed to connect - cool down briefly
            rejectPeer(peer->ip(), peer->port());
        }
    }
    return connectedCount >= minPeers;
}

void Downloader::requestPieces() {
    // Count requests per peer
    std::map<PeerConnection*, int> requestsPerPeer;
    for (const auto& req : pendingRequests_) {
        requestsPerPeer[req.peer]++;
    }

    // Sort peers by speed (prefer faster peers)
    std::vector<PeerConnection*> sortedPeers;
    for (auto& peerPtr : peers_) {
        if (peerPtr && peerPtr->isConnected() && !peerPtr->peerChoking()) {
            sortedPeers.push_back(peerPtr.get());
        }
    }
    std::sort(sortedPeers.begin(), sortedPeers.end(),
        [this, &requestsPerPeer](PeerConnection* a, PeerConnection* b) {
            auto itA = peerSpeedStats_.find(a);
            auto itB = peerSpeedStats_.find(b);
            double speedA = (itA != peerSpeedStats_.end()) ? itA->second.avgSpeed : 0.0;
            double speedB = (itB != peerSpeedStats_.end()) ? itB->second.avgSpeed : 0.0;
            // Prefer peers with fewer pending requests if speeds are similar
            int reqA = requestsPerPeer[a];
            int reqB = requestsPerPeer[b];
            if (std::abs(speedA - speedB) < 1024.0) {  // Within 1KB/s
                return reqA < reqB;
            }
            return speedA > speedB;
        });

    const int maxRequestsPerPeer = options_.maxRequestsPerPeer;
    for (auto* peer : sortedPeers) {
        // Skip if this peer already has too many pending requests
        if (requestsPerPeer[peer] >= maxRequestsPerPeer) continue;
        requestBlocksFromPeer(peer);
    }
}

void Downloader::requestBlocksFromPeer(PeerConnection* peer) {
    if (!peer || !peer->isConnected() || peer->peerChoking()) return;

    const uint32_t blockSize = 16384;  // 16 KB
    const int maxRequestsPerPeer = options_.maxRequestsPerPeer;

    // Count current requests for this peer
    int currentRequests = 0;
    for (const auto& req : pendingRequests_) {
        if (req.peer == peer) currentRequests++;
    }

    if (currentRequests >= maxRequestsPerPeer) return;

    // Find pieces this peer has that we need
    std::vector<uint32_t> neededPieces;
    for (std::size_t i = 0; i < torrent_.numPieces(); i++) {
        if (piecesCompleted_[i]) continue;
        if (!peer->hasPiece(static_cast<uint32_t>(i))) continue;

        // Check if we're already downloading all blocks of this piece
        if (pieceStates_[i].receivedBlocks < pieceStates_[i].totalBlocks) {
            neededPieces.push_back(static_cast<uint32_t>(i));
        }
    }

    if (neededPieces.empty()) return;

    // Select the best piece using rarest-first with partial completion priority
    uint32_t selectedPiece = selectNextPiece();
    if (selectedPiece == UINT32_MAX || !peer->hasPiece(selectedPiece)) {
        // Fallback to any available piece from this peer
        for (uint32_t p : neededPieces) {
            if (peer->hasPiece(p)) {
                selectedPiece = p;
                break;
            }
        }
    }

    if (selectedPiece == UINT32_MAX) return;

    // Request missing blocks for this piece
    PieceState& state = pieceStates_[selectedPiece];
    int64_t pieceStart = static_cast<int64_t>(selectedPiece) * torrent_.pieceLength;
    int64_t pieceEnd = std::min(pieceStart + torrent_.pieceLength, torrent_.totalLength());
    int64_t pieceSize = pieceEnd - pieceStart;

    for (int i = 0; i < state.totalBlocks && currentRequests < maxRequestsPerPeer; i++) {
        if (state.blocksReceived[i]) continue;

        if (state.blocksRequested[i]) {
            // Block is in flight somewhere.
            bool requestedFromPeer = false;
            int dupCount = 0;
            for (const auto& req : pendingRequests_) {
                if (req.pieceIndex == selectedPiece && req.offset == i * blockSize) {
                    if (req.peer == peer) {
                        requestedFromPeer = true;
                        break;
                    }
                    dupCount++;
                }
            }
            if (!endgame_) continue;        // Normal mode: never duplicate
            if (requestedFromPeer) continue; // Endgame: never twice from same peer
            if (dupCount >= 2) continue;    // Endgame: cap duplicates per block
        }

        uint32_t offset = i * blockSize;
        uint32_t blockLen = std::min(static_cast<int64_t>(blockSize), pieceSize - offset);

        if (peer->sendRequest(selectedPiece, offset, blockLen)) {
            state.blocksRequested[i] = true;

            BlockRequest req;
            req.pieceIndex = selectedPiece;
            req.offset = offset;
            req.length = blockLen;
            req.peer = peer;
            req.requestTime = std::chrono::steady_clock::now();
            pendingRequests_.push_back(req);

            currentRequests++;
        } else {
            break;  // Peer can't handle more requests
        }
    }
}

uint32_t Downloader::selectNextPiece() {
    // Rarest-first strategy with partial completion priority
    // 1. First, prefer pieces that are partially downloaded (to avoid wasting bandwidth)
    // 2. Among those, select the one with highest completion percentage
    // 3. If no partial pieces, use rarest-first based on peer availability

    uint32_t bestPiece = UINT32_MAX;
    int bestScore = -1;

    // Count how many peers have each piece
    std::vector<int> peerCount(torrent_.numPieces(), 0);
    for (const auto& peerPtr : peers_) {
        if (!peerPtr || !peerPtr->isConnected()) continue;
        for (std::size_t i = 0; i < torrent_.numPieces(); i++) {
            if (!piecesCompleted_[i] && peerPtr->hasPiece(static_cast<uint32_t>(i))) {
                peerCount[i]++;
            }
        }
    }

    for (std::size_t i = 0; i < torrent_.numPieces(); i++) {
        if (piecesCompleted_[i]) continue;

        const PieceState& state = pieceStates_[i];
        
        // Skip pieces with no progress unless no partial pieces exist
        if (state.receivedBlocks == 0) continue;

        // Score based on:
        // 1. Completion percentage (higher = better, to finish pieces quickly)
        // 2. Rarity (fewer peers = higher priority)
        int completionPct = state.receivedBlocks * 1000 / state.totalBlocks;
        int rarityScore = (peerCount[i] > 0) ? (1000 / peerCount[i]) : 10000;
        
        // Combined score: completion is primary, rarity is secondary
        int score = completionPct * 100 + rarityScore;

        if (score > bestScore) {
            bestScore = score;
            bestPiece = static_cast<uint32_t>(i);
        }
    }

    // If no partial pieces, select based on rarest-first
    if (bestPiece == UINT32_MAX) {
        int minPeers = INT32_MAX;
        for (std::size_t i = 0; i < torrent_.numPieces(); i++) {
            if (piecesCompleted_[i]) continue;
            if (peerCount[i] < minPeers) {
                minPeers = peerCount[i];
                bestPiece = static_cast<uint32_t>(i);
            }
        }
    }

    return bestPiece;
}

void Downloader::processPeerMessages() {
    const uint32_t blockSize = 16384;
    std::vector<Peer> pexPeers;

    for (auto& peerPtr : peers_) {
        if (!peerPtr || !peerPtr->isConnected()) continue;

        PeerConnection* peer = peerPtr.get();

        // Try to receive and process messages (non-blocking)
        PeerMessage msg;
        while (peer->receiveMessageNonBlocking(msg)) {
            switch (msg.id) {
                case MessageId::Choke:
                    // Peer is choking, cancel pending requests to this peer
                    cancelRequestsForPeer(peer);
                    // Mark peer as slow on choke
                    peerSpeedStats_[peer].failedRequests++;
                    break;

                case MessageId::Unchoke:
                    // Peer is unchoking, can request more blocks
                    // Reset peer speed stats on unchoke
                    peerSpeedStats_[peer].lastUpdateTime = std::chrono::steady_clock::now();
                    peerSpeedStats_[peer].bytesDownloaded = 0;
                    break;

                case MessageId::Have:
                    if (msg.payload.size() >= 4) {
                        // uint32_t pieceIndex = utils::bytesToInt(msg.payload.data());
                        // Update peer's bitfield (handled in peer.cpp)
                    }
                    break;

                case MessageId::KeepAlive:
                    // Keep-alive message received, connection is still active
                    // No action needed, just acknowledge
                    break;

                case MessageId::Bitfield:
                    // Bitfield already handled in peer.cpp
                    // Peer's bitfield is updated, we can now request pieces
                    break;

                case MessageId::Block: {
                    // Parse block response
                    if (msg.payload.size() < 8) break;

                    uint32_t pieceIndex = utils::bytesToInt(msg.payload.data());
                    uint32_t offset = utils::bytesToInt(msg.payload.data() + 4);

                    // Find matching request
                    auto it = std::find_if(pendingRequests_.begin(), pendingRequests_.end(),
                        [pieceIndex, offset, peer](const BlockRequest& req) {
                            return req.pieceIndex == pieceIndex &&
                                   req.offset == offset &&
                                   req.peer == peer;
                        });

                    if (it != pendingRequests_.end()) {
                        // Extract block data
                        std::vector<uint8_t> blockData(msg.payload.begin() + 8, msg.payload.end());

                        // Protocol check: block must match the requested length
                        if (blockData.size() != it->length) {
                            PieceState& state = pieceStates_[pieceIndex];
                            int blockIndex = offset / blockSize;
                            if (blockIndex < state.totalBlocks) {
                                state.blocksRequested[blockIndex] = false;
                            }
                            peerSpeedStats_[peer].failedRequests++;
                            pendingRequests_.erase(it);
                            break;
                        }

                        // Validate block index before writing
                        PieceState& state = pieceStates_[pieceIndex];
                        int blockIndex = offset / blockSize;
                        if (blockIndex >= state.totalBlocks ||
                            state.blocksReceived[blockIndex]) {
                            // Unknown or duplicate block: drop it
                            peerSpeedStats_[peer].failedRequests++;
                            pendingRequests_.erase(it);
                            break;
                        }

                        // Consume the fulfilled request *before* any further vector
                        // mutation: later hash-failure cleanup may erase entries of
                        // this piece, which would otherwise invalidate `it`.
                        pendingRequests_.erase(it);

                        // Write piece (may span multiple files)
                        writePiece(pieceIndex, offset, blockData);
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            downloadedBytes_ += blockData.size();
                        }

                        // Update peer speed stats
                        peerSpeedStats_[peer].updateSpeed(blockData.size());

                        // Mark block as received
                        state.blocksReceived[blockIndex] = true;
                        state.receivedBlocks++;

                        // Check if piece is complete
                        if (state.receivedBlocks >= state.totalBlocks) {
                            if (verifyPiece(pieceIndex)) {
                                {
                                    std::lock_guard<std::mutex> lock(mutex_);
                                    piecesCompleted_[pieceIndex] = true;
                                    piecesCompletedCount_++;
                                }
                                utils::logDebug("Piece " + std::to_string(pieceIndex) + " complete (" +
                                                std::to_string(piecesCompletedCount_) + "/" +
                                                std::to_string(torrent_.numPieces()) + ")");
                            } else {
                                // SHA1 mismatch: reset the piece so it gets re-requested
                                utils::logWarn("Piece " + std::to_string(pieceIndex) +
                                               " failed hash check, re-requesting");
                                state.blocksRequested.assign(state.totalBlocks, false);
                                state.blocksReceived.assign(state.totalBlocks, false);
                                state.receivedBlocks = 0;
                                // Drop any still-pending requests for this piece so
                                // late-arriving stale blocks cannot mix into the re-download
                                for (auto pr = pendingRequests_.begin();
                                     pr != pendingRequests_.end(); ) {
                                    if (pr->pieceIndex == pieceIndex) {
                                        pr = pendingRequests_.erase(pr);
                                    } else {
                                        ++pr;
                                    }
                                }
                                // The corrupted data must not be counted towards progress
                                {
                                    std::lock_guard<std::mutex> lock(mutex_);
                                    downloadedBytes_ = std::max<int64_t>(0,
                                        downloadedBytes_ - pieceSize(pieceIndex));
                                }
                            }
                        }

                        // Cancel endgame duplicates of the same block from other peers
                        cancelDuplicateRequests(pieceIndex, offset, peer);
                    }
                    break;
                }

                default:
                    break;
            }
        }

        auto discovered = peer->takeDiscoveredPeers();
        pexPeers.insert(pexPeers.end(), discovered.begin(), discovered.end());
    }

    // Adding peers can reallocate peers_, so defer it until iteration finishes.
    if (!pexPeers.empty()) {
        utils::logDebug("PEX discovered " + std::to_string(pexPeers.size()) + " peers");
        addPeers(pexPeers);
    }
}

void Downloader::cleanupTimedOutRequests() {
    auto now = std::chrono::steady_clock::now();

    auto it = pendingRequests_.begin();
    while (it != pendingRequests_.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->requestTime).count();

        if (elapsed > REQUEST_TIMEOUT_MS) {
            // Reset block request flag
            PieceState& state = pieceStates_[it->pieceIndex];
            int blockIndex = it->offset / 16384;
            if (blockIndex < state.totalBlocks) {
                state.blocksRequested[blockIndex] = false;
            }

            // Mark peer request as failed
            peerSpeedStats_[it->peer].failedRequests++;

            it = pendingRequests_.erase(it);
        } else {
            ++it;
        }
    }
}

void Downloader::cancelRequestsForPeer(PeerConnection* peer) {
    for (auto it = pendingRequests_.begin(); it != pendingRequests_.end(); ) {
        if (it->peer == peer) {
            // Reset block request flag
            PieceState& state = pieceStates_[it->pieceIndex];
            int blockIndex = it->offset / 16384;
            if (blockIndex < state.totalBlocks) {
                state.blocksRequested[blockIndex] = false;
            }
            it = pendingRequests_.erase(it);
        } else {
            ++it;
        }
    }
}

void Downloader::cancelDuplicateRequests(uint32_t pieceIndex, uint32_t offset,
                                         PeerConnection* keep) {
    // Endgame mode may have the same block pending from several peers; once one
    // copy arrives, tell the others to stop and drop their pending entries.
    for (auto it = pendingRequests_.begin(); it != pendingRequests_.end(); ) {
        if (it->pieceIndex == pieceIndex && it->offset == offset && it->peer != keep) {
            it->peer->sendCancel(pieceIndex, offset, it->length);

            PieceState& state = pieceStates_[pieceIndex];
            int blockIndex = offset / 16384;
            if (blockIndex < state.totalBlocks) {
                state.blocksRequested[blockIndex] = false;
            }
            it = pendingRequests_.erase(it);
        } else {
            ++it;
        }
    }
}

void Downloader::pruneBadPeers() {
    const auto now = std::chrono::steady_clock::now();

    for (auto& peerPtr : peers_) {
        if (!peerPtr || !peerPtr->isConnected()) continue;

        PeerConnection* peer = peerPtr.get();
        const PeerSpeedStats& stats = peerSpeedStats_[peer];

        bool bad = false;
        // Peer that only ever fails requests (bad data, protocol abuse)
        if (stats.failedRequests >= 5 && stats.successfulRequests == 0) {
            bad = true;
        }
        // Peer connected for over a minute that never delivered anything
        if (!bad && stats.successfulRequests == 0) {
            auto connectedSecs = std::chrono::duration_cast<std::chrono::seconds>(
                now - stats.connectedAt).count();
            if (stats.connectedAt.time_since_epoch().count() > 0 && connectedSecs > 60) {
                bad = true;
            }
        }

        if (bad) {
            utils::logDebug("Evicting unproductive peer " + peer->ip() + ":" +
                            std::to_string(peer->port()));
            peer->disconnect();
            rejectPeer(peer->ip(), peer->port());
        }
    }
}

bool Downloader::isPeerRejected(const std::string& ip, uint16_t port) const {
    const auto now = std::chrono::steady_clock::now();
    const std::string key = ip + ":" + std::to_string(port);

    auto it = recentlyRejected_.find(key);
    if (it == recentlyRejected_.end()) return false;

    if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count()
        > REJECT_COOLDOWN_SECONDS) {
        recentlyRejected_.erase(key);
        return false;
    }
    return true;
}

void Downloader::rejectPeer(const std::string& ip, uint16_t port) {
    const std::string key = ip + ":" + std::to_string(port);
    recentlyRejected_[key] = std::chrono::steady_clock::now();
}

void Downloader::updateSpeedStats() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - lastSpeedUpdateTime_).count();
    
    if (elapsed >= 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t delta = downloadedBytes_ - lastDownloadedBytes_;
        currentSpeed_ = static_cast<double>(delta) / elapsed;
        lastSpeedUpdateTime_ = now;
        lastDownloadedBytes_ = downloadedBytes_;
    }
}

int Downloader::calculateBlocksForPiece(uint32_t pieceIndex) const {
    const uint32_t blockSize = 16384;  // 16 KB
    int64_t bytesInPiece = this->pieceSize(pieceIndex);
    return (bytesInPiece + blockSize - 1) / blockSize;
}

int64_t Downloader::pieceSize(uint32_t pieceIndex) const {
    int64_t start = static_cast<int64_t>(pieceIndex) * torrent_.pieceLength;
    return std::min<int64_t>(torrent_.pieceLength, torrent_.totalLength() - start);
}

void Downloader::writePiece(uint32_t index, uint32_t offset, const std::vector<uint8_t>& data) {
    if (outputFiles_.empty()) return;

    int64_t absOffset = static_cast<int64_t>(index) * torrent_.pieceLength + offset;
    size_t pos = 0;
    size_t fi = 0;

    // Blocks may straddle file boundaries in multi-file torrents
    while (pos < data.size() && fi < outputFiles_.size()) {
        const OutputFile& f = outputFiles_[fi];
        if (absOffset >= f.startOffset + f.length) {
            ++fi;  // This file is entirely before the write position
            continue;
        }
        if (absOffset < f.startOffset) {
            return;  // Write position is before the first file (should not happen)
        }

        int64_t within = absOffset - f.startOffset;
        int64_t avail = f.length - within;
        if (avail <= 0) {
            ++fi;
            continue;
        }

        size_t chunk = static_cast<size_t>(std::min<int64_t>(data.size() - pos, avail));
        if (fseeko(f.handle, within, SEEK_SET) != 0) return;
        if (fwrite(data.data() + pos, 1, chunk, f.handle) != chunk) return;

        pos += chunk;
        absOffset += static_cast<int64_t>(chunk);
    }
}

bool Downloader::readPieceFromDisk(uint32_t index, std::vector<uint8_t>& data) {
    if (outputFiles_.empty()) return false;

    const int64_t pieceBytes = pieceSize(index);
    if (pieceBytes <= 0) return false;
    data.assign(static_cast<size_t>(pieceBytes), 0);

    int64_t absOffset = static_cast<int64_t>(index) * torrent_.pieceLength;
    size_t pos = 0;
    size_t fi = 0;

    while (pos < data.size() && fi < outputFiles_.size()) {
        const OutputFile& f = outputFiles_[fi];
        if (absOffset >= f.startOffset + f.length) {
            ++fi;
            continue;
        }
        if (absOffset < f.startOffset) return false;

        int64_t within = absOffset - f.startOffset;
        int64_t avail = f.length - within;
        if (avail <= 0) {
            ++fi;
            continue;
        }

        size_t chunk = static_cast<size_t>(std::min<int64_t>(data.size() - pos, avail));
        if (fseeko(f.handle, within, SEEK_SET) != 0) return false;
        if (fread(data.data() + pos, 1, chunk, f.handle) != chunk) return false;

        pos += chunk;
        absOffset += static_cast<int64_t>(chunk);
    }

    return pos == data.size();
}

bool Downloader::verifyPiece(uint32_t index) {
    // Read the piece back from disk and check its SHA1 against the torrent
    std::vector<uint8_t> data;
    if (!readPieceFromDisk(index, data)) return false;

    const std::string& expected = torrent_.pieces;
    const size_t hashOffset = static_cast<size_t>(index) * 20;
    if (expected.size() < hashOffset + 20) return false;

    std::string actual = utils::sha1(data.data(), data.size());
    return actual == expected.substr(hashOffset, 20);
}

bool Downloader::hasPiece(uint32_t index) const {
    return index < piecesCompleted_.size() && piecesCompleted_[index];
}

bool Downloader::isComplete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return piecesCompletedCount_ >= torrent_.numPieces();
}

double Downloader::getProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Progress is byte-based: the last piece may be smaller than pieceLength,
    // so counting completed pieces under- or over-reports the real progress
    // (todo P0: "修复下载完成统计口径").
    const int64_t total = torrent_.totalLength();
    if (total <= 0) return 0.0;
    return std::min(1.0, static_cast<double>(downloadedBytes_) / total);
}

DownloadStats Downloader::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    DownloadStats stats;
    stats.downloaded = downloadedBytes_;
    stats.uploaded = uploadedBytes_;
    stats.totalLength = torrent_.totalLength();
    stats.downloadSpeed = currentSpeed_;
    stats.currentSpeed = currentSpeed_;
    stats.progress = getProgress();
    stats.connectedPeers = static_cast<int>(peers_.size());
    stats.activePeers = 0;

    for (const auto& peer : peers_) {
        if (peer && peer->isConnected()) {
            stats.activePeers++;
        }
    }

    return stats;
}

void Downloader::addPeers(const std::vector<Peer>& peers) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& peer : peers) {
        // Skip peers we recently rejected for misbehaviour
        if (isPeerRejected(peer.ip, peer.port)) continue;

        // Check if already connected
        bool exists = false;
        for (const auto& existing : peers_) {
            if (existing && existing->ip() == peer.ip && existing->port() == peer.port) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            peers_.push_back(std::make_unique<PeerConnection>(
                peer.ip, peer.port, torrent_, peerId_
            ));
        }
    }
}

void Downloader::fetchLatestTrackers() {
    utils::logInfo("Fetching latest tracker list from online sources...");
    
    std::vector<std::string> newTrackers = TrackerList::fetchTrackersFromMultipleSources({
        TrackerList::Source::TRACKERSLIST_ALL,
        TrackerList::Source::NGOSANG_ALL
    });
    
    if (newTrackers.empty()) {
        utils::logWarn("Failed to fetch online tracker list");
        return;
    }
    
    utils::logDebug("Fetched " + std::to_string(newTrackers.size()) + " trackers");
    
    // Merge with existing announce list, avoiding duplicates
    std::set<std::string> existingTrackers(torrent_.announceList.begin(), torrent_.announceList.end());
    
    for (const auto& tracker : newTrackers) {
        if (existingTrackers.find(tracker) == existingTrackers.end()) {
            torrent_.announceList.push_back(tracker);
            trackerManager_.add(tracker);
            existingTrackers.insert(tracker);
        }
    }
    
    utils::logDebug("Total trackers available: " + std::to_string(torrent_.announceList.size()));
}
