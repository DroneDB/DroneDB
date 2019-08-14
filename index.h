#ifndef INDEX_H
#define INDEX_H

#include <experimental/filesystem>
#include <algorithm>
#include <cctype>
#include <string>
#include <exiv2/exiv2.hpp>

#include "database.h"
#include "exceptions.h"
#include "logger.h"

namespace fs = std::experimental::filesystem;

void updateIndex(const std::string &directory, Database *db);
bool checkExtension(const fs::path &extension, const std::initializer_list<std::string>& matches);

#endif // INDEX_H
