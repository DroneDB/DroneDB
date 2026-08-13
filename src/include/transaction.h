/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TRANSACTION_H
#define TRANSACTION_H

#include "ddb_export.h"
#include "retrypolicy.h"
#include "sqlite_database.h"

namespace ddb {

// RAII wrapper for BEGIN/COMMIT/ROLLBACK. Guarantees a matching ROLLBACK on exception, early
// return or cancellation (I4). Replaces the hand-rolled BEGIN/COMMIT sites across dbops.cpp and
// metamanager.cpp.
class DDB_DLL Transaction {
public:
    enum class Mode { Deferred, Immediate, Exclusive };

    // Acquires the transaction, retrying on DBBusyException per the policy.
    explicit Transaction(SqliteDatabase *db, Mode mode = Mode::Immediate,
                        const RetryPolicy &policy = RetryPolicy::defaultPolicy());

    // Rolls back if commit() was never called.
    ~Transaction() noexcept;

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;
    Transaction(Transaction &&other) noexcept;
    Transaction &operator=(Transaction &&) = delete;

    void commit();
    void rollback();

private:
    SqliteDatabase *db_;
    RetryPolicy policy_;
    bool finished_;
};

} // namespace ddb

#endif // TRANSACTION_H
