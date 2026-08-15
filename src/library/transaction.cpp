/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "transaction.h"

#include "exceptions.h"
#include "logger.h"

namespace ddb {

namespace {
const char *beginSql(Transaction::Mode mode) {
    switch (mode) {
        case Transaction::Mode::Exclusive: return "BEGIN EXCLUSIVE TRANSACTION";
        case Transaction::Mode::Deferred:  return "BEGIN DEFERRED TRANSACTION";
        case Transaction::Mode::Immediate:
        default:                           return "BEGIN IMMEDIATE TRANSACTION";
    }
}
} // namespace

Transaction::Transaction(SqliteDatabase *db, Mode mode, const RetryPolicy &policy)
    : db_(db), policy_(policy), finished_(false) {
    if (db_ == nullptr)
        throw DBException("Cannot start transaction: db is null");

    const char *sql = beginSql(mode);
    policy_.run([&] { db_->exec(sql); }, "begin transaction");
}

Transaction::Transaction(Transaction &&other) noexcept
    : db_(other.db_), policy_(other.policy_), finished_(other.finished_) {
    other.db_ = nullptr;
    other.finished_ = true;
}

Transaction::~Transaction() noexcept {
    if (finished_ || db_ == nullptr) return;
    try {
        db_->exec("ROLLBACK");
    } catch (const std::exception &e) {
        LOGD << "Rollback failed: " << e.what();
    }
    finished_ = true;
}

void Transaction::commit() {
    if (finished_) throw DBException("Transaction already finished");
    // COMMIT can itself return SQLITE_BUSY in rare WAL situations; retry it too, with the same
    // policy. If every retry is exhausted the transaction is still open and the destructor will
    // roll it back.
    policy_.run([&] { db_->exec("COMMIT"); }, "commit transaction");
    finished_ = true;
}

void Transaction::rollback() {
    if (finished_) throw DBException("Transaction already finished");
    db_->exec("ROLLBACK");
    finished_ = true;
}

} // namespace ddb
