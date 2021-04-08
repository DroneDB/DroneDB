:: Instructions on how to build PDAL for Windows
:: Requires: libgeotiff and zlib already built, plus GDAL/CURL libs+binaries
:: Adjust paths as needed

set DOWNLOADS_DIR=d:/ddb/build/downloads

cd PDAL
md build
cd build
cmake .. -DGDAL_LIBRARY="%DOWNLOADS_DIR%/gdal/lib/gdal_i.lib" -DGDAL_INCLUDE_DIR="%DOWNLOADS_DIR%/gdal/include" -DGEOTIFF_LIBRARY="%DOWNLOADS_DIR%/geotiff/lib/geotiff_i.lib" -DGEOTIFF_INCLUDE_DIR="%DOWNLOADS_DIR%/geotiff/include" -DZLIB_LIBRARY="%DOWNLOADS_DIR%/zlib/lib/zlib.lib" -DZLIB_INCLUDE_DIR="%DOWNLOADS_DIR%/zlib/include" -DCURL_LIBRARY="%DOWNLOADS_DIR%/gdal/lib/libcurl_imp.lib" -DCURL_INCLUDE_DIR="%DOWNLOADS_DIR%/gdal/include" -DLASZIP_LIBRARY="%DOWNLOADS_DIR%/laszip/lib/laszip3.lib" -DLASZIP_INCLUDE_DIR="%DOWNLOADS_DIR%/laszip/include" -DCMAKE_INSTALL_PREFIX=./install
cmake --build . --config Release --target install