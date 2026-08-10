# Desktop (Linux/macOS) harness for the shared replay core.
#
# The core in android/app/src/main/cpp/core is platform independent, so it can
# be compiled and exercised without an Android toolchain. This is what CI and
# local development use to check the engine integration; the Android build
# compiles the exact same sources through app/src/main/cpp/CMakeLists.txt.
#
#   cmake -S tools/native-test -B build-native -DOPENBW_DIR=/path/to/openbw
#   cmake --build build-native
#   SDL_VIDEODRIVER=dummy ./build-native/bwreplay-native-test <data-dir> <replay.rep>

cmake_minimum_required(VERSION 3.16)
project(bwreplay-native-test CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_path(OPENBW_DIR bwgame.h PATHS ENV OPENBW_DIR)
if (NOT OPENBW_DIR)
  message(FATAL_ERROR "Couldn't find openbw. Pass -DOPENBW_DIR=/path/to/openbw")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

set(CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../android/app/src/main/cpp/core)

add_executable(bwreplay-native-test
  ${CMAKE_CURRENT_LIST_DIR}/main.cpp
  ${CORE_DIR}/bwreplay_core.cpp
  ${OPENBW_DIR}/ui/sdl2.cpp
)

target_include_directories(bwreplay-native-test PRIVATE
  ${CORE_DIR}
  ${OPENBW_DIR}
  ${OPENBW_DIR}/ui
  ${SDL2_INCLUDE_DIRS}
)

# Keep these in lockstep with the Android build so both compile the same code.
target_compile_definitions(bwreplay-native-test PRIVATE
  OPENBW_ENABLE_UI
  OPENBW_NO_SDL_IMAGE
  OPENBW_NO_SDL_MIXER
)

target_link_directories(bwreplay-native-test PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(bwreplay-native-test PRIVATE ${SDL2_LIBRARIES})
