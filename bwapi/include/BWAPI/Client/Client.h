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
    /// Connects to the free server in the game table that has waited longest.
    bool connect();
    /// Connects to the server whose process id is serverProcID.
    bool connectTo(int serverProcID);
    void disconnect();
    void update();

    GameData *data = nullptr;

  private:
    int syncSocket = -1;
    int mapFileHandle = 0;
    int gameTableFileHandle = 0;
    GameTable *gameTable = nullptr;
    bool connected = false;
    bool frameHeld = false;   // a frame the server handed over that update() has not handed back yet
  };
  extern Client BWAPIClient;
}
