project(segvcatch)

cmake_minimum_required(VERSION 2.8)

set(SOURCES
  segvcatch.cpp 
  segvcatch.h
  i386-signal.h
  x86_64-signal.h
)

add_library(segvcatch STATIC ${SOURCES})

if(NOT MSVC)
    target_compile_options(segvcatch PRIVATE -Wall -fexceptions -fnon-call-exceptions)
endif()
