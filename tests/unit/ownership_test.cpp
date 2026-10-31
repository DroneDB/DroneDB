/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Ownership of the SQLite wrappers and of the OS handles. Statement, SqliteDatabase
// and Database used to be implicitly copyable: a copy finalized the same sqlite3_stmt (or
// closed the same connection) twice. The static_asserts below did not compile before; the
// tests check that a move hands the resource over exactly once.

#include <memory>
#include <type_traits>
#include <utility>

#include "database.h"
#include "dbops.h"
#include "gtest/gtest.h"
#include "metamanager.h"
#include "sqlite_database.h"
#include "statement.h"
#include "test.h"
#include "testarea.h"
#include "unique_handle.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace ddb;

// A copy would finalize the prepared statement twice: move-only
static_assert(!std::is_copy_constructible_v<Statement>);
static_assert(!std::is_copy_assignable_v<Statement>);
static_assert(std::is_nothrow_move_constructible_v<Statement>);
static_assert(std::is_nothrow_move_assignable_v<Statement>);

// Polymorphic connection owners: neither copyable nor movable (held by unique_ptr)
static_assert(!std::is_copy_constructible_v<SqliteDatabase>);
static_assert(!std::is_copy_assignable_v<SqliteDatabase>);
static_assert(!std::is_move_constructible_v<SqliteDatabase>);
static_assert(!std::is_move_assignable_v<SqliteDatabase>);
static_assert(!std::is_copy_constructible_v<Database>);
static_assert(!std::is_copy_assignable_v<Database>);
static_assert(!std::is_move_constructible_v<Database>);
static_assert(!std::is_move_assignable_v<Database>);
static_assert(std::is_default_constructible_v<Database>);

// getMetaManager() keeps its exported signature: a non-owning pointer
static_assert(std::is_same_v<decltype(std::declval<Database&>().getMetaManager()), MetaManager*>);

#ifdef _WIN32
static_assert(!std::is_copy_constructible_v<UniqueHandle>);
static_assert(!std::is_copy_assignable_v<UniqueHandle>);
static_assert(std::is_nothrow_move_constructible_v<UniqueHandle>);
static_assert(std::is_nothrow_move_assignable_v<UniqueHandle>);
#else
static_assert(!std::is_copy_constructible_v<UniqueFd>);
static_assert(!std::is_copy_assignable_v<UniqueFd>);
static_assert(std::is_nothrow_move_constructible_v<UniqueFd>);
static_assert(std::is_nothrow_move_assignable_v<UniqueFd>);
#endif

// An in-memory connection that counts its unfinalized statements
class CountingDatabase : public SqliteDatabase {
public:
    CountingDatabase() {
        open(":memory:");
        exec("CREATE TABLE t(v INTEGER); INSERT INTO t VALUES (1), (2);");
    }

    int liveStatements() const {
        int n = 0;
        for (sqlite3_stmt* s = sqlite3_next_stmt(db, nullptr); s != nullptr;
             s = sqlite3_next_stmt(db, s))
            n++;
        return n;
    }
};

TEST(Ownership, statementMoveConstructionTransfersThePreparedStatement) {
    CountingDatabase db;
    auto source = db.query("SELECT v FROM t ORDER BY v");
    ASSERT_EQ(db.liveStatements(), 1);

    Statement moved(std::move(*source));
    source.reset();  // the moved-from statement owns nothing: destroying it finalizes nothing
    EXPECT_EQ(db.liveStatements(), 1);

    EXPECT_EQ(moved.getQuery(), "SELECT v FROM t ORDER BY v");
    ASSERT_TRUE(moved.fetch());
    EXPECT_EQ(moved.getInt(0), 1);
    ASSERT_TRUE(moved.fetch());
    EXPECT_EQ(moved.getInt(0), 2);
    EXPECT_FALSE(moved.fetch());
}

TEST(Ownership, statementMoveAssignmentFinalizesTheReplacedStatement) {
    CountingDatabase db;
    auto target = db.query("SELECT count(*) FROM t");
    auto source = db.query("SELECT v FROM t ORDER BY v DESC");
    ASSERT_EQ(db.liveStatements(), 2);

    *target = std::move(*source);
    EXPECT_EQ(db.liveStatements(), 1);  // the statement target held is finalized

    source.reset();
    EXPECT_EQ(db.liveStatements(), 1);

    ASSERT_TRUE(target->fetch());
    EXPECT_EQ(target->getInt(0), 2);

    target.reset();
    EXPECT_EQ(db.liveStatements(), 0);
}

TEST(Ownership, metaManagerIsOwnedByItsDatabase) {
    TestArea ta(TEST_NAME, true);
    initIndex(ta.getPath().string());
    const auto db = open(ta.getPath().string(), false);

    MetaManager* meta = db->getMetaManager();
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(db->getMetaManager(), meta);  // created once, then reused
    EXPECT_TRUE(meta->list().empty());
    // Freed with db (LeakSanitizer reports it otherwise)
}

#ifndef _WIN32
bool isOpen(int fd) {
    return ::fcntl(fd, F_GETFD) != -1;
}

TEST(Ownership, uniqueFdClosesOnceAndMovesOwnership) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    UniqueFd readEnd(fds[0]);
    UniqueFd writeEnd(fds[1]);

    {
        UniqueFd moved(std::move(writeEnd));
        EXPECT_FALSE(writeEnd);
        EXPECT_EQ(writeEnd.get(), -1);
        EXPECT_EQ(moved.get(), fds[1]);
        EXPECT_TRUE(isOpen(fds[1]));
    }
    EXPECT_FALSE(isOpen(fds[1]));  // closed by the new owner only

    // Move assignment closes the descriptor it replaces
    int other[2];
    ASSERT_EQ(::pipe(other), 0);
    UniqueFd otherRead(other[0]);
    readEnd = std::move(otherRead);
    EXPECT_FALSE(isOpen(fds[0]));
    EXPECT_EQ(readEnd.get(), other[0]);
    EXPECT_TRUE(isOpen(other[0]));

    // release() gives the descriptor up without closing it
    UniqueFd otherWrite(other[1]);
    const int released = otherWrite.release();
    EXPECT_FALSE(otherWrite);
    EXPECT_TRUE(isOpen(released));
    ::close(released);

    readEnd.reset();
    EXPECT_FALSE(readEnd);
    EXPECT_FALSE(isOpen(other[0]));
}
#else
TEST(Ownership, uniqueHandleMovesOwnership) {
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(event);
    const HANDLE raw = event.get();

    UniqueHandle moved(std::move(event));
    EXPECT_FALSE(event);
    EXPECT_EQ(moved.get(), raw);
    EXPECT_EQ(WaitForSingleObject(moved.get(), 0), static_cast<DWORD>(WAIT_TIMEOUT));

    // The failure value of CreateFile is not a handle: never closed
    UniqueHandle failed(INVALID_HANDLE_VALUE);
    EXPECT_FALSE(failed);

    moved.reset();
    EXPECT_FALSE(moved);
}
#endif

}  // namespace
