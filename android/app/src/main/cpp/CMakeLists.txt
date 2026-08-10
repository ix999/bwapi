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

# Build SDL with its Android defaults. Trimming subsystems looks tempting
# because the viewer only needs video, but SDL's Android build does not support
# it: src/core/android/SDL_android.c calls Android_AddHaptic/Android_RemoveHaptic
# unguarded, so SDL_HAPTIC=OFF fails to compile. The same shape of problem
# applies to SDL_HIDAPI, whose HIDDeviceManager.java (compiled here along with
# the rest of SDL's Java sources) binds to native methods that would no longer
# exist. Only the shared/static/test switches are safe to set.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
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
