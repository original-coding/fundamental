#include "database/sqlite3/sqlite.hpp"
#include "test_sqlite3_common.hpp"
#include <gtest/gtest.h>
#include <iostream>

using namespace std;

TEST(test_sqlite3, testbackup) {
    try {
        sqlite::database db = contacts_db();
        default_init_contacts_db(db);
        {
            sqlite::query qry(db, "SELECT COUNT(*) from contacts");
            auto iter = qry.begin();
            EXPECT_TRUE(iter != qry.end());
            EXPECT_EQ((*iter).get<int>(0), 2);
        }
        sqlite::database backupdb;
        try {
            // table is not existed
            sqlite::query qry(backupdb, "SELECT COUNT(*) from contacts");
            EXPECT_TRUE(false);
        } catch (...) {
        }
        db.backup(backupdb, [](int pagecount, int remaining, int rc) {
            FINFOS << "status:(" << rc << ") progress:" << pagecount << "/" << remaining;
            EXPECT_EQ(rc,SQLITE_DONE);
            if (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            }
        });
        {
            sqlite::query qry(backupdb, "SELECT COUNT(*) from contacts");
            auto iter = qry.begin();
            EXPECT_TRUE(iter != qry.end());
            EXPECT_EQ((*iter).get<int>(0), 2);
        }
    } catch (exception& ex) {
        FERR("ex:{}", ex.what());
        EXPECT_TRUE(false);
    }
}
