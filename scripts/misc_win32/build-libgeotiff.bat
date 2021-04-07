:: Instructions on how to build libgeotiff for Windows
:: Requires: GDAL/CURL libs+binaries
:: Make sure to clone the vcpkg repo from https://github.com/microsoft/vcpkg

set DOWNLOADS_DIR=d:/ddb/build/downloads

cd vcpkg
bootstrap-vcpkg.bat
cd ..
cd libgeotiff
md build
cd build
cmake .. -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release  -DCMAKE_C_FLAGS="/WX" -DCMAKE_CXX_FLAGS="/WX"  -DCMAKE_INSTALL_PREFIX="./install" -DPROJ_INCLUDE_DIR="%DOWNLOADS_DIR%/gdal/include/proj6" -DPROJ_LIBRARY="%DOWNLOADS_DIR%/gdal/lib/proj_6_1" -DTIFF_LIBRARY="%DOWNLOADS_DIR%/gdal/lib/libtiff_i" -DTIFF_INCLUDE_DIR="%DOWNLOADS_DIR%/gdal/include" -DCMAKE_TOOLCHAIN_FILE=../../libgeotiff/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release --target install