# StageGdalData.cmake
# Copies GDAL's data files (vcpkg's share/gdal) from SOURCE_DIR to DEST_DIR, the "gdal" folder
# libddb finds next to itself (setupGdalData, process_init.cpp), and packages from there. That
# share folder also holds what vcpkg installs around them (CMake package files and Find modules,
# vcpkg's own metadata), which is left out: only the data files ship.

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR not specified")
endif()

if(NOT DEST_DIR)
    message(FATAL_ERROR "DEST_DIR not specified")
endif()

if(NOT EXISTS "${SOURCE_DIR}/gdalvrt.xsd")
    message(FATAL_ERROR "Not a GDAL data directory: ${SOURCE_DIR}")
endif()

file(COPY "${SOURCE_DIR}/" DESTINATION "${DEST_DIR}"
    PATTERN "*.cmake" EXCLUDE
    PATTERN "vcpkg*" EXCLUDE
    PATTERN "usage" EXCLUDE
    PATTERN "packages" EXCLUDE
    REGEX "/[0-9]+\\.[0-9]+$" EXCLUDE)
