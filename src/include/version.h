#ifndef VERSION_H
#define VERSION_H


namespace ddb {

// Defined in the CMake-generated version.cpp (template: src/library/version.cpp.in).

// "X.Y.Z (git commit abc1234)", or "X.Y.Z" when git is unavailable at configure time.
const char* version();

// Short git hash, or "dev" when git is unavailable at configure time.
const char* revision();

}  // namespace ddb

#endif // VERSION_H
