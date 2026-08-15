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
    {
      std::cout << "Already connected." << std::endl;
      return true;
    }
#ifndef _WIN32

    int serverProcID = -1;
    int gameTableIndex = -1;

    this->gameTable = NULL;
    this->gameTableFileHandle = shm_open("/bwapi_shared_memory_game_list", O_RDWR | O_CREAT, S_IRWXU);
    if (this->gameTableFileHandle < 0)
    {
      std::cerr << "Game table mapping not found." << std::endl;
      return false;
    }
    this->gameTable = static_cast<GameTable *>(mmap(NULL, sizeof(GameTable), PROT_WRITE | PROT_READ, MAP_SHARED, gameTableFileHandle, 0));
    if (!this->gameTable)
    {
      std::cerr << "Unable to map Game table." << std::endl;
      return false;
    }

    //Find row with most recent keep alive that isn't connected
    unsigned int latest = 0;
    for (int i = 0; i < GameTable::MAX_GAME_INSTANCES; i++)
    {
      std::cout << i << " | " << gameTable->gameInstances[i].serverProcessID << " | " << gameTable->gameInstances[i].isConnected << " | " << gameTable->gameInstances[i].lastKeepAliveTime << std::endl;
      if (gameTable->gameInstances[i].serverProcessID != 0 && !gameTable->gameInstances[i].isConnected)
      {
        if (gameTableIndex == -1 || latest == 0 || gameTable->gameInstances[i].lastKeepAliveTime < latest)
        {
          latest = gameTable->gameInstances[i].lastKeepAliveTime;
          gameTableIndex = i;
        }
      }
    }

    if (gameTableIndex != -1)
      serverProcID = gameTable->gameInstances[gameTableIndex].serverProcessID;

    if (serverProcID == -1)
    {
      std::cerr << "No server proc ID" << std::endl;
      return false;
    }

    std::stringstream sharedMemoryName;
    sharedMemoryName << "/bwapi_shared_memory_";
    sharedMemoryName << serverProcID;

    std::stringstream communicationSocket;
    communicationSocket << "/tmp/bwapi_socket_";
    communicationSocket << serverProcID;

    struct sockaddr_un name;
    memset(&name, 0, sizeof(struct sockaddr_un));

    name.sun_family = AF_UNIX;
    strncpy(name.sun_path, communicationSocket.str().c_str(), communicationSocket.str().length());
    syncSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (syncSocket < 0)
    {
      std::cerr << "Unable to open communications socket" << std::endl;
      shm_unlink(communicationSocket.str().c_str());
      shm_unlink("/bwapi_shared_memory_game_list");
      munmap(gameTable, sizeof(GameTable));
      return false;
    }
    if (::connect(syncSocket, (const struct sockaddr *)&name, sizeof(struct sockaddr_un)) == 0)
    {
      std::cerr << "Unable to open communications socket: " << communicationSocket.str() << std::endl;
      shm_unlink(communicationSocket.str().c_str());
      shm_unlink("/bwapi_shared_memory_game_list");
      munmap(gameTable, sizeof(GameTable));
      return false;
    }

    std::cout << "Connected" << std::endl;

    mapFileHandle = shm_open(sharedMemoryName.str().c_str(), O_RDWR | O_CREAT, S_IRWXU);
    if (mapFileHandle < 0)
    {
      std::cerr << "Unable to open shared memory mapping: " << sharedMemoryName.str() << std::endl;
      shm_unlink(communicationSocket.str().c_str());
      shm_unlink("/bwapi_shared_memory_game_list");
      munmap(gameTable, sizeof(GameTable));
      return false;
    }
    data = static_cast<GameData *>(mmap(NULL, sizeof(GameData), PROT_WRITE | PROT_READ, MAP_SHARED, mapFileHandle, 0));
    if (data == nullptr)
    {
      std::cerr << "Unable to map game data." << std::endl;
      return false;
    }

    // Create new instance of Game/Broodwar
    if (BWAPI::BroodwarPtr)
      delete static_cast<GameImpl *>(BWAPI::BroodwarPtr);
    //    BWAPI::BroodwarPtr = new GameImpl(data);
    assert(BWAPI::BroodwarPtr != nullptr);

    if (BWAPI::CLIENT_VERSION != BWAPI::Broodwar->getClientVersion())
    {
      //error
      std::cerr << "Error: Client and Server are not compatible!" << std::endl;
      std::cerr << "Client version: " << BWAPI::CLIENT_VERSION << std::endl;
      std::cerr << "Server version: " << BWAPI::Broodwar->getClientVersion() << std::endl;
      disconnect();
      std::this_thread::sleep_for(std::chrono::milliseconds{2000});
      return false;
    }

    std::cout << "Connection successful" << std::endl;
    assert(BWAPI::BroodwarPtr != nullptr);

    this->connected = true;
    return true;
#endif
  }
  void Client::disconnect()
  {
  }
  void Client::update()
  {
  }
}
