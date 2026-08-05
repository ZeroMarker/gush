#include "tracker_manager.h"
#include "utils.h"
#include <algorithm>

TrackerManager::TrackerManager(std::vector<std::string> urls) {
    addAll(urls);
}

void TrackerManager::add(const std::string& url) {
    if (url.empty()) return;
    for (const auto& t : trackers_) {
        if (t.url == url) return;  // already tracked
    }
    TrackerState st;
    st.url = url;
    trackers_.push_back(st);
}

void TrackerManager::addAll(const std::vector<std::string>& urls) {
    for (const auto& url : urls) {
        add(url);
    }
}

int TrackerManager::backoffSeconds(int consecutiveFailures) const {
    // Exponential backoff: 30s, 60s, 120s, ... capped at 30 minutes.
    int64_t seconds = static_cast<int64_t>(BASE_BACKOFF_SECONDS)
                      << std::min(consecutiveFailures, 12);
    return static_cast<int>(std::min<int64_t>(seconds, MAX_BACKOFF_SECONDS));
}

std::vector<std::string> TrackerManager::dueTrackers() const {
    const auto now = std::chrono::steady_clock::now();

    std::vector<TrackerState> due;
    for (const auto& st : trackers_) {
        if (now >= st.nextAllowed) {
            due.push_back(st);
        }
    }

    // Priority order: preferred tracker first, then fewest failures,
    // then oldest last attempt.
    std::sort(due.begin(), due.end(),
        [this](const TrackerState& a, const TrackerState& b) {
            if (a.url == preferred_) return true;
            if (b.url == preferred_) return false;
            if (a.consecutiveFailures != b.consecutiveFailures) {
                return a.consecutiveFailures < b.consecutiveFailures;
            }
            return a.lastAttempt < b.lastAttempt;
        });

    std::vector<std::string> result;
    result.reserve(due.size());
    for (const auto& st : due) {
        result.push_back(st.url);
    }
    return result;
}

TrackerResponse TrackerManager::contact(const TorrentInfo& torrent,
                                        const std::string& peerId,
                                        int64_t downloaded,
                                        int64_t uploaded,
                                        int64_t left,
                                        const std::string& event,
                                        int maxAttempts) {
    const auto now = std::chrono::steady_clock::now();
    TrackerResponse lastResponse;
    lastResponse.failure = "No trackers available (none configured or all in backoff)";

    int attempts = 0;
    for (const auto& url : dueTrackers()) {
        if (attempts >= maxAttempts) break;

        for (auto& st : trackers_) {
            if (st.url == url) {
                st.lastAttempt = now;
                break;
            }
        }
        attempts++;

        TrackerResponse resp = contactTracker(url, torrent, peerId,
                                              downloaded, uploaded, left, event);
        if (resp.ok()) {
            reportSuccess(url, resp);
            return resp;
        }
        reportFailure(url);
        lastResponse = resp;
    }

    return lastResponse;
}

void TrackerManager::reportSuccess(const std::string& url, const TrackerResponse& resp) {
    preferred_ = url;
    for (auto& st : trackers_) {
        if (st.url != url) continue;
        st.consecutiveFailures = 0;
        st.everSucceeded = true;
        st.interval = resp.interval > 0 ? resp.interval : 1800;
        st.lastPeerCount = static_cast<int64_t>(resp.peers.size());
        st.nextAllowed = std::chrono::steady_clock::now();
        break;
    }
}

void TrackerManager::reportFailure(const std::string& url) {
    for (auto& st : trackers_) {
        if (st.url != url) continue;
        st.consecutiveFailures++;
        st.nextAllowed = std::chrono::steady_clock::now() +
            std::chrono::seconds(backoffSeconds(st.consecutiveFailures));
        break;
    }
}
