#include "database/sqlite3/sqlite.hpp"
#include "test_sqlite3_common.hpp"
#include <gtest/gtest.h>
#include <iostream>
#include <string>
using namespace std;

TEST(test_sqlite3, testselect) {
    try {
        auto db = contacts_db();
        default_init_contacts_db(db);

        sqlite::transaction xct(db, true);

        {
            sqlite::query qry(db, "SELECT id, name, phone FROM contacts");
            EXPECT_EQ(qry.column_count(), 3);
            EXPECT_EQ("id", std::string(qry.column_name(0)));
            EXPECT_EQ("name", std::string(qry.column_name(1)));
            EXPECT_EQ("phone", std::string(qry.column_name(2)));
            qry.reset();

            for (sqlite::query::iterator i = qry.begin(); i != qry.end(); ++i) {
                auto [id, name, phone] = (*i).get_columns<std::string, std::string, std::string>(0, 1, 2);
                EXPECT_EQ(id, name);
                EXPECT_EQ(id, phone);
                FINFOS << "id:" << id << " name:" << name << " phone:" << phone;
            }

            qry.reset();

            for (sqlite::query::iterator i = qry.begin(); i != qry.end(); ++i) {
                std::string name, phone;
                (*i).getter() >> sqlite::internal::ignore >> name >> phone;
                EXPECT_EQ(phone, name);
            }
        }
    } catch (exception& ex) {
        FERR("ex:{}", ex.what());
        EXPECT_TRUE(false);
    }
}
