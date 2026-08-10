# Native build for the replay viewer.
#
# Produces libbwreplay.so, which contains the OpenBW engine, openbw's SDL2
# rendering backend, the shared playback core and the JNI bridge. SDL2 itself is
# built from the pinned submodule as libSDL2.so; ViewerActivity loads the two in
# that order.
#
# Both dependencies are git submodules so the build is reproducible and works
# offline once cloned:
#   git submodule update --init --recursive

cmake_minimum_required(VERSION 3.22.1)
project(bwreplay CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(ANDROID_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(SDL_DIR "${ANDROID_PROJECT_DIR}/third_party/SDL")
set(OPENBW_DIR "${ANDROID_PROJECT_DIR}/third_party/openbw")

if (NOT EXISTS "${SDL_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "SDL submodule missing. Run: git submodule update --init --recursive")
endif()
if (NOT EXISTS "${OPENBW_DIR}/bwgame.h")
  message(FATAL_ERROR "openbw submodule missing. Run: git submodule update --init --recursive")
endif()

# The viewer needs video only. Turning the rest off keeps the .so small and
# avoids pulling in the HIDAPI/joystick Java plumbing the app does not use.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDL_DIR}" sdl2 EXCLUDE_FROM_ALL)

add_library(bwreplay SHARED
  core/bwreplay_core.cpp
  android_main.cpp
  bwreplay_jni.cpp
  "${OPENBW_DIR}/ui/sdl2.cpp"
)

target_include_directories(bwreplay PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/core"
  "${OPENBW_DIR}"
  "${OPENBW_DIR}/ui"
)

# Keep in lockstep with tools/native-test/CMakeLists.txt so the desktop harness
# exercises the same code. Sound needs SDL_mixer, which is not built here, so
# native_sound compiles to stubs and playback is silent.
target_compile_definitions(bwreplay PRIVATE
  OPENBW_ENABLE_UI
  OPENBW_NO_SDL_IMAGE
  OPENBW_NO_SDL_MIXER
)

target_link_libraries(bwreplay PRIVATE SDL2 android log)
