vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO assimp/assimp
    REF v6.0.2
    SHA512 dc9637b183a1ab4c87d3548b1cacf4278fc5d30ffa4ca35436f94723c20b916932791e8e2c2f0d2a63786078457e61a42fb7aac8462551172f7f5bd2582ad9a9
)

# Remove third-party internals that you do NOT want to use
# NOTE: Keep contrib/draco - we need its headers, but we'll use external draco library
file(REMOVE_RECURSE
    "${SOURCE_PATH}/contrib/zlib"
    "${SOURCE_PATH}/contrib/zip"
)

# Fix MSVC compiler flag conflict on Windows: /utf-8 and /source-charset:utf-8 cannot be used together
if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt"
        "ADD_COMPILE_OPTIONS(/source-charset:utf-8)"
        ""
    )
endif()

# Patch Assimp to use external/system draco library instead of building its own
# This avoids symbol conflicts when linking statically with other libraries that also use draco (e.g., PDAL)
# Replace the ELSE() block that builds internal draco with find_package(draco)
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt"
[[    IF(ASSIMP_HUNTER_ENABLED)
      hunter_add_package(draco)
      find_package(draco CONFIG REQUIRED)
      SET(draco_LIBRARIES draco::draco)
    ELSE()]]
[[    IF(ASSIMP_HUNTER_ENABLED)
      hunter_add_package(draco)
    ENDIF()
    # Use external/system draco library (e.g., from vcpkg) to avoid symbol conflicts
    find_package(draco CONFIG REQUIRED)
    SET(draco_LIBRARIES draco::draco)
    get_target_property(draco_INCLUDE_DIRS draco::draco INTERFACE_INCLUDE_DIRECTORIES)
    IF(FALSE) # Disabled: we use external draco]]
)

# Close the IF(FALSE) block - find the end of the internal draco build section
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt"
[[      SET(draco_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/contrib/draco/src")

      # This is probably wrong
      IF (ASSIMP_INSTALL)
        INSTALL( TARGETS ${draco_LIBRARIES}
          EXPORT "${TARGETS_EXPORT_NAME}"
          LIBRARY DESTINATION ${ASSIMP_LIB_INSTALL_DIR}
          ARCHIVE DESTINATION ${ASSIMP_LIB_INSTALL_DIR}
          RUNTIME DESTINATION ${ASSIMP_BIN_INSTALL_DIR}
          FRAMEWORK DESTINATION ${ASSIMP_LIB_INSTALL_DIR}
          COMPONENT ${LIBASSIMP_COMPONENT}
          INCLUDES DESTINATION include
        )
      ENDIF()
    ENDIF()
  ENDIF()
ENDIF()]]
[[      SET(draco_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/contrib/draco/src")

      # This is probably wrong
      IF (ASSIMP_INSTALL)
        INSTALL( TARGETS ${draco_LIBRARIES}
          EXPORT "${TARGETS_EXPORT_NAME}"
          LIBRARY DESTINATION ${ASSIMP_LIB_INSTALL_DIR}
          ARCHIVE DESTINATION ${ASSIMP_LIB_INSTALL_DIR}
          RUNTIME DESTINATION ${ASSIMP_BIN_INSTALL_DIR}
          FRAMEWORK DESTINATION ${ASSIMP_LIB_INSTALL_DIR}
          COMPONENT ${LIBASSIMP_COMPONENT}
          INCLUDES DESTINATION include
        )
      ENDIF()
    ENDIF() # End of IF(FALSE) block for internal draco
  ENDIF()
ENDIF()]]
)

# Platform-specific options
set(PLATFORM_OPTIONS "")
if(VCPKG_TARGET_IS_WINDOWS)
    # On Windows, we need to explicitly use the system zlib
    list(APPEND PLATFORM_OPTIONS
        -DZLIB_ROOT=${CURRENT_INSTALLED_DIR}
    )
endif()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF
        -DASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT=OFF
        -DASSIMP_NO_EXPORT=OFF

        # Importer/Exporter to KEEP
        -DASSIMP_BUILD_GLTF_IMPORTER=ON
        -DASSIMP_BUILD_GLTF_EXPORTER=ON
        -DASSIMP_BUILD_OBJ_IMPORTER=ON
        -DASSIMP_BUILD_OBJ_EXPORTER=ON
        -DASSIMP_BUILD_PLY_IMPORTER=ON
        -DASSIMP_BUILD_PLY_EXPORTER=ON

        # Make sure to NOT bring along zip/zlib stuff
        -DASSIMP_BUILD_3MF_IMPORTER=OFF
        -DASSIMP_BUILD_3MF_EXPORTER=OFF
        -DASSIMP_BUILD_ZLIB=OFF

        # Enable Draco for GLTF compression support (uses external draco from vcpkg)
        -DASSIMP_BUILD_DRACO=ON

        # Variants
        -DASSIMP_BUILD_TESTS=OFF
        -DASSIMP_BUILD_SAMPLES=OFF
        -DASSIMP_BUILD_ASSIMP_TOOLS=OFF
        -DASSIMP_WARNINGS_AS_ERRORS=OFF
        -DASSIMP_IGNORE_GIT_HASH=ON

        ${PLATFORM_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME assimp CONFIG_PATH lib/cmake/assimp-6.0)
vcpkg_fixup_pkgconfig()

# Clean up any kubazip/zip dependencies from the generated config (as you already do)
set(_cfg "${CURRENT_PACKAGES_DIR}/share/assimp/assimpConfig.cmake")
if(EXISTS "${_cfg}")
    file(READ "${_cfg}" _cfg_contents)
    string(REPLACE "find_dependency(kubazip CONFIG)" "" _cfg_contents "${_cfg_contents}")
    string(REPLACE "find_dependency(zip CONFIG)" "" _cfg_contents "${_cfg_contents}")
    file(WRITE "${_cfg}" "${_cfg_contents}")
endif()

# Tidy
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
