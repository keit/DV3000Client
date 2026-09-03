cmake_minimum_required(VERSION 3.17.0)
project(dv3000client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(BUILD_TOOL OFF CACHE BOOL "" FORCE) # don't build serialDV's own dvtest binary
add_subdirectory(third_party/serialDV)

add_executable(roundtrip_test
  src/roundtrip_test.cpp
)
target_link_libraries(roundtrip_test serialdv)
target_include_directories(roundtrip_test PRIVATE third_party/serialDV)

add_executable(dextra_test
  src/dextra_test.cpp
)
target_link_libraries(dextra_test serialdv)
target_include_directories(dextra_test PRIVATE third_party/serialDV)
