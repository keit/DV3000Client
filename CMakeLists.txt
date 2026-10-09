cmake_minimum_required(VERSION 3.17.0)
project(dv3000client VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Release candidates are built from the same project() version with a
# suffix, set by the release workflow from a vX.Y.Z-rc.N tag: shown as
# "0.1.0-rc.1", and packaged as "0.1.0~rc.1" -- Debian sorts "~" before
# anything, so the final 0.1.0 .deb still upgrades an installed RC.
set(DV3K_PRERELEASE "" CACHE STRING "Pre-release suffix, e.g. rc.1 (empty for a release)")
if(DV3K_PRERELEASE)
  set(DV3K_VERSION "${PROJECT_VERSION}-${DV3K_PRERELEASE}")
  set(DV3K_DEB_VERSION "${PROJECT_VERSION}~${DV3K_PRERELEASE}")
else()
  set(DV3K_VERSION "${PROJECT_VERSION}")
  set(DV3K_DEB_VERSION "${PROJECT_VERSION}")
endif()

include(GNUInstallDirs)

# serialDV (our fork, vendored as a submodule) is compiled straight in as a
# static library rather than via its own CMakeLists -- that one builds a
# shared libserialdv and installs it plus its headers system-wide, which a
# packaged dv3kclient must not do (it would ship a modified fork under the
# same name as Debian's own libserialdv).
add_library(serialdv STATIC
  third_party/serialDV/datacontroller.cpp
  third_party/serialDV/dummydatacontroller.cpp
  third_party/serialDV/dvcontroller.cpp
  third_party/serialDV/serialdatacontroller.cpp
  third_party/serialDV/udpdatacontroller.cpp
)
target_compile_definitions(serialdv PUBLIC serialdv_STATIC)
target_include_directories(serialdv PUBLIC third_party/serialDV)

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

# Keeps several ThumbDV requests in flight instead of waiting for each
# reply -- see vocoder_pipeline.h. Used by every live audio path.
add_library(vocoder_pipeline
  src/vocoder_pipeline.cpp
)
target_link_libraries(vocoder_pipeline serialdv)

add_library(dextra_client
  src/dextra_client.cpp
)
target_link_libraries(dextra_client serialdv vocoder_pipeline)
target_include_directories(dextra_client PUBLIC third_party/serialDV)

# Protocol-independent ALSA audio (streams, RX queue, playback thread,
# capture pacing) shared by the D-Star and DMR live-audio modules.
add_library(alsa_audio
  src/alsa_audio.cpp
)
target_link_libraries(alsa_audio serialdv ${ALSA_LIBRARIES})
target_include_directories(alsa_audio PUBLIC third_party/serialDV ${ALSA_INCLUDE_DIRS})

add_library(dextra_audio
  src/dextra_audio.cpp
)
target_link_libraries(dextra_audio dextra_client alsa_audio)

add_executable(dextra_test
  src/dextra_test.cpp
)
target_link_libraries(dextra_test dextra_audio xlx_directory)

# Asks a local FTDI ThumbDV for a 1 ms latency timer and reports what it
# got -- see ftdi_latency.h.
add_library(ftdi_latency
  src/ftdi_latency.cpp
)

add_library(sha256
  src/sha256.cpp
)

# DMR voice framing needs the same FEC coding (BPTC/Golay/QR/Hamming/RS)
# real DMR bursts use for the voice LC header/terminator/embedded
# signaling -- reused directly from the vendored xlxd submodule (unmodified,
# self-contained, no dependency on the rest of xlxd) rather than hand-
# transcribed, since these are lookup-table-heavy and a single mistyped
# table entry would silently corrupt every transmission. Their GPLv2+
# licence is compatible with the GPLv3 code here.
#
# Except their small cutils.h/.cpp helper, which is GPLv2-only and so
# can't be combined with GPLv3 code: src/dmr_fec has our own replacement.
# The FEC sources are copied next to it in the build tree, so their
# #include "cutils.h" (a same-directory include, which no include path
# could override) finds ours instead of xlxd's.
set(DMR_FEC_DIR ${CMAKE_BINARY_DIR}/dmr_fec)
set(DMR_FEC_SOURCES)
foreach(name cbptc19696 cgolay2087 cqr1676 chamming crs129 ccrc)
  configure_file(third_party/xlxd/src/${name}.h ${DMR_FEC_DIR}/${name}.h COPYONLY)
  configure_file(third_party/xlxd/src/${name}.cpp ${DMR_FEC_DIR}/${name}.cpp COPYONLY)
  list(APPEND DMR_FEC_SOURCES ${DMR_FEC_DIR}/${name}.cpp)
endforeach()
foreach(file cutils.h cutils.cpp)
  configure_file(src/dmr_fec/${file} ${DMR_FEC_DIR}/${file} COPYONLY)
endforeach()
add_library(dmr_fec ${DMR_FEC_SOURCES} ${DMR_FEC_DIR}/cutils.cpp)
target_include_directories(dmr_fec PUBLIC ${DMR_FEC_DIR})

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

# DMR's own capture thread and RX decode handler (dmr_audio.h/cpp) use
# alsa_audio for the ALSA side and only depend on the generic DmrTransport
# interface, not any one wire protocol -- so both DmrClient (Homebrew) and
# RewindClient (Open DMR Terminal) callers can use them as-is.
add_library(dmr_audio
  src/dmr_audio.cpp
)
target_link_libraries(dmr_audio dmr_transport alsa_audio vocoder_pipeline)

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
  set(CMAKE_AUTORCC ON)

  add_executable(dv3kclient
    src/gui/audiolevelspanel.cpp
    src/gui/dmriddirectory.cpp
    src/gui/dmrtab.cpp
    src/gui/dstartab.cpp
    src/gui/localcache.cpp
    src/gui/masterdirectory.cpp
    src/gui/filelogging.cpp
    src/gui/main.cpp
    src/gui/mainwindow.cpp
    src/gui/protocoltab.h
    src/gui/reflectorlistmodel.cpp
    src/gui/settings.cpp
    src/gui/settingsdialog.cpp
    src/gui/talkgroupdirectory.cpp
    src/gui/talkgrouplistmodel.cpp
    resources/dv3kclient.qrc
  )
  target_link_libraries(dv3kclient alsa_audio dextra_audio dmr_audio dmr_client dmr_rewind ftdi_latency xlx_directory http_get Qt6::Widgets Qt6::Network ${ALSA_LIBRARIES})
  target_include_directories(dv3kclient PRIVATE src ${ALSA_INCLUDE_DIRS})
  # Shown in Help > About and --version; bump it in project() above.
  target_compile_definitions(dv3kclient PRIVATE DV3K_VERSION="${DV3K_VERSION}")

  # What the .deb and AppImage are built from (cmake --install): just the
  # GUI, its launcher entry and icons. The command-line test tools above
  # stay build-tree only.
  install(TARGETS dv3kclient RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES resources/dv3kclient.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  foreach(size 16 24 32 48 64 128 256)
    install(FILES resources/icons/dv3kclient-${size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
            RENAME dv3kclient.png)
  endforeach()
  install(FILES packaging/copyright DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/dv3kclient)
else()
  message(STATUS "Qt6 Widgets not found -- skipping dv3kclient")
endif()

# .deb packaging: `cpack -G DEB` in the build directory, after building.
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME dv3kclient)
set(CPACK_PACKAGE_VERSION ${DV3K_DEB_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "D-Star and DMR client for the ThumbDV (AMBE-3000)")
set(CPACK_PACKAGE_DESCRIPTION
  "Connects a ThumbDV (or an AMBEServer) to D-Star DExtra reflectors and\n\
DMR networks (BrandMeister, TGIF) from the desktop, with push-to-talk\n\
audio through the PC's microphone and speakers.")
set(CPACK_PACKAGE_CONTACT "Kei Takashima <takashima.kei@gmail.com>")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_STRIP_FILES ON)
# Otherwise directories get the building user's umask (e.g. 775).
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
  OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT) # dv3kclient_<version>_<arch>.deb
set(CPACK_DEBIAN_PACKAGE_SECTION hamradio)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/keit/DV3000Client")
# Dependencies are listed by hand rather than generated by dpkg-shlibdeps,
# so one .deb per architecture installs on every release it supports. The
# packages are built on Debian 12 (bookworm, the base of Raspberry Pi OS
# Bookworm), the oldest supported, and package names differ between
# releases: bookworm has libasound2 and libqt6*6, Ubuntu 24.04 renamed
# them libasound2t64 and libqt6*6t64, and 26.04 dropped the suffix again
# for some -- shlibdeps would only ever write the build system's names.
# Minimum versions are what shlibdeps generates on bookworm. The release
# workflow fails if the binary needs a package, or a newer version of one,
# than this list says, and installs each .deb on every supported release.
# Qt's platform plugins are loaded at runtime, so shlibdeps couldn't see
# those anyway.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
  "libc6 (>= 2.34), libgcc-s1 (>= 3.0), libstdc++6 (>= 11), \
libasound2t64 (>= 1.0.16) | libasound2 (>= 1.0.16), \
libqt6core6t64 (>= 6.4.0) | libqt6core6 (>= 6.4.0), \
libqt6gui6t64 (>= 6.1.2) | libqt6gui6 (>= 6.1.2), \
libqt6network6t64 (>= 6.1.2) | libqt6network6 (>= 6.1.2), \
libqt6widgets6t64 (>= 6.3.0) | libqt6widgets6 (>= 6.3.0), \
qt6-qpa-plugins")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-wayland")
include(CPack)
