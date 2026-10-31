/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "transaction.h"

#include "exceptions.h"
#include "logger.h"

namespace ddb {

Transaction::Transaction(SqliteDatabase *db, const RetryPolicy &policy)
    : db_(db), policy_(policy), finished_(false) {
    if (db_ == nullptr)
        throw DBException("Cannot start transaction: db is null");

    policy_.run([&] { db_->exec("BEGIN IMMEDIATE TRANSACTION"); }, "begin transaction");
}

Transaction::~Transaction() noexcept {
    if (finished_) return;
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

} // namespace ddb
