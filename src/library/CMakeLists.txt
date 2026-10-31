
# glob sources
file(GLOB SOURCES CONFIGURE_DEPENDS "*.cpp")

# Add sources to SRC_LIST
set(SRC_LIST
    ${SRC_LIST}
    ${SOURCES}
    PARENT_SCOPE
)
