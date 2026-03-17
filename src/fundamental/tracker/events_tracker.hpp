#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace Fundamental
{
class events_tracker;
class events_tracker_handle {
    friend class events_tracker;

public:
    events_tracker_handle(const events_tracker_handle& other)            = delete;
    events_tracker_handle& operator=(const events_tracker_handle& other) = delete;
    events_tracker_handle(events_tracker_handle&& other) : events_tracker_handle(other.tracker_ref_, other.event_id_) {
        other.tracker_ref_.reset();
    }

    events_tracker_handle& operator=(events_tracker_handle&& other) {
        tracker_ref_ = other.tracker_ref_;
        event_id_    = other.event_id_;
        other.tracker_ref_.reset();
        return *this;
    }
    ~events_tracker_handle();
    auto get_event_id() const {
        return event_id_;
    }

private:
    events_tracker_handle(std::weak_ptr<events_tracker> tracker, std::uint64_t event_id) :
    tracker_ref_(tracker), event_id_(event_id) {
    }

private:
    std::weak_ptr<events_tracker> tracker_ref_;
    std::uint64_t event_id_ = 0;
};

class events_tracker : public std::enable_shared_from_this<events_tracker> {
    friend class events_tracker_handle;

public:
    using track_start_func_t = std::function<void(std::uint64_t, const std::string&)>;
    using track_cost_func_t  = std::function<void(std::uint64_t, const std::string&, double)>;

public:
    static auto make_tracker(const track_start_func_t& start_func,
                             const track_cost_func_t& verbose_func,
                             const track_cost_func_t& finish_func) {
        return std::make_shared<events_tracker>(start_func, verbose_func, finish_func);
    }
    events_tracker_handle track_event(std::string event_description) {
        while (access_flag_.test_and_set())
            ;
        while (true) {
            if (tracking_events.count(next_event_id_) == 0) break;
            ++next_event_id_;
        }
        auto using_event_id = next_event_id_++;
        tracking_events.emplace(using_event_id, std::make_tuple(event_description, std::chrono::steady_clock::now()));
        access_flag_.clear();
        if (start_func_) {
            start_func_(using_event_id, event_description);
        }
        return events_tracker_handle(weak_from_this(), using_event_id);
    }
    events_tracker(const track_start_func_t& start_func,
                   const track_cost_func_t& verbose_func,
                   const track_cost_func_t& finish_func) :
    start_func_(start_func), verbose_func_(verbose_func), finish_func_(finish_func) {
    }
    void verbose_all() {
        while (access_flag_.test_and_set())
            ;
        auto copy_track_event = tracking_events;
        access_flag_.clear();
        for (auto& event : copy_track_event) {
            auto& [description, start_timepoint] = event.second;
            auto elapsedTime                     = std::chrono::steady_clock::now() - start_timepoint;
            double elapsedTimeSec = std::chrono::duration_cast<std::chrono::duration<double>>(elapsedTime).count();
            if (verbose_func_) {
                verbose_func_(event.first, description, elapsedTimeSec);
            }
        }
    }

private:
    void untrack_event(std::uint64_t event_id) {
        while (access_flag_.test_and_set())
            ;
        auto iter = tracking_events.find(event_id);
        if (iter == tracking_events.end()) {
            access_flag_.clear();
            return;
        }
        auto& [description, start_timepoint] = iter->second;
        auto elapsedTime                     = std::chrono::steady_clock::now() - start_timepoint;
        double elapsedTimeSec   = std::chrono::duration_cast<std::chrono::duration<double>>(elapsedTime).count();
        auto notify_description = std::move(description);
        tracking_events.erase(event_id);
        access_flag_.clear();
        if (finish_func_) {
            finish_func_(event_id, notify_description, elapsedTimeSec);
        }
    }

private:
    events_tracker(const events_tracker& other)            = delete;
    events_tracker& operator=(const events_tracker& other) = delete;

private:
    std::atomic_flag access_flag_ = ATOMIC_FLAG_INIT;
    track_start_func_t start_func_;
    track_cost_func_t verbose_func_;
    track_cost_func_t finish_func_;
    std::uint64_t next_event_id_ = 0;
    std::map<std::uint64_t, std::tuple<std::string, std::chrono::steady_clock::time_point>> tracking_events;
};

inline events_tracker_handle::~events_tracker_handle() {
    auto strong = tracker_ref_.lock();
    if (strong) {
        strong->untrack_event(event_id_);
    }
}
} // namespace Fundamental