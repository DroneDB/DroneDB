#include <assert.h>
#include "statement.h"
#include "exceptions.h"

Statement::Statement(sqlite3 *db, const std::string &query)
    : db(db), query(query) {
    if (sqlite3_prepare_v2(db, query.c_str(), static_cast<int>(query.size()), &stmt, nullptr) != SQLITE_OK) {
        throw SQLException("Cannot prepare SQL statement: " + query);
    }

    LOGD << "Statement: " + query;
}

void Statement::bindCheck(int ret) {
    if (ret != SQLITE_OK) {
        throw SQLException("Failed binding values for " + query);
    }
}

Statement::~Statement() {
    if (stmt != nullptr) {
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }
}

Statement &Statement::bind(int paramNum, const std::string &value) {
    assert(stmt != nullptr && db != nullptr);
    bindCheck(sqlite3_bind_text(stmt, paramNum, value.c_str(), static_cast<int>(value.size()), SQLITE_STATIC));
    return *this;
}

Statement &Statement::step() {
    assert(stmt != nullptr);

    switch(sqlite3_step(stmt)) {
    case SQLITE_DONE:
        done = true;
        hasRow = false;
        break;
    case SQLITE_ROW:
        hasRow = true;
        break;
    default:
        hasRow = done = false;
    }

    return *this;
}

bool Statement::fetch() {
    this->step();
    return hasRow;
}

int Statement::getInt(int columnId) {
    assert(stmt != nullptr);
    return sqlite3_column_int(stmt, columnId);
}
