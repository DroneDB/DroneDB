if(APPLE)
  find_library(CoreFoundation CoreFoundation)
endif()

set(CCTZ_HDRS
  include/cctz/time_zone.h
  include/cctz/civil_time_detail.h
  include/cctz/zone_info_source.h
  include/cctz/civil_time.h
  )
add_library(cctz STATIC 
  src/civil_time_detail.cc
  src/time_zone_fixed.cc
  src/time_zone_fixed.h
  src/time_zone_format.cc
  src/time_zone_if.cc
  src/time_zone_if.h
  src/time_zone_impl.cc
  src/time_zone_impl.h
  src/time_zone_info.cc
  src/time_zone_info.h
  src/time_zone_libc.cc
  src/time_zone_libc.h
  src/time_zone_lookup.cc
  src/time_zone_posix.cc
  src/time_zone_posix.h
  src/tzfile.h
  src/zone_info_source.cc
  ${CCTZ_HDRS}
  )

target_include_directories(cctz PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  )
set_target_properties(cctz PROPERTIES
  PUBLIC_HEADER "${CCTZ_HDRS}"
  )
target_link_libraries(cctz PUBLIC $<$<PLATFORM_ID:Darwin>:${CoreFoundation}>)  
add_library(cctz::cctz ALIAS cctz)

