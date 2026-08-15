#pragma once

#include "GameData.h"
#include "GameImpl.h"
#include "ForceImpl.h"
#include "PlayerImpl.h"
#include "UnitImpl.h"
#include "GameTable.h"

namespace BWAPI
{
  class ClientImpl;
  class Client
  {
  public:
    Client();
    ~Client();

    bool isConnected() const;
    bool connect();
    void disconnect();
    void update();

    GameData *data = nullptr;

  private:
    int syncSocket = -1;
    int mapFileHandle = 0;
    int gameTableFileHandle = 0;
    GameTable *gameTable = nullptr;
    bool connected = false;
  };
  extern Client BWAPIClient;
}
