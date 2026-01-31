#include "database/sqlite3/sqliteext.hpp"
#include "test_sqlite3_common.hpp"
#include <gtest/gtest.h>
#include <iostream>
#include <string>

using namespace std;

int test0() {
    return 100;
}

void test1(sqlite::ext::context& ctx) {
    ctx.result(200);
}

void test2(sqlite::ext::context& ctx) {
    std::string args = ctx.get<std::string>(0);
    ctx.result(args);
}

void test3(sqlite::ext::context& ctx) {
    ctx.result_copy(0);
}

std::string test6(std::string const& s1, std::string const& s2, std::string const& s3) {
    return s1 + s2 + s3;
}

TEST(test_sqlite3, testfunction) {
    try {
        auto db = contacts_db();
        sqlite::ext::function func(db);
        func.create<int()>("h0", &test0);
        func.create("h1", &test1);
        func.create("h2", &test2, 1);
        func.create("h3", &test3, 1);
        func.create<int()>("h4", [] { return 500; });
        func.create<int(int)>("h5", [](int i) { return i + 1000; });
        func.create<string(string, string, string)>("h6", &test6);

        sqlite::query qry(db, "SELECT  h0(), h1(), h2('x'), h3('y'), h4(), h5(10), h6('a', 'b', 'c')");
        auto iter = qry.begin();
        EXPECT_TRUE(iter != qry.end());
        auto row = *iter;
        EXPECT_EQ(row.get<int>(0), 100);
        EXPECT_EQ(row.get<int>(1), 200);
        EXPECT_EQ(row.get<std::string>(2), "x");
        EXPECT_EQ(row.get<int>(3), 0);
        EXPECT_EQ(row.get<int>(4), 500);
        EXPECT_EQ(row.get<int>(5), 1010);
        EXPECT_EQ(row.get<std::string>(6), "abc");
    } catch (exception& ex) {
        FERR("ex:{}", ex.what());
        EXPECT_TRUE(false);
    }
}
