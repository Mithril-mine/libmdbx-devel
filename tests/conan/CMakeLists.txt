cmake_minimum_required(VERSION 3.15)
project(test_package LANGUAGES CXX)

find_package(mdbx REQUIRED CONFIG)

add_executable(test_package main.cpp)
target_link_libraries(test_package PRIVATE mdbx::mdbx)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME test_package COMMAND test_package)
endif()
