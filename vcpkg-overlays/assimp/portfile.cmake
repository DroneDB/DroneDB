vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO assimp/assimp
    REF v6.0.2
    SHA512 dc9637b183a1ab4c87d3548b1cacf4278fc5d30ffa4ca35436f94723c20b916932791e8e2c2f0d2a63786078457e61a42fb7aac8462551172f7f5bd2582ad9a9
    PATCHES
        patches/no-kubazip-and-no-3mf.patch
)

# Configure: disable 3MF importer/exporter
vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=ON
        -DASSIMP_BUILD_3MF_IMPORTER=OFF
        -DASSIMP_BUILD_3MF_EXPORTER=OFF
        -DASSIMP_BUILD_TESTS=OFF
        -DASSIMP_BUILD_SAMPLES=OFF
        -DASSIMP_BUILD_ASSIMP_TOOLS=OFF
        -DASSIMP_NO_EXPORT=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME assimp CONFIG_PATH lib/cmake/assimp-6.0)
vcpkg_fixup_pkgconfig()

# Remove any kubazip dependency from assimpConfig.cmake
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
