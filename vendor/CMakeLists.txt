add_subdirectory("segvcatch")
add_subdirectory("url")
add_subdirectory("base64")
add_subdirectory("libnexus")

set(SRC_LIST
    ${SRC_LIST}
    ${CMAKE_CURRENT_SOURCE_DIR}/zonedetect/library/zonedetect.c
    PARENT_SCOPE
)
