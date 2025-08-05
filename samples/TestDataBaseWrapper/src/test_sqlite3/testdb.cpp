#include "database/sqlite3/sqliteext.hpp"
#include <iostream>

#include "test_sqlite3_common.hpp"

#include <gtest/gtest.h>

using namespace std;
TEST(test_sqlite3, test_insert_execute) {
    auto db = contacts_db();
    EXPECT_EQ(0, db.execute("INSERT INTO contacts (name, phone) VALUES ('Mike', '555-1234')"));

    sqlite::query qry(db, "SELECT name, phone FROM contacts");
    auto iter = qry.begin();
    string name, phone;
    (*iter).getter() >> name >> phone;
    EXPECT_EQ("Mike", name);
    EXPECT_EQ("555-1234", phone);
}

TEST(test_sqlite3, test_insert_execute_all) {
    auto db = contacts_db();
    sqlite::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (:user, '555-0000');"
                               "INSERT INTO contacts (name, phone) VALUES (:user, '555-1111');"
                               "INSERT INTO contacts (name, phone) VALUES (:user, '555-2222')");
    cmd.bind(":user", "Mike", sqlite::nocopy);
    EXPECT_EQ(0, cmd.execute_all());

    sqlite::query qry(db, "SELECT COUNT(*) FROM contacts");
    auto iter = qry.begin();
    int count = (*iter).get<int>(0);
    EXPECT_EQ(3, count);
}

TEST(test_sqlite3, test_insert_binder) {
    auto db = contacts_db();
    sqlite::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (?, ?)");
    cmd.binder() << "Mike" << "555-1234";
    EXPECT_EQ(0, cmd.execute());

    sqlite::query qry(db, "SELECT name, phone FROM contacts");
    auto iter = qry.begin();
    string name, phone;
    (*iter).getter() >> name >> phone;
    EXPECT_EQ("Mike", name);
    EXPECT_EQ("555-1234", phone);
}
TEST(test_sqlite3, test_insert_bind1) {
    auto db = contacts_db();
    sqlite::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (?, ?)");
    cmd.bind(1, "Mike", sqlite::nocopy);
    cmd.bind(2, "555-1234", sqlite::nocopy);
    EXPECT_EQ(0, cmd.execute());

    sqlite::query qry(db, "SELECT name, phone FROM contacts");
    auto iter = qry.begin();
    string name, phone;
    (*iter).getter() >> name >> phone;
    EXPECT_EQ("Mike", name);
    EXPECT_EQ("555-1234", phone);
}
TEST(test_sqlite3, test_insert_bind2) {
    auto db = contacts_db();
    sqlite::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (?100, ?101)");
    cmd.bind(100, "Mike", sqlite::nocopy);
    cmd.bind(101, "555-1234", sqlite::nocopy);
    EXPECT_EQ(0, cmd.execute());

    sqlite::query qry(db, "SELECT name, phone FROM contacts");
    auto iter = qry.begin();
    string name, phone;
    (*iter).getter() >> name >> phone;
    EXPECT_EQ("Mike", name);
    EXPECT_EQ("555-1234", phone);
}
TEST(test_sqlite3, test_insert_bind3) {
    auto db = contacts_db();
    sqlite::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (:user, :phone)");
    cmd.bind(":user", "Mike", sqlite::nocopy);
    cmd.bind(":phone", "555-1234", sqlite::nocopy);
    EXPECT_EQ(0, cmd.execute());

    sqlite::query qry(db, "SELECT name, phone FROM contacts");
    auto iter = qry.begin();
    string name, phone;
    (*iter).getter() >> name >> phone;
    EXPECT_EQ("Mike", name);
    EXPECT_EQ("555-1234", phone);
}
TEST(test_sqlite3, test_insert_bind_null) {
    auto db = contacts_db();
    sqlite::command cmd(db, "INSERT INTO contacts (name, phone, address) VALUES (:user, :phone, :address)");
    cmd.bind(":user", "Mike", sqlite::nocopy);
    cmd.bind(":phone", "555-1234", sqlite::nocopy);
    cmd.bind(":address");
    EXPECT_EQ(0, cmd.execute());

    sqlite::query qry(db, "SELECT name, phone, address FROM contacts");
    auto iter = qry.begin();
    string name, phone;
    const char* address = nullptr;
    (*iter).getter() >> name >> phone >> address;
    EXPECT_EQ("Mike", name);
    EXPECT_EQ("555-1234", phone);
    EXPECT_EQ(nullptr, address);
}
TEST(test_sqlite3, test_insert_binder_null) {
    auto db = contacts_db();
    sqlite::command cmd(db, "INSERT INTO contacts (name, phone, address) VALUES (?, ?, ?)");
    cmd.binder() << "Mike" << "555-1234" << nullptr;
    EXPECT_EQ(0, cmd.execute());

    sqlite::query qry(db, "SELECT name, phone, address FROM contacts");
    auto iter = qry.begin();
    string name, phone;
    const char* address = nullptr;
    (*iter).getter() >> name >> phone >> address;
    EXPECT_EQ("Mike", name);
    EXPECT_EQ("555-1234", phone);
    EXPECT_EQ(nullptr, address);
}
TEST(test_sqlite3, test_query_columns) {
    auto db = contacts_db();
    EXPECT_EQ(0, db.execute("INSERT INTO contacts (name, phone) VALUES ('Mike', '555-1234')"));

    sqlite::query qry(db, "SELECT id, name, phone FROM contacts");
    EXPECT_EQ(3, qry.column_count());
    EXPECT_EQ(string("id"), qry.column_name(0));
    EXPECT_EQ(string("name"), qry.column_name(1));
    EXPECT_EQ(string("phone"), qry.column_name(2));
}
TEST(test_sqlite3, test_query_get) {
    auto db = contacts_db();
    EXPECT_EQ(0, db.execute("INSERT INTO contacts (name, phone) VALUES ('Mike', '555-1234')"));

    sqlite::query qry(db, "SELECT id, name, phone FROM contacts");
    auto iter = qry.begin();
    EXPECT_EQ(string("Mike"), (*iter).get<char const*>(1));
    EXPECT_EQ(string("555-1234"), (*iter).get<char const*>(2));
}

