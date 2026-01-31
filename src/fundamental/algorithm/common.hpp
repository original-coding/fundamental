#pragma once
#include <algorithm>
#include <cstddef>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace Fundamental
{
template <typename Container,
          typename = std::enable_if_t<std::is_same_v<
              typename std::iterator_traits<decltype(std::begin(std::declval<Container>()))>::iterator_category,
              std::random_access_iterator_tag>>>
inline void shuffle(Container& c) {
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(std::begin(c), std::end(c), g);
}

// all: 0 1 2 3 4 5 6
// result:
//-> round 0: 0-1 2-3 4-5
//-> round 1: 0-2 4-6
//->round 2: 0-4
inline std::vector<std::map<std::size_t, std::size_t>> gen_two_party_pairs(std::size_t player_num) {
    std::vector<std::map<std::size_t, std::size_t>> ret;
    std::size_t base_gap = 1;
    while (true) {
        std::map<std::size_t, std::size_t> current_round;
        std::size_t index = 0;
        while (index + base_gap < player_num) {
            current_round[index] = index + base_gap;
            index += 2 * base_gap;
        }
        if (current_round.empty()) break;
        ret.emplace_back(std::move(current_round));
        base_gap *= 2;
    }
    return ret;
}

} // namespace Fundamental
