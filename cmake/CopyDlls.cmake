# CopyDlls.cmake
# Copies all DLLs from SOURCE_DIR to DEST_DIR that don't already exist in DEST_DIR
# This is used to copy vcpkg runtime DLLs including transitive dependencies

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR not specified")
endif()

if(NOT DEST_DIR)
    message(FATAL_ERROR "DEST_DIR not specified")
endif()

# For Debug builds, we may also need to check FALLBACK_DIR for DLLs that only exist in Release
# (some dependencies don't have debug versions)

if(NOT EXISTS "${SOURCE_DIR}")
    message(WARNING "Source directory does not exist: ${SOURCE_DIR}")
    return()
endif()

# Get list of all DLLs in source directory
file(GLOB SOURCE_DLLS "${SOURCE_DIR}/*.dll")

foreach(DLL_PATH ${SOURCE_DLLS})
    get_filename_component(DLL_NAME "${DLL_PATH}" NAME)
    set(DEST_PATH "${DEST_DIR}/${DLL_NAME}")

    # Only copy if destination doesn't exist (avoid overwriting)
    if(NOT EXISTS "${DEST_PATH}")
        message(STATUS "Copying: ${DLL_NAME}")
        file(COPY "${DLL_PATH}" DESTINATION "${DEST_DIR}")
    endif()
endforeach()

# If FALLBACK_DIR is specified (for Debug builds needing Release DLLs), copy those too
if(FALLBACK_DIR AND EXISTS "${FALLBACK_DIR}")
    file(GLOB FALLBACK_DLLS "${FALLBACK_DIR}/*.dll")

    foreach(DLL_PATH ${FALLBACK_DLLS})
        get_filename_component(DLL_NAME "${DLL_PATH}" NAME)
        set(DEST_PATH "${DEST_DIR}/${DLL_NAME}")

        # Only copy if destination doesn't exist
        if(NOT EXISTS "${DEST_PATH}")
            message(STATUS "Copying (fallback): ${DLL_NAME}")
            file(COPY "${DLL_PATH}" DESTINATION "${DEST_DIR}")
        endif()
    endforeach()
endif()
