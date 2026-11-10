#include <BWAPI/Client/Client.h>
#include <stdexcept>
#include <iostream>

#ifndef _WIN32
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cassert>
#include <thread>
#include <chrono>
#include <string>
#include <cstring>
#include <unistd.h>
#endif

namespace BWAPI
{
  class ClientImpl {
  };

  Client BWAPIClient;
  Client::Client()
  {}
  Client::~Client()
  {
    this->disconnect();
  }
  bool Client::isConnected() const
  {
    return this->connected;
  }
  bool Client::connect()
  {
    if (this->connected)
      return true;
#ifndef _WIN32
    int fd = shm_open("/bwapi_shared_memory_game_list", O_RDONLY, 0);
    if (fd < 0)
      return false;
    GameTable *table = static_cast<GameTable *>(mmap(NULL, sizeof(GameTable), PROT_READ, MAP_SHARED, fd, 0));
    close(fd);
    if (table == MAP_FAILED)
      return false;
    // the free row (a server, no client yet) whose keep-alive time is the smallest
    int serverProcID = -1;
    unsigned int latest = 0;
    for (int i = 0; i < GameTable::MAX_GAME_INSTANCES; i++)
    {
      const GameInstance &g = table->gameInstances[i];
      if (g.serverProcessID != 0 && !g.isConnected && (serverProcID == -1 || g.lastKeepAliveTime < latest))
      {
        serverProcID = g.serverProcessID;
        latest = g.lastKeepAliveTime;
      }
    }
    munmap(table, sizeof(GameTable));
    return serverProcID != -1 && connectTo(serverProcID);
#else
    return false;
#endif
  }
  bool Client::connectTo(int serverProcID)
  {
    if (this->connected)
      return true;
#ifndef _WIN32
    // the game data is mapped before the socket connects: the server unlinks its name once it accepts a client
    std::string shm = "/bwapi_shared_memory_" + std::to_string(serverProcID);
    mapFileHandle = shm_open(shm.c_str(), O_RDWR, 0);
    if (mapFileHandle < 0)
      return false;
    void *m = mmap(NULL, sizeof(GameData), PROT_WRITE | PROT_READ, MAP_SHARED, mapFileHandle, 0);
    close(mapFileHandle);
    mapFileHandle = -1;
    if (m == MAP_FAILED)
      return false;
    data = static_cast<GameData *>(m);
    if (data->client_version != BWAPI::CLIENT_VERSION)
    {
      std::cerr << "Error: Client and Server are not compatible! client " << BWAPI::CLIENT_VERSION
                << ", server " << data->client_version << std::endl;
      munmap(data, sizeof(GameData));
      data = nullptr;
      return false;
    }
    struct sockaddr_un name;
    memset(&name, 0, sizeof(name));
    name.sun_family = AF_UNIX;
    std::string sock = "/tmp/bwapi_socket_" + std::to_string(serverProcID);
    strncpy(name.sun_path, sock.c_str(), sizeof(name.sun_path) - 1);
    syncSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (syncSocket < 0 || ::connect(syncSocket, (const struct sockaddr *)&name, sizeof(name)) != 0)
    {
      if (syncSocket >= 0)
        close(syncSocket);
      syncSocket = -1;
      munmap(data, sizeof(GameData));
      data = nullptr;
      return false;
    }
    if (BWAPI::BroodwarPtr)
      delete static_cast<GameImpl *>(BWAPI::BroodwarPtr);
    BWAPI::BroodwarPtr = new GameImpl(data);
    frameHeld = false;
    this->connected = true;
    return true;
#else
    return false;
#endif
  }
  void Client::disconnect()
  {
#ifndef _WIN32
    if (syncSocket >= 0)
      close(syncSocket);
    syncSocket = -1;
    if (data)
      munmap(data, sizeof(GameData));
    data = nullptr;
#endif
    frameHeld = false;
    connected = false;
  }
  void Client::update()
  {
#ifndef _WIN32
    if (!connected)
      return;
    // hands back the frame held (its commands are in the game data), then waits for the server's next one
    char code = 1;
    if (frameHeld && write(syncSocket, &code, 1) != 1)
    {
      disconnect();
      return;
    }
    frameHeld = false;
    do
    {
      if (read(syncSocket, &code, 1) != 1)
      {
        disconnect();
        return;
      }
    } while (code != 2);
    frameHeld = true;
    GameImpl *game = static_cast<GameImpl *>(BWAPI::BroodwarPtr);
    bool started = false;
    for (int i = 0; i < data->eventCount; ++i)
      started |= data->events[i].type == EventType::MatchStart;
    if (started)
      game->onMatchStart();
    else if (data->isInGame)
      game->onMatchFrame();
#endif
  }
}
