/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdexcept>

class AppException : public std::runtime_error{
    using std::runtime_error::runtime_error;
};
class DBException : public AppException{
    using AppException::AppException;
};
class SQLException : public DBException{
    using DBException::DBException;
};
class FSException : public AppException{
    using AppException::AppException;
};
class TimezoneException : public AppException{
    using AppException::AppException;
};
class IndexException : public AppException{
    using AppException::AppException;
};
class InvalidArgsException : public AppException{
    using AppException::AppException;
};
class GDALException : public AppException{
    using AppException::AppException;
};
class CURLException : public AppException{
    using AppException::AppException;
};

#endif // EXCEPTIONS_H
