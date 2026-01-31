#include "database/sqlite3/sqlite.hpp"
#include <gtest/gtest.h>
#include <iostream>

#include "test_sqlite3_common.hpp"

using namespace std;

TEST(test_sqlite3, testinsertall) {
    try {
        auto db = contacts_db();
        {
            sqlite::transaction xct(db);
            {
                sqlite::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (:name, '1234');"
                                           "INSERT INTO contacts (name, phone) VALUES (:name, '5678');"
                                           "INSERT INTO contacts (name, phone) VALUES (:name, '9012');");
                EXPECT_EQ(0, cmd.bind(":name", "user", sqlite::copy));
                EXPECT_EQ(0, cmd.execute_all());
            }
            EXPECT_EQ(0, xct.commit());
            sqlite::query qry(db, "SELECT COUNT(*) from contacts");
            auto iter = qry.begin();
            EXPECT_TRUE(iter != qry.end());
            EXPECT_EQ((*iter).get<int>(0),3);
        }
    } catch (exception& ex) {
        FERR("ex:{}", ex.what());
        EXPECT_TRUE(false);
    }
}
