
option(OPENBW_ENABLE_UI "Enables support for the graphical user interface, requires SDL2")

find_path(OPENBW_DIR bwgame.h
  PATHS
  ENV OPENBW_DIR
)

if (NOT OPENBW_DIR)
  message(FATAL_ERROR "Couldn't find openbw. Specify the path to it with the OPENBW_DIR variable")
endif()

if (OPENBW_ENABLE_UI)
  add_subdirectory(${OPENBW_DIR}/ui openbw_ui)
  add_definitions(-DOPENBW_ENABLE_UI)
endif()

include_directories(
  ../Util/Source
  ../BWAPI/openbw
  ../BWAPI/Source
  ${OPENBW_DIR}
)

if (OPENBWAPI_BUILD_STATIC_LIBS)
  add_library(OpenBWData STATIC
    BW/BWData.cpp
  )
else()
  add_library(OpenBWData SHARED
    BW/BWData.cpp
  )
endif()

set_property(TARGET OpenBWData PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET OpenBWData PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS ON)

# sb-perf: platform-specific code generation for the sim translation unit, with the
# standard build as the fallback on every other platform (CI builds Linux x86_64 and is
# untouched). The sim is integer-deterministic, so scheduling/selection changes cannot
# alter outputs; the equivalence/oracle/dual ladder gates every configuration anyway.
#  - Apple Silicon: tune for the host core (NEON is baseline; M2 = ARMv8.6-class).
#  - SB_PGO=generate builds an instrumented sim; SB_PGO=use consumes SB_PGO_PROFDATA
#    (a merged .profdata) for profile-guided layout/inlining. Empty = plain build.
if (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
  target_compile_options(OpenBWData PRIVATE -mcpu=native)
endif()
set(SB_PGO "" CACHE STRING "PGO mode for the sim TU: generate | use (empty = off)")
set(SB_PGO_PROFDATA "" CACHE FILEPATH "merged .profdata for SB_PGO=use")
if (SB_PGO STREQUAL "generate")
  target_compile_options(OpenBWData PRIVATE -fprofile-generate)
  target_link_options(OpenBWData PRIVATE -fprofile-generate)
elseif (SB_PGO STREQUAL "use")
  target_compile_options(OpenBWData PRIVATE
    -fprofile-use=${SB_PGO_PROFDATA}
    -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled)
  target_link_options(OpenBWData PRIVATE -fprofile-use=${SB_PGO_PROFDATA})
endif()

if (OPENBW_ENABLE_UI)
  target_link_libraries(OpenBWData openbw_ui)
endif()

if (NOT WIN32)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
endif()
find_package(Threads REQUIRED)
target_link_libraries(OpenBWData Threads::Threads)

if (WIN32)
  target_link_libraries(OpenBWData "ws2_32;mswsock")
endif()

install(TARGETS OpenBWData
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)
