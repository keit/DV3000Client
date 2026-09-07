cmake_minimum_required(VERSION 3.17.0)
project(dv3000client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(BUILD_TOOL OFF CACHE BOOL "" FORCE) # don't build serialDV's own dvtest binary
add_subdirectory(third_party/serialDV)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)

add_executable(roundtrip_test
  src/roundtrip_test.cpp
)
target_link_libraries(roundtrip_test serialdv)
target_include_directories(roundtrip_test PRIVATE third_party/serialDV)

add_library(xlx_directory
  src/xlx_directory.cpp
)

add_executable(xlx_directory_test
  src/xlx_directory_test.cpp
)
target_link_libraries(xlx_directory_test xlx_directory)

add_library(dextra_client
  src/dextra_client.cpp
)
target_link_libraries(dextra_client serialdv)
target_include_directories(dextra_client PUBLIC third_party/serialDV)

add_library(dextra_audio
  src/dextra_audio.cpp
)
target_link_libraries(dextra_audio dextra_client ${ALSA_LIBRARIES})
target_include_directories(dextra_audio PUBLIC ${ALSA_INCLUDE_DIRS})

add_executable(dextra_test
  src/dextra_test.cpp
)
target_link_libraries(dextra_test dextra_audio xlx_directory)
