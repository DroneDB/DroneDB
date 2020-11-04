#if(APPLE)
#  find_library(CoreFoundation CoreFoundation)
#endif()

set(SRC_LIST
   ${SRC_LIST}
   ${CMAKE_CURRENT_SOURCE_DIR}/src/civil_time_detail.cc
   ${CMAKE_CURRENT_SOURCE_DIR}/src/time_zone_fixed.cc
   ${CMAKE_CURRENT_SOURCE_DIR}/src/time_zone_format.cc
   ${CMAKE_CURRENT_SOURCE_DIR}/src/time_zone_if.cc
   ${CMAKE_CURRENT_SOURCE_DIR}/src/time_zone_impl.cc
   ${CMAKE_CURRENT_SOURCE_DIR}/src/time_zone_info.cc
   ${CMAKE_CURRENT_SOURCE_DIR}/src/time_zone_libc.cc
   ${CMAKE_CURRENT_SOURCE_DIR}/src/time_zone_lookup.cc
   ${CMAKE_CURRENT_SOURCE_DIR}/src/time_zone_posix.cc
   ${CMAKE_CURRENT_SOURCE_DIR}/src/zone_info_source.cc
   PARENT_SCOPE
)

#set_target_properties(cctz PROPERTIES
#  PUBLIC_HEADER "${CCTZ_HDRS}"
#  )
#target_link_libraries(cctz PUBLIC $<$<PLATFORM_ID:Darwin>:${CoreFoundation}>)  
#add_library(cctz::cctz ALIAS cctz)

