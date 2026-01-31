#include "fundamental/basic/utils.hpp"

#include "database/sqlite3/sqlite.hpp"
#include "test_sqlite3_common.hpp"
#include <exception>
#include <functional>
#include <gtest/gtest.h>
#include <iostream>
using namespace std;
using namespace std::placeholders;

struct update_handler {
    update_handler() : cnt_(0) {
    }

    void handle_update([[maybe_unused]] int opcode,
                       [[maybe_unused]] char const* dbname,
                       [[maybe_unused]] char const* tablename,
                       [[maybe_unused]] long long int rowid) {
        cnt_++;
    }
    int cnt_ = 0;
};

static std::size_t auth_cnt = 0;

int handle_authorize([[maybe_unused]] int evcode,
                     [[maybe_unused]] char const* p1,
                     [[maybe_unused]] char const* p2,
                     [[maybe_unused]] char const* dbname,
                     [[maybe_unused]] char const* tvname) {
    ++auth_cnt;
    return 0;
}

struct rollback_handler {
    void operator()() {
        ++cnt_;
    }
    int cnt_ = 0;
};

TEST(test_sqlite3, testcallback) {
    try {
        auto db                                          = contacts_db();
        [[maybe_unused]] std::size_t target_commit_cnt   = 0;
        [[maybe_unused]] std::size_t target_update_cnt   = 0;
        [[maybe_unused]] std::size_t target_auth_cnt     = 0;
        [[maybe_unused]] std::size_t target_rollback_cnt = 0;
        [[maybe_unused]] std::size_t commit_cnt          = 0;

        db.set_commit_handler([&] {
            ++commit_cnt;
            return 0;
        });
        auto r_h = rollback_handler();
        db.set_rollback_handler(std::ref(r_h));

        update_handler h;

        db.set_update_handler(std::bind(&update_handler::handle_update, &h, _1, _2, _3, _4));

        db.set_authorize_handler(&handle_authorize);
        {
            db.execute("INSERT INTO contacts (name, phone) VALUES ('AAAA', '1234')");
            target_commit_cnt++;
            target_update_cnt++;
            ++target_auth_cnt;
            EXPECT_EQ(target_commit_cnt, commit_cnt);
            EXPECT_EQ(h.cnt_, target_update_cnt);
            EXPECT_EQ(target_auth_cnt, auth_cnt);
        }

        {
            sqlite::transaction xct(db);
            {
                ++target_auth_cnt;
                EXPECT_EQ(target_auth_cnt, auth_cnt);
            }
            sqlite::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (?, ?)");
            {
                ++target_auth_cnt;
                EXPECT_EQ(target_auth_cnt, auth_cnt);
            }
            cmd.bind(1, "BBBB", sqlite::copy);
            cmd.bind(2, "1234", sqlite::copy);

            cmd.execute();
            {
                ++target_update_cnt;
                EXPECT_EQ(target_update_cnt, h.cnt_);
            }
            cmd.reset();

            cmd.binder() << "CCCC" << "1234";

            cmd.execute();
            {
                ++target_update_cnt;
                EXPECT_EQ(target_update_cnt, h.cnt_);
            }
            xct.commit();
            {
                target_commit_cnt++;
                ++target_auth_cnt;
                EXPECT_EQ(target_commit_cnt, commit_cnt);
                EXPECT_EQ(target_auth_cnt, auth_cnt);
            }
        }
        {
            // check no commit operation
            Fundamental::ScopeGuard g([&]() {
                target_rollback_cnt++;
                ++target_auth_cnt;
                EXPECT_EQ(target_rollback_cnt, r_h.cnt_);
                EXPECT_EQ(target_auth_cnt, auth_cnt);
            });
            sqlite::transaction xct(db);
            {
                ++target_auth_cnt;
                EXPECT_EQ(target_auth_cnt, auth_cnt);
            }
            sqlite::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (:name, :name)");
            {
                ++target_auth_cnt;
                EXPECT_EQ(target_auth_cnt, auth_cnt);
            }
            cmd.bind(":name", "DDDD", sqlite::copy);

            cmd.execute();
            {
                ++target_update_cnt;
                EXPECT_EQ(target_update_cnt, h.cnt_);
            }
        }
    } catch (std::exception& ex) {
        FERR("ex:{}", ex.what());
        EXPECT_TRUE(false);
    }
}
