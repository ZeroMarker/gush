#include "tracker_list.h"
#include <curl/curl.h>
#include <sstream>
#include <algorithm>
#include <set>
#include <map>
#include <iostream>

namespace TrackerList {

// Source URL mappings
static const std::map<Source, std::string> g_sourceUrls = {
    {Source::TRACKERSLIST_ALL, "https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/all.txt"},
    {Source::TRACKERSLIST_BEST, "https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/best.txt"},
    {Source::TRACKERSLIST_HTTP, "https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/http.txt"},
    {Source::TRACKERSLIST_HTTPS, "https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/https.txt"},
    {Source::TRACKERSLIST_UDP, "https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/udp.txt"},
    {Source::TRACKERSLIST_WSS, "https://raw.githubusercontent.com/XIU2/TrackersListCollection/master/wss.txt"},
    {Source::NGOSANG_ALL, "https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_all.txt"},
    {Source::NGOSANG_HTTP, "https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_http.txt"},
    {Source::NGOSANG_HTTPS, "https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_https.txt"},
    {Source::NGOSANG_UDP, "https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_udp.txt"},
};

static size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t totalSize = size * nmemb;
    userp->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::string getProtocol(const std::string& trackerUrl) {
    if (trackerUrl.substr(0, 8) == "https://") return "https";
    if (trackerUrl.substr(0, 7) == "http://") return "http";
    if (trackerUrl.substr(0, 6) == "udp://") return "udp";
    if (trackerUrl.substr(0, 6) == "wss://") return "wss";
    if (trackerUrl.substr(0, 5) == "ws://") return "ws";
    return "unknown";
}

bool isValidTrackerUrl(const std::string& url) {
    std::string protocol = getProtocol(url);
    return protocol != "unknown" && !url.empty();
}

std::vector<std::string> filterByProtocol(
    const std::vector<std::string>& trackers,
    const std::string& protocol) {

    std::vector<std::string> result;
    for (const auto& tracker : trackers) {
        if (getProtocol(tracker) == protocol) {
            result.push_back(tracker);
        }
    }
    return result;
}

std::vector<std::string> removeDuplicates(const std::vector<std::string>& trackers) {
    std::set<std::string> seen;
    std::vector<std::string> result;

    for (const auto& tracker : trackers) {
        if (seen.find(tracker) == seen.end()) {
            seen.insert(tracker);
            result.push_back(tracker);
        }
    }

    return result;
}

std::vector<std::string> fetchTrackers(Source source) {
    std::vector<std::string> trackers;

    auto it = g_sourceUrls.find(source);
    if (it == g_sourceUrls.end()) {
        return trackers;
    }

    const std::string& url = it->second;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to initialize CURL for tracker list fetch" << std::endl;
        return trackers;
    }

    std::string responseData;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseData);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Gush/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        std::cerr << "Failed to fetch tracker list: " << curl_easy_strerror(res) << std::endl;
        curl_easy_cleanup(curl);
        return trackers;
    }

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (httpCode != 200) {
        std::cerr << "HTTP error fetching tracker list: " << httpCode << std::endl;
        return trackers;
    }

    // Parse tracker list (one per line, skip empty lines)
    std::istringstream iss(responseData);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end = line.find_last_not_of(" \t\r\n");

        if (start == std::string::npos) continue;  // Empty line

        std::string tracker = line.substr(start, end - start + 1);

        // Skip comments
        if (tracker.empty() || tracker[0] == '#') continue;

        // Validate tracker URL
        if (isValidTrackerUrl(tracker)) {
            trackers.push_back(tracker);
        }
    }

    return trackers;
}

std::vector<std::string> fetchTrackersFromMultipleSources(
    const std::vector<Source>& sources) {

    std::vector<std::string> allTrackers;

    for (Source source : sources) {
        std::vector<std::string> trackers = fetchTrackers(source);
        if (!trackers.empty()) {
            std::cout << "Fetched " << trackers.size() << " trackers from source" << std::endl;
            allTrackers.insert(allTrackers.end(), trackers.begin(), trackers.end());
        }
    }

    // Remove duplicates
    return removeDuplicates(allTrackers);
}

} // namespace TrackerList
