#ifndef TRACKER_LIST_H
#define TRACKER_LIST_H

#include <string>
#include <vector>
#include <map>

// Fetch latest tracker list from online sources
namespace TrackerList {

    // Tracker source URLs
    enum class Source {
        TRACKERSLIST_ALL,      // https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/all.txt
        TRACKERSLIST_BEST,     // https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/best.txt
        TRACKERSLIST_HTTP,     // https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/http.txt
        TRACKERSLIST_HTTPS,    // https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/https.txt
        TRACKERSLIST_UDP,      // https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/udp.txt
        TRACKERSLIST_WSS,      // https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/wss.txt
        NGOSANG_ALL,           // https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_all.txt
        NGOSANG_HTTP,          // https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_http.txt
        NGOSANG_HTTPS,         // https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_https.txt
        NGOSANG_UDP,           // https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_udp.txt
    };

    // Fetch tracker list from a specific source
    std::vector<std::string> fetchTrackers(Source source = Source::TRACKERSLIST_ALL);

    // Fetch tracker lists from multiple sources and merge them
    std::vector<std::string> fetchTrackersFromMultipleSources(
        const std::vector<Source>& sources = {
            Source::TRACKERSLIST_ALL,
            Source::NGOSANG_ALL
        });

    // Filter trackers by protocol
    std::vector<std::string> filterByProtocol(
        const std::vector<std::string>& trackers,
        const std::string& protocol);  // "http", "https", "udp", "wss"

    // Validate tracker URL format
    bool isValidTrackerUrl(const std::string& url);

    // Get tracker protocol
    std::string getProtocol(const std::string& trackerUrl);

    // Remove duplicate trackers
    std::vector<std::string> removeDuplicates(const std::vector<std::string>& trackers);

} // namespace TrackerList

#endif // TRACKER_LIST_H
