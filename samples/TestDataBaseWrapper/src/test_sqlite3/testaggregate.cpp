#include "database/sqlite3/sqliteext.hpp"
#include "test_sqlite3_common.hpp"

#include <gtest/gtest.h>
#include <iostream>
#include <string>

using namespace std;

void step0(sqlite::ext::context& c) {
    int* sum = (int*)c.aggregate_data(sizeof(int));

    *sum += c.get<int>(0);
}
void finalize0(sqlite::ext::context& c) {
    int* sum = (int*)c.aggregate_data(sizeof(int));
    c.result(*sum);
}

void step1(sqlite::ext::context& c) {
    string* sum = (string*)c.aggregate_data(sizeof(string));

    if (c.aggregate_count() == 1) {
        new (sum) string;
    }

    *sum += c.get<string>(0);
}
void finalize1(sqlite::ext::context& c) {
    string* sum = (string*)c.aggregate_data(sizeof(string));
    c.result(*sum);

    sum->~string();
}

template <class T>
struct mysum {
    mysum() {
        s_ = T();
    }
    void step(T s) {
        s_ += s;
    }
    T finish() {
        return s_;
    }
    T s_;
};

struct mycnt {
    void step() {
        ++n_;
    }
    int finish() {
        return n_;
    }
    int n_;
};

struct strcnt {
    void step(string const& s) {
        s_ += s;
    }
    int finish() {
        return static_cast<int>(s_.size());
    }
    string s_;
};

struct plussum {
    void step(int n1, int n2) {
        n_ += n1 + n2;
    }
    int finish() {
        return n_;
    }
    int n_;
};

TEST(test_sqlite3, testaggregate) {
    try {
        auto db = contacts_db();
        default_init_contacts_db(db);

        sqlite::ext::aggregate aggr(db);
        EXPECT_EQ(aggr.create("a0", &step0, &finalize0), 0);
        EXPECT_EQ(aggr.create("a1", &step1, &finalize1), 0);
        EXPECT_EQ((aggr.create<mysum<string>, string>("a2")), 0);
        EXPECT_EQ((aggr.create<mysum<int>, int>("a3")), 0);
        EXPECT_EQ(aggr.create<mycnt>("a4"), 0);
        EXPECT_EQ((aggr.create<strcnt, string>("a5")), 0);
        EXPECT_EQ((aggr.create<plussum, int, int>("a6")), 0);

        sqlite::query qry(
            db, "SELECT a0(id), a1(name), a2(name), a3(id), a4(), a5(name), sum(id), a6(age, level) FROM contacts");
        EXPECT_TRUE(strcmp("a0(id)", qry.column_name(0)) == 0);
        EXPECT_TRUE(strcmp("a1(name)", qry.column_name(1)) == 0);
        EXPECT_TRUE(strcmp("a2(name)", qry.column_name(2)) == 0);
        EXPECT_TRUE(strcmp("a3(id)", qry.column_name(3)) == 0);
        EXPECT_TRUE(strcmp("a4()", qry.column_name(4)) == 0);
        EXPECT_TRUE(strcmp("a5(name)", qry.column_name(5)) == 0);
        EXPECT_TRUE(strcmp("sum(id)", qry.column_name(6)) == 0);
        EXPECT_TRUE(strcmp("a6(age, level)", qry.column_name(7)) == 0);

        auto iter = qry.begin();
        EXPECT_TRUE(iter != qry.end());
        auto row = *iter;
        EXPECT_EQ(row.data_count(), 8);
        auto [a0, a1, a2, a3, a4, a5, sum, a6] =
            row.get_columns<int, std::string, std::string, int, int, int, int, int>(0, 1, 2, 3, 4, 5, 6, 7);
        EXPECT_EQ(a0, 3);
        EXPECT_EQ(a1, "12");
        EXPECT_EQ(a2, "12");
        EXPECT_EQ(a3, 3);
        EXPECT_EQ(a4, 2);
        EXPECT_EQ(a5, 2);
        EXPECT_EQ(sum, 3);
        EXPECT_EQ(a6, 6);
    } catch (exception& ex) {
        FERR("ex:{}", ex.what());
        EXPECT_TRUE(false);
    }
}
