#pragma once
#include "database/sqlite3/sqliteext.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"

inline sqlite::database contacts_db(const std::string& name = "") {
    sqlite::database db(name.empty() ? sqlite::database::kMemoryDbName : name.c_str());
    db.execute(R"""(
    CREATE TABLE contacts (
      id INTEGER PRIMARY KEY,
      name TEXT NOT NULL,
      phone TEXT NOT NULL,
      address TEXT,
      age INTEGER,
      level INTEGER,
      UNIQUE(name, phone)
    );
  )""");
    return db;
}

inline void default_init_contacts_db(sqlite::database& db) {
    db.execute("INSERT INTO contacts (name, phone,age,address,level) VALUES ('1', '1',1,'1',1)");
    db.execute("INSERT INTO contacts (name, phone,age,address,level) VALUES ('2', '2',2,'2',2)");
}