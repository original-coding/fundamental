#pragma once
#include <algorithm>
#include <cassert>
#include <set>
namespace Fundamental::algorithm
{

//[low,up)
template <typename T>
struct range {
    using value_type = T;
    range()          = default;
    ~range()         = default;

    template <typename U1, typename U2>
    range(U1&& low_, U2&& up_) : low(std::forward<U1>(low_)), up(std::forward<U2>(up_)) {
    }
    range(const range& other) : low(other.low), up(other.up) {
    }
    range(range&& other) : low(std::move(other.low)), up(std::move(other.up)) {
    }
    range& operator=(const range& other) {
        low = other.low;
        up  = other.up;
        return *this;
    }
    range& operator=(range&& other) {
        low = std::move(other.low);
        up  = std::move(other.up);
        return *this;
    }

    bool operator<(const range& other) const {
        return up < other.low;
    }

    bool operator>(const range& other) const {
        return low > other.up;
    }

    bool in_range(const T& v) const {
        return !(v < low) && v < up;
    }
    operator bool() const {
        return low < up;
    }

    bool has_patitial_intersection(const range& other) const {
        return (in_range(other.low) && !in_range(other.up) && !(up > other.up /*up not equal*/)) ||
               (in_range(other.up) && !in_range(other.low));
    }

    range& range_combine(const range& other) {
        low = std::min(low, other.low);
        up  = std::max(up, other.up);
        return *this;
    }

    bool has_intersection(const range& other) const {

        return in_range(other.low) || (in_range(other.up) && other.up > low) || other.in_range(low) ||
               (other.in_range(up) && up > other.low);
    }

    bool contains(const range& other) const {
        return in_range(other.low) && in_range(other.up);
    }

    range get_intersection(const range& other) const {
        return range(std::max(other.low, low), std::min(other.up, up));
    }
    std::pair<range, range> get_difference(const range& other) const {
        return { range(low, other.low), range(other.up, up) };
    }

    T low = {};
    T up  = {};
};
template <typename _Value,
          typename _Key     = range<_Value>,
          typename _Compare = std::less<_Key>,
          typename _Alloc   = std::allocator<_Key>>
class range_set : public std::multiset<_Key, _Compare, _Alloc> {
    using super            = std::multiset<_Key, _Compare, _Alloc>;
    using range_value_type = typename _Key::value_type;
    using iterator=typename super::iterator;
public:
    range_set()  = default;
    ~range_set() = default;
    /// @brief emplace a range into set
    /// @return true for update set
    /// @note ranges will be combined as following rules:
    ///
    ///  original: [1,2) [3,4) [5,6) [10,15)
    ///
    ///  range_emplace [2,3) -> [1,4) [5,6) [10,15) return true
    ///  range_emplace [1,3) -> [1,4) [5,6) [10,15) return false
    ///  range_emplace [4,16) -> [1,16) return true
    template <typename... Args>
    std::pair<iterator, bool> range_emplace(Args&&... args) {
        range<range_value_type> new_range(std::forward<Args>(args)...);
        if (!new_range) return { iterator{}, false };
        auto ret   = super::equal_range(new_range);
        auto begin = ret.first;
        while (begin != ret.second) {
            assert(begin->has_intersection(new_range) || new_range.in_range(begin->up) ||
                   begin->in_range(new_range.up));
            if (begin->contains(new_range)) return { iterator{}, false };
            if (begin->has_patitial_intersection(new_range) || new_range.in_range(begin->up) ||
                begin->in_range(new_range.up)) { // combine
                new_range.range_combine(*begin);
            }
            super::erase(begin++);
        }
        return { super::emplace(std::move(new_range)), true };
    }

    /// @brief remove a range from set
    /// @return true for update set
    /// @note ranges will be removed as following rules:
    ///
    ///  original: [1,2) [10,15)
    ///
    ///  range_remove [1,2) ->  [10,15) return true
    ///  range_remove [0,1) -> [1,2) [10,15) return false
    ///  range_remove [4,12) -> [1,2) [12,15) return true
    ///  range_remove [11,12) -> [1,2) [10,11] [12,15) return true
    template <typename... Args>
    bool range_remove(Args&&... args) {
        range<range_value_type> remove_range(std::forward<Args>(args)...);
        if (!remove_range) return false;
        auto ret = super::equal_range(remove_range);
        if (ret.first == ret.second) return false;
        auto begin = ret.first;
        super temp;
        bool remove_flag = false;
        while (begin != ret.second) {
            if (!begin->has_intersection(remove_range)) {
                ++begin;
                continue;
            }
            auto& operate_range = *begin;
            auto diff           = operate_range.get_difference(remove_range);
            if (diff.first) temp.emplace(std::move(diff.first));
            if (diff.second) temp.emplace(std::move(diff.second));
            super::erase(begin++);
            remove_flag = true;
        }
        super::merge(std::move(temp));
        return remove_flag;
    }
};

} // namespace Fundamental::algorithm