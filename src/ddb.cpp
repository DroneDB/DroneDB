/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <gdal_priv.h>
#include "ddb.h"

#include <passwordmanager.h>

#include "dbops.h"
#include "info.h"
#include "status.h"
#include "version.h"
#include "mio.h"
#include "logger.h"
#include "database.h"
#include "net.h"
#include "entry.h"
#include "json.h"
#include "exceptions.h"
#include "utils.h"

using namespace ddb;

char ddbLastError[255];

// Could not be enough in a multi-threaded environment: check std::once_flag and std::call_once instead
static bool initialized = false;

void DDBRegisterProcess(bool verbose) {

	// Prevent multiple initializations
	if (initialized) {
		LOGD << "Called DDBRegisterProcess when already initialized";
		return;
	}
#ifndef WIN32
	// Windows does not let us change env vars for some reason
	// so this works only on Unix
	std::string projPaths = io::getExeFolderPath().string() + ":/usr/share/proj";
	setenv("PROJ_LIB", projPaths.c_str(), 1);
#endif

	// Gets the environment variable to enable logging to file
	const auto logToFile = std::getenv(DDB_LOG_ENV) != nullptr;

    // Enable logging if the environment variable is set
    if (!verbose && std::getenv(DDB_DEBUG_ENV) != nullptr) verbose = true;

	init_logger(logToFile);
	if (verbose || logToFile) {
		set_logger_verbose();
	}
	Database::Initialize();
	net::Initialize();
	GDALAllRegister();

	initialized = true;
}

const char* DDBGetVersion() {
	return APP_VERSION;
}

DDBErr DDBInit(const char* directory, char** outPath) {
	DDB_C_BEGIN

	if (directory == nullptr)
		throw InvalidArgsException("No directory provided");

	if (outPath == nullptr)
		throw InvalidArgsException("No output provided");

	const fs::path dirPath = directory;
	if (!exists(dirPath)) throw FSException("Invalid directory: " + dirPath.string() + " (does not exist)");

	auto ddbDirPath = dirPath / ".ddb";
	if (std::string(directory) == ".") ddbDirPath = ".ddb"; // Nicer to the eye
	const auto dbasePath = ddbDirPath / "dbase.sqlite";

	LOGD << "Checking if .ddb directory exists...";
	if (exists(ddbDirPath)) {
		throw FSException("Cannot initialize database: " + ddbDirPath.string() + " already exists");
	}
	
	if (create_directory(ddbDirPath)) {
		LOGD << ddbDirPath.string() + " created";
	}
	else {
		throw FSException("Cannot create directory: " + ddbDirPath.string() + ". Check that you have the proper permissions?");
	}

	LOGD << "Checking if database exists...";
	if (exists(dbasePath))
	{
		throw FSException(ddbDirPath.string() + " already exists");
	}
	LOGD << "Creating " << dbasePath.string();

	// Create database
	auto db = std::make_unique<Database>();
	db->open(dbasePath.string());
	db->createTables();
	db->close();

	utils::copyToPtr(ddbDirPath.string(), outPath);
	DDB_C_END
}

const char* DDBGetLastError()
{
	return ddbLastError;
}

void DDBSetLastError(const char* err) {
	strncpy(ddbLastError, err, 255);
	ddbLastError[254] = '\0';
}

DDBErr DDBAdd(const char* ddbPath, const char** paths, int numPaths, char** output, bool recursive) {
	DDB_C_BEGIN

		if (ddbPath == nullptr)
			throw InvalidArgsException("No directory provided");

		if (paths == nullptr || numPaths == 0)
			throw InvalidArgsException("No paths provided");

		if (output == nullptr)
			throw InvalidArgsException("No output provided");

		const auto db = ddb::open(std::string(ddbPath), true);
		const std::vector<std::string> pathList(paths, paths + numPaths);
		auto outJson = json::array();
		ddb::addToIndex(db.get(), ddb::expandPathList(pathList,
		                                              recursive,
		                                              0), [&outJson](const Entry& e, bool)
		{
			json j;
			e.toJSON(j);
			outJson.push_back(j);
			return true;
		});

		utils::copyToPtr(outJson.dump(), output);
	DDB_C_END
}

DDBErr DDBRemove(const char* ddbPath, const char** paths, int numPaths) {
	DDB_C_BEGIN

		if (ddbPath == nullptr)
			throw InvalidArgsException("No directory provided");

		if (paths == nullptr || numPaths == 0)
			throw InvalidArgsException("No paths provided");

		const auto db = ddb::open(std::string(ddbPath), true);
		const std::vector<std::string> pathList(paths, paths + numPaths);

		removeFromIndex(db.get(), pathList);
	DDB_C_END
}

DDBErr DDBInfo(const char** paths, int numPaths, char** output, const char* format, bool recursive, int maxRecursionDepth, const char* geometry, bool withHash, bool stopOnError) {
	DDB_C_BEGIN

		if (format == nullptr || strlen(format) == 0)
			throw InvalidArgsException("No format provided");

		if (geometry == nullptr || strlen(geometry) == 0)
			throw InvalidArgsException("No format provided");

		if (paths == nullptr || numPaths == 0)
			throw InvalidArgsException("No paths provided");

		if (output == nullptr)
			throw InvalidArgsException("No output provided");

		const std::vector<std::string> input(paths, paths + numPaths);
		std::ostringstream ss;
		info(input, ss, format, recursive, maxRecursionDepth,
		     geometry, withHash, stopOnError);
		utils::copyToPtr(ss.str(), output);
	DDB_C_END
}

DDBErr DDBList(const char* ddbPath, const char** paths, int numPaths, char** output, const char* format, bool recursive, int maxRecursionDepth) {
	DDB_C_BEGIN

		if (ddbPath == nullptr)
			throw InvalidArgsException("No ddb path provided");

		if (format == nullptr || strlen(format) == 0)
			throw InvalidArgsException("No format provided");

		if (paths == nullptr || numPaths == 0)
			throw InvalidArgsException("No paths provided");

		if (output == nullptr)
			throw InvalidArgsException("No output provided");

		const auto db = ddb::open(std::string(ddbPath), true);
		const std::vector<std::string> pathList(paths, paths + numPaths);

		std::ostringstream ss;
		listIndex(db.get(), pathList, ss, format, recursive, maxRecursionDepth);

		utils::copyToPtr(ss.str(), output);

	DDB_C_END
}

DDBErr DDBAppendPassword(const char* ddbPath, const char* password) {
	DDB_C_BEGIN

		if (ddbPath == nullptr)
			throw InvalidArgsException("No ddb path provided");

		if (password == nullptr || strlen(password) == 0)
			throw InvalidArgsException("No password provided");

		const auto db = ddb::open(std::string(ddbPath), true);

		PasswordManager manager(db.get());

		manager.append(std::string(password));
	
	DDB_C_END
}

DDBErr DDBVerifyPassword(const char* ddbPath, const char* password, bool* verified) {
	DDB_C_BEGIN

		if (ddbPath == nullptr)
			throw InvalidArgsException("No ddb path provided");

		// We allow empty password verification
		if (password == nullptr)
			throw InvalidArgsException("No password provided");

		if (verified == nullptr)
			throw InvalidArgsException("Output parameter pointer is null");

		const auto db = ddb::open(std::string(ddbPath), true);

		PasswordManager manager(db.get());

		*verified = manager.verify(std::string(password));
	
	DDB_C_END
}

DDBErr DDBClearPasswords(const char* ddbPath) {
	DDB_C_BEGIN

		if (ddbPath == nullptr)
			throw InvalidArgsException("No ddb path provided");

		const auto db = ddb::open(std::string(ddbPath), true);

		PasswordManager manager(db.get());

    manager.clearAll();
  
  DDB_C_END
}

DDB_DLL DDBErr DDBStatus(const char* ddbPath, char** output) {
  DDB_C_BEGIN

    if (ddbPath == nullptr)
      throw InvalidArgsException("No ddb path provided");

    if (output == nullptr)
      throw InvalidArgsException("No output provided");

      const auto db = ddb::open(std::string(ddbPath), true);

      std::ostringstream ss;

      const auto cb = [&ss](ddb::FileStatus status, const std::string& string)
      {
          switch (status){
          case ddb::NotIndexed:
              ss << "?\t";
              break;
          case ddb::Deleted:
              ss << "!\t";
              break;
          case ddb::Modified:
              ss << "M\t";
              break;
          }
      };

      statusIndex(db.get(), cb);

      utils::copyToPtr(ss.str(), output);

  DDB_C_END

}
