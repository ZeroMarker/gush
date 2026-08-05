#include <gtest/gtest.h>
#include "tracker_manager.h"
#include <chrono>

TEST(TrackerManagerTest, EmptyByDefault) {
    TrackerManager mgr;
    EXPECT_TRUE(mgr.empty());
    EXPECT_EQ(mgr.size(), 0u);
}

TEST(TrackerManagerTest, AddAndDeduplicate) {
    TrackerManager mgr({"udp://tracker.a:1337/announce"});
    mgr.add("udp://tracker.a:1337/announce");  // duplicate
    mgr.add("http://tracker.b/announce");
    mgr.addAll({"udp://tracker.c/announce", "http://tracker.b/announce"});
    EXPECT_EQ(mgr.size(), 3u);
    EXPECT_FALSE(mgr.empty());
}

TEST(TrackerManagerTest, AllFreshTrackersAreDue) {
    TrackerManager mgr({"udp://a/announce", "udp://b/announce"});
    auto due = mgr.dueTrackers();
    EXPECT_EQ(due.size(), 2u);
}

TEST(TrackerManagerTest, FailureAppliesBackoff) {
    TrackerManager mgr({"udp://a/announce"});
    mgr.reportFailure("udp://a/announce");
    // First failure: 30s backoff, so the tracker is not due immediately
    EXPECT_TRUE(mgr.dueTrackers().empty());

    // Failures accumulate exponentially
    mgr.reportFailure("udp://a/announce");
    EXPECT_TRUE(mgr.dueTrackers().empty());
    EXPECT_EQ(mgr.states()[0].consecutiveFailures, 2);
}

TEST(TrackerManagerTest, SuccessResetsBackoff) {
    TrackerManager mgr({"udp://a/announce"});
    mgr.reportFailure("udp://a/announce");
    EXPECT_TRUE(mgr.dueTrackers().empty());

    TrackerResponse resp;
    resp.interval = 120;
    Peer p;
    p.ip = "1.2.3.4";
    p.port = 6881;
    resp.peers = {p};
    mgr.reportSuccess("udp://a/announce", resp);

    // Backoff cleared: tracker is due again and recorded interval/peers
    EXPECT_EQ(mgr.dueTrackers().size(), 1u);
    EXPECT_EQ(mgr.states()[0].consecutiveFailures, 0);
    EXPECT_EQ(mgr.states()[0].interval, 120);
    EXPECT_EQ(mgr.states()[0].lastPeerCount, 1);
}

TEST(TrackerManagerTest, PreferredTrackerComesFirst) {
    TrackerManager mgr({"udp://first/announce", "udp://second/announce"});

    TrackerResponse resp;
    resp.interval = 1800;
    mgr.reportSuccess("udp://second/announce", resp);

    auto due = mgr.dueTrackers();
    ASSERT_EQ(due.size(), 2u);
    EXPECT_EQ(due[0], "udp://second/announce");  // preferred first
    EXPECT_EQ(due[1], "udp://first/announce");
}

TEST(TrackerManagerTest, FewestFailuresComeFirst) {
    TrackerManager mgr({"udp://a/announce", "udp://b/announce", "udp://c/announce"});
    mgr.reportFailure("udp://a/announce");
    mgr.reportFailure("udp://a/announce");
    mgr.reportFailure("udp://b/announce");

    auto due = mgr.dueTrackers();
    ASSERT_EQ(due.size(), 1u);  // only c is out of backoff
    EXPECT_EQ(due[0], "udp://c/announce");
}

TEST(TrackerManagerTest, BackoffClearsOverTime) {
    TrackerManager mgr({"udp://a/announce"});
    mgr.reportFailure("udp://a/announce");
    EXPECT_TRUE(mgr.dueTrackers().empty());

    // Simulate waiting past the 30s backoff
    auto& st = mgr.states()[0];
    st.nextAllowed = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    EXPECT_EQ(mgr.dueTrackers().size(), 1u);
}
