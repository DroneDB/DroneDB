#include "../../vendor/base64/base64.h"
#include "passwordmanager.h"
#include "logger.h"
#include "hash.h"
#include "transaction.h"

#include <string>
#include <random>
#include "utils.h"

namespace ddb
{

	DDB_DLL int PasswordManager::countPasswords()
	{

		const std::string sql = "SELECT COUNT(*) FROM passwords";
		const auto q = this->db->query(sql);
		q->fetch();
		const auto cnt = q->getInt(0);
		q->reset();

		return cnt;
	}

	DDB_DLL void PasswordManager::append(const std::string &password)
	{

		if (password.empty())
		{
			LOGD << "Trying to add an empty password";
			return;
		}

		const auto salt = utils::generateRandomString(SALT_LENGTH);
		const auto hash = Hash::strSHA256(salt + password);

		const std::string sql = "INSERT INTO passwords VALUES(?, ?)";

		const auto q = this->db->query(sql);

		q->bind(1, salt);
		q->bind(2, hash);

		// Single-statement autocommit writer; wrapped in an IMMEDIATE transaction so BEGIN is
		// retried with jitter on SQLITE_BUSY (busy_timeout is only a 200 ms floor). Safe: never
		// called with a transaction already open on the connection (C API only, ddb.cpp).
		Transaction tx(this->db, Transaction::Mode::Immediate);
		q->execute();
		q->reset();
		tx.commit();
	}

	// Nice to have
	// void PasswordManager::remove(const std::string& password)
	//{
	//}

	DDB_DLL bool PasswordManager::verify(const std::string &password)
	{
		const auto noPasswords = countPasswords() == 0;
		if (password.empty() && noPasswords)
			return true;

		const std::string sql = "SELECT salt, hash FROM passwords";
		const auto q = this->db->query(sql);

		while (q->fetch())
		{

			auto salt = q->getText(0);
			auto hash = q->getText(1);

			auto calculatedHash = Hash::strSHA256(salt + password);

			if (hash == calculatedHash)
			{
				q->reset();
				return true;
			}
		}

		q->reset();

		return false;
	}

	DDB_DLL void PasswordManager::clearAll()
	{
		const std::string sql = "DELETE FROM passwords";
		const auto q = this->db->query(sql);
		q->execute();
		// Same rationale as append(): retried IMMEDIATE transaction around the write.
		Transaction tx(this->db, Transaction::Mode::Immediate);
		q->execute();
		q->reset();
		tx.commit();
	}

}
