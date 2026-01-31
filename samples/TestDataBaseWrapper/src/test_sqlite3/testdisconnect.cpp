#include "database/sqlite3/sqlite.hpp"
#include "test_sqlite3_common.hpp"
#include <gtest/gtest.h>
#include <iostream>
using namespace std;

TEST(test_sqlite3, testdisconnect) {
    try {
        auto db = contacts_db();
        {
            sqlite::transaction xct(db);
            {
                sqlite::command cmd(db, "INSERT INTO contacts (name, phone) VALUES ('AAAA', '1234')");

                EXPECT_EQ(cmd.execute(), 0);
            }
        }
        EXPECT_EQ(db.disconnect(), 0);

    } catch (exception& ex) {
        FERR("ex:{}", ex.what());
        EXPECT_TRUE(false);
    }
}
