#include "database/sqlite3/sqlite.hpp"
#include "test_sqlite3_common.hpp"
#include "fundamental/basic/cxx_config_include.hpp"
#include <gtest/gtest.h>
using namespace std;

TEST(test_sqlite3, testattach) {
    Fundamental::ScopeGuard g([&]() { std_fs::remove("test.db"); });
    {
        std_fs::remove("test.db");
        sqlite::database att_db = contacts_db("test.db");
        default_init_contacts_db(att_db);
    }
    if (1) return;
    try {

        sqlite::database db;
        try {
            // table is not existed
            sqlite::query qry(db, "SELECT COUNT(*) from contacts");
            EXPECT_TRUE(false);
        } catch (...) {
        }

        db.attach("test.db", "test");
        {
            sqlite::query qry(db, "SELECT COUNT(*) from test.contacts");
            auto iter = qry.begin();
            EXPECT_TRUE(iter != qry.end());
            EXPECT_EQ((*iter).get<int>(0), 2);
        }
        EXPECT_EQ(db.detach("test.db"), 0);
        try {
            // table is not existed
            sqlite::query qry(db, "SELECT COUNT(*) from test.contacts");
            EXPECT_TRUE(false);
        } catch (...) {
        }
    } catch (exception& ex) {
        FERR("ex:{}", ex.what());
        EXPECT_TRUE(false);
    }
}
