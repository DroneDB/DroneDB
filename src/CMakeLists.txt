file(GLOB SOURCES "*.cpp")

add_subdirectory("net")

set(SRC_LIST
   ${SRC_LIST}
   ${SOURCES}
   PARENT_SCOPE
)

add_subdirectory("cmd")

set(CMD_SRC_LIST
   ${CMD_SRC_LIST}
   PARENT_SCOPE
)

