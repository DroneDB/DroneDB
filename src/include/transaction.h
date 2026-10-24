/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TRANSACTION_H
#define TRANSACTION_H

#include "retrypolicy.h"
#include "sqlite_database.h"

namespace ddb {

// RAII wrapper for BEGIN/COMMIT/ROLLBACK. Guarantees a matching ROLLBACK on exception, early
// return or cancellation.
class Transaction {
public:
    // Begins an immediate transaction (the write lock is taken at once, so a conflict shows at
    // BEGIN and not at the first write), retrying on DBBusyException per the policy.
    explicit Transaction(SqliteDatabase *db, const RetryPolicy &policy = RetryPolicy::defaultPolicy());

    // Rolls back if commit() was never called.
    ~Transaction() noexcept;

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    void commit();

private:
    SqliteDatabase *db_;
    RetryPolicy policy_;
    bool finished_;
};

} // namespace ddb

#endif // TRANSACTION_H