TEST(test_sqlite3, test_query_tie) {
    auto db = contacts_db();
    EXPECT_EQ(0, db.execute("INSERT INTO contacts (name, phone) VALUES ('Mike', '555-1234')"));

    sqlite::query qry(db, "SELECT id, name, phone FROM contacts");
    auto iter = qry.begin();
    char const *name, *phone;
    std::tie(name, phone) = (*iter).get_columns<char const*, char const*>(1, 2);
    EXPECT_EQ(string("Mike"), name);
    EXPECT_EQ(string("555-1234"), phone);
}

TEST(test_sqlite3, test_query_getter) {
    auto db = contacts_db();
    EXPECT_EQ(0, db.execute("INSERT INTO contacts (name, phone) VALUES ('Mike', '555-1234')"));

    sqlite::query qry(db, "SELECT id, name, phone FROM contacts");
    auto iter = qry.begin();
    string name, phone;
    (*iter).getter() >> sqlite::internal::ignore >> name >> phone;
    EXPECT_EQ(string("Mike"), name);
    EXPECT_EQ(string("555-1234"), phone);
}
TEST(test_sqlite3, test_query_iterator) {
    auto db = contacts_db();
    EXPECT_EQ(0, db.execute("INSERT INTO contacts (name, phone) VALUES ('Mike', '555-1234')"));

    sqlite::query qry(db, "SELECT id, name, phone FROM contacts");
    bool has_value = false;
    for (auto row : qry) {
        string name, phone;
        row.getter() >> sqlite::internal::ignore >> name >> phone;
        EXPECT_EQ(string("Mike"), name);
        EXPECT_EQ(string("555-1234"), phone);
        has_value = true;
    }
    EXPECT_TRUE(has_value);
}
TEST(test_sqlite3, test_function) {
    auto db = contacts_db();
    sqlite::ext::function func(db);
    EXPECT_EQ(0, func.create<int()>("test_fn", [] { return 100; }));

    sqlite::query qry(db, "SELECT test_fn()");
    auto iter = qry.begin();
    int count = (*iter).get<int>(0);
    EXPECT_EQ(100, count);
}
TEST(test_sqlite3, test_function_args) {
    auto db = contacts_db();
    sqlite::ext::function func(db);
    func.create<string(string)>("test_fn", [](const string& name) { return "Hello " + name; });
    EXPECT_EQ(0, db.execute("INSERT INTO contacts (name, phone) VALUES ('Mike', '555-1234')"));

    sqlite::query qry(db, "SELECT name, test_fn(name) FROM contacts");
    string name, hello_name;
    auto iter = qry.begin();
    EXPECT_TRUE(iter != qry.end());
    (*iter).getter() >> name >> hello_name;
    EXPECT_EQ(string("Mike"), name);
    EXPECT_EQ(string("Hello Mike"), hello_name);
}

struct strlen_aggr {
    void step(const string& s) {
        total_len +=static_cast<std::int32_t>(s.size());
    }

    int finish() {
        return total_len;
    }
    int total_len = 0;
};
TEST(test_sqlite3, test_aggregate) {
    auto db = contacts_db();
    sqlite::ext::aggregate aggr(db);
    aggr.create<strlen_aggr, string>("strlen_aggr");

    EXPECT_EQ(0, db.execute("INSERT INTO contacts (name, phone) VALUES ('Mike', '555-1234')"));
    EXPECT_EQ(0, db.execute("INSERT INTO contacts (name, phone) VALUES ('Janette', '555-4321')"));

    sqlite::query qry(db, "SELECT strlen_aggr(name), strlen_aggr(phone) FROM contacts");
    auto iter = qry.begin();
    EXPECT_EQ(11, (*iter).get<int>(0));
    EXPECT_EQ(16, (*iter).get<int>(1));
}
TEST(test_sqlite3, test_invalid_path) {
    try {
        sqlite::database db("/test/invalid/path");
    } catch (sqlite::database_error& e) {
        return;
    }
    EXPECT_TRUE(false);
}
TEST(test_sqlite3, test_reset) {
    auto db = contacts_db();
    sqlite::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (:user, :phone)");
    cmd.bind(":user", "Mike", sqlite::nocopy);
    cmd.bind(":phone", "555-1234", sqlite::nocopy);
    EXPECT_EQ(0, cmd.execute());

    cmd.reset();
    cmd.bind(":user", "Janette", sqlite::nocopy);
    EXPECT_EQ(0, cmd.execute());

    sqlite::query qry(db, "SELECT COUNT(*) FROM contacts");
    auto iter = qry.begin();
    int count = (*iter).get<int>(0);
    EXPECT_EQ(2, count);

    cmd.reset();
    cmd.clear_bindings();
    cmd.bind(":user", "Dave", sqlite::nocopy);
    EXPECT_EQ(SQLITE_CONSTRAINT, cmd.execute());
}
