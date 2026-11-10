# The BWAPI client library: a game served by a BWAPI server in another process, over the POSIX transport (Client.cpp).
# Links BWAPILIB only: the server library BWAPI defines its own BWAPI::GameImpl.
add_library(BWAPIClient STATIC
  Source/Client.cpp
  Source/GameImpl.cpp
  Source/UnitImpl.cpp
  Source/PlayerImpl.cpp
  Source/ForceImpl.cpp
  Source/BulletImpl.cpp
  Source/RegionImpl.cpp
  ../Shared/GameShared.cpp
  ../Shared/UnitShared.cpp
  ../Shared/PlayerShared.cpp
  ../Shared/ForceShared.cpp
  ../Shared/BulletShared.cpp
  ../Shared/RegionShared.cpp
)

target_include_directories(BWAPIClient PRIVATE ../include/BWAPI/Client ../Util/Source ../BWAPICore ../Shared Source)

target_compile_definitions(BWAPIClient PRIVATE BWAPI_FINDER_ID_MAX=10000)

target_link_libraries(BWAPIClient BWAPILIB)

set_property(TARGET BWAPIClient PROPERTY POSITION_INDEPENDENT_CODE ON)

install(TARGETS BWAPIClient
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)
