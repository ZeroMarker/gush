#ifndef TRACKER_MANAGER_H
#define TRACKER_MANAGER_H

#include "tracker.h"
#include "torrent.h"
#include <string>
#include <vector>
#include <chrono>

// Manages the tracker announce strategy (todo P1 "改进 tracker 策略"):
//  - exponential failure backoff so dead trackers are not hammered
//  - successful trackers are preferred (tried first) on the next round
//  - bounded number of attempts per cycle so a slow tracker cannot stall
//    the whole download loop
class TrackerManager {
public:
    struct TrackerState {
        std::string url;
        int consecutiveFailures = 0;
        int interval = 1800;         // last successful announce interval
        int64_t lastPeerCount = 0;
        bool everSucceeded = false;
        std::chrono::steady_clock::time_point nextAllowed{};  // backoff gate
        std::chrono::steady_clock::time_point lastAttempt{};
    };

    explicit TrackerManager(std::vector<std::string> urls = {});

    void add(const std::string& url);
    void addAll(const std::vector<std::string>& urls);
    bool empty() const { return trackers_.empty(); }
    std::size_t size() const { return trackers_.size(); }
    const std::vector<TrackerState>& states() const { return trackers_; }
    std::vector<TrackerState>& states() { return trackers_; }

    // Trackers that are currently allowed to be contacted (not in backoff),
    // ordered by priority: preferred (last successful) first, then by
    // ascending failure count, then by oldest last attempt.
    std::vector<std::string> dueTrackers() const;

    // Try due trackers in priority order until one succeeds or maxAttempts
    // attempts have been made this cycle. Reports each result to the manager
    // so backoff state stays current.
    TrackerResponse contact(const TorrentInfo& torrent,
                            const std::string& peerId,
                            int64_t downloaded,
                            int64_t uploaded,
                            int64_t left,
                            const std::string& event = "",
                            int maxAttempts = 3);

    void reportSuccess(const std::string& url, const TrackerResponse& resp);
    void reportFailure(const std::string& url);

private:
    static constexpr int MAX_BACKOFF_SECONDS = 1800;
    static constexpr int BASE_BACKOFF_SECONDS = 30;

    int backoffSeconds(int consecutiveFailures) const;

    std::vector<TrackerState> trackers_;
    // URL of the tracker that succeeded most recently (tried first).
    std::string preferred_;
};

#endif // TRACKER_MANAGER_H
