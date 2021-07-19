:: Instructions on how to build libzip for Windows
:: Requires: zlib binaries and library

set DOWNLOADS_DIR=d:/ddb/build/downloads

cd libzip
md build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="./install" -DZLIB_INCLUDE_DIR="%DOWNLOADS_DIR%/zlib/include/" -DZLIB_LIBRARY="%DOWNLOADS_DIR%/zlib/lib/zlib.lib"
cmake --build . --config Release --target install