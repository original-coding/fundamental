#include "events_tracker.hpp"
#include "fundamental/basic/log.h"

#include <gtest/gtest.h>
#include <iostream>
#include <thread>

TEST(tracker, tracker_basic) {
    std::size_t cnt1        = 0;
    std::size_t cnt2        = 0;
    std::size_t cnt3        = 0;
    std::string events_name = "test_events";
    auto tracker            = Fundamental::events_tracker::make_tracker(
        [&](std::uint64_t id, const std::string& name) {
            if (name == events_name) {
                cnt1++;
            }
            FINFO("start event[{}]:{}", id, name);
        },
        [&](std::uint64_t id, const std::string& name, double cost_sec) {
            if (name == events_name) {
                cnt2++;
            }
            FINFO("event[{}]:{} cost {:.6f} seconds", id, name, cost_sec);
        },
        [&](std::uint64_t id, const std::string& name, double cost_sec) {
            if (name == events_name) {
                cnt3++;
            }
            FINFO("event[{}]:{} finally cost {:.6f} seconds", id, name, cost_sec);
        });
    {
        auto handle = tracker->track_event(events_name);
        // check init event
        EXPECT_EQ(cnt1, 1);
        EXPECT_EQ(cnt2, 0);
        EXPECT_EQ(cnt3, 0);
        // check verbose
        tracker->verbose_all();
        EXPECT_EQ(cnt1, 1);
        EXPECT_EQ(cnt2, 1);
        EXPECT_EQ(cnt3, 0);
    }
    // check release handle
    EXPECT_EQ(cnt1, 1);
    EXPECT_EQ(cnt2, 1);
    EXPECT_EQ(cnt3, 1);
    tracker->verbose_all();
    EXPECT_EQ(cnt1, 1);
    EXPECT_EQ(cnt2, 1);
    EXPECT_EQ(cnt3, 1);
}

TEST(tracker, auto_manager) {
    std::size_t cnt1 = 0;
    std::size_t cnt2 = 0;
    std::size_t cnt3 = 0;

    auto tracker = Fundamental::events_tracker::make_tracker(
        [&](std::uint64_t id, const std::string& name) {
            ++cnt1;
            FINFO("start event[{}]:{}", id, name);
        },
        [&](std::uint64_t id, const std::string& name, double cost_sec) {
            ++cnt2;
            FINFO("event[{}]:{} cost {:.6f} seconds", id, name, cost_sec);
        },
        [&](std::uint64_t id, const std::string& name, double cost_sec) {
            FINFO("event[{}]:{} finally cost {:.6f} seconds", id, name, cost_sec);
            ++cnt3;
        });
    {
        {
            auto handle = tracker->track_event("111");
        }
        {
            auto handle = tracker->track_event("333");
        }
        auto handle3 = tracker->track_event("444");
        EXPECT_EQ(cnt1, 3);
        tracker->verbose_all();

        EXPECT_EQ(cnt2, 1);
        tracker.reset();
    }
    EXPECT_EQ(cnt3, 2);
}
int main(int argc, char** argv) {

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}