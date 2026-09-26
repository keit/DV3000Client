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

add_library(http_get
  src/http_get.cpp
)

add_library(xlx_directory
  src/xlx_directory.cpp
)
target_link_libraries(xlx_directory http_get)

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

add_library(sha256
  src/sha256.cpp
)

# DMR voice framing needs the same FEC coding (BPTC/Golay/QR/Hamming/RS)
# real DMR bursts use for the voice LC header/terminator/embedded
# signaling -- reused directly from the vendored xlxd submodule (unmodified,
# self-contained, no dependency on the rest of xlxd) rather than hand-
# transcribed, since these are lookup-table-heavy and a single mistyped
# table entry would silently corrupt every transmission. Same GPLv3 as
# serialDV, already a dependency of every binary in this project.
add_library(dmr_fec
  third_party/xlxd/src/cbptc19696.cpp
  third_party/xlxd/src/cgolay2087.cpp
  third_party/xlxd/src/cqr1676.cpp
  third_party/xlxd/src/chamming.cpp
  third_party/xlxd/src/crs129.cpp
  third_party/xlxd/src/ccrc.cpp
  third_party/xlxd/src/cutils.cpp
)
target_include_directories(dmr_fec PUBLIC third_party/xlxd/src)

# The DmrTransport interface and the g_running flag every backend shares
# (dmr_transport.h/cpp) -- deliberately its own tiny library, independent
# of any one wire protocol, so a non-Homebrew backend (dmr_rewind below)
# doesn't need to link the Homebrew client just to get a shared global.
add_library(dmr_transport
  src/dmr_transport.cpp
)

add_library(dmr_client
  src/dmr_client.cpp
  src/dmr_voice.cpp
)
target_link_libraries(dmr_client dmr_transport sha256 dmr_fec)

add_executable(dmr_test
  src/dmr_test.cpp
)
target_link_libraries(dmr_test dmr_client)

# DMR's own capture/playback threads (dmr_audio.h/cpp) reuse dextra_audio's
# AlsaPcm/PcmQueue (protocol-agnostic ALSA glue) and only depend on the
# generic DmrTransport interface, not any one wire protocol -- so both
# DmrClient (Homebrew) and RewindClient (Open DMR Terminal) callers can
# use captureThread/makeVoiceRxHandler/playbackThread as-is.
add_library(dmr_audio
  src/dmr_audio.cpp
)
target_link_libraries(dmr_audio dmr_transport dextra_audio)

# BrandMeister's Open DMR Terminal (Rewind) protocol -- see dmr_rewind.h's
# header comment. Implements DmrTransport, same as dmr_client; shares
# sha256 with it for its own challenge/response auth.
add_library(dmr_rewind
  src/dmr_rewind.cpp
)
target_link_libraries(dmr_rewind dmr_transport sha256)

add_executable(odt_test
  src/odt_test.cpp
)
target_link_libraries(odt_test dmr_rewind dmr_audio)

find_package(Qt6 QUIET COMPONENTS Widgets Network)
if(Qt6_FOUND)
  set(CMAKE_AUTOMOC ON)

  add_executable(dv3000client_gui
    src/gui/audiolevelspanel.cpp
    src/gui/dmriddirectory.cpp
    src/gui/dmrtab.cpp
    src/gui/dstartab.cpp
    src/gui/localcache.cpp
    src/gui/filelogging.cpp
    src/gui/main.cpp
    src/gui/mainwindow.cpp
    src/gui/protocoltab.h
    src/gui/reflectorlistmodel.cpp
    src/gui/settings.cpp
    src/gui/settingsdialog.cpp
    src/gui/talkgroupdirectory.cpp
    src/gui/talkgrouplistmodel.cpp
  )
  target_link_libraries(dv3000client_gui dextra_audio dmr_audio dmr_client dmr_rewind xlx_directory http_get Qt6::Widgets Qt6::Network ${ALSA_LIBRARIES})
  target_include_directories(dv3000client_gui PRIVATE src ${ALSA_INCLUDE_DIRS})
else()
  message(STATUS "Qt6 Widgets not found -- skipping dv3000client_gui")
endif()
