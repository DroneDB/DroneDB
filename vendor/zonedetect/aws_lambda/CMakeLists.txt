cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 11)
project(timezone LANGUAGES CXX C)

find_package(aws-lambda-runtime REQUIRED)
add_executable(${PROJECT_NAME} ../library/zonedetect.c main.cpp)
target_link_libraries(${PROJECT_NAME} PUBLIC AWS::aws-lambda-runtime)
aws_lambda_package_target(${PROJECT_NAME} NO_LIBC)
