
# glob sources
file(GLOB SOURCES "*.cpp")

# Add sources to SRC_LIST
set(SRC_LIST
    ${SRC_LIST}
    ${SOURCES}
    PARENT_SCOPE
)
