
#include "common.hpp"
#include <gtest/gtest.h>
#include <iostream>

TEST(algorithm, gen_two_party_pairs) {
    {
        std::size_t players = 7;
        auto ret            = Fundamental::gen_two_party_pairs(players);
        EXPECT_EQ(ret.size(), 3);
        if (ret.size() != 3) return;
        std::size_t round_index = 0;
        {
            auto& round = ret[round_index++];
            EXPECT_EQ(round[0], 1);
            EXPECT_EQ(round[2], 3);
            EXPECT_EQ(round[4], 5);
            EXPECT_EQ(round.size(), 3);
        }
        {
            auto& round = ret[round_index++];
            EXPECT_EQ(round[0], 2);
            EXPECT_EQ(round[4], 6);
            EXPECT_EQ(round.size(), 2);
        }
        {
            auto& round = ret[round_index++];
            EXPECT_EQ(round[0], 4);
            EXPECT_EQ(round.size(), 1);
        }
    }
}

int main(int argc, char** argv) {
    if (argc > 1) {
        std::size_t players_num = 0;
        try {
            players_num = std::stoul(argv[1]);
            if (players_num < 128) {
                auto ret = Fundamental::gen_two_party_pairs(players_num);
                for (std::size_t round_index = 0; round_index < ret.size(); ++round_index) {
                    std::cout << "round " << round_index << " ->";
                    auto& round = ret[round_index];
                    for (auto& item : round) {
                        std::cout << " [" << item.first << "," << item.second << "]";
                    }
                    std::cout << std::endl;
                }
            }
        } catch (...) {
        }
    }
    argc = 1;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}