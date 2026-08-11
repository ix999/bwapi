#include "GameImpl.h"
#include <vector>
#include <string>

#include "../WMode.h"

#include <BWAPI/BWtoBWAPI.h>
#include <BWAPI/UnitImpl.h>
#include <BWAPI/PlayerImpl.h>
#include <BWAPI/BulletImpl.h>
#include <BWAPI/RegionImpl.h>
#include "Command.h"

#include "../../../Debug.h"

#include "BW/BWData.h"

#include <chrono>
#include <type_traits>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/*
  This files holds all functions of the GameImpl class that are not part of the Game interface.
 */

namespace BWAPI
{
  struct BroodwarImpl_storage_t {
    std::aligned_storage<sizeof(GameImpl), alignof(GameImpl)>::type buf;
    bool inited;
    ~BroodwarImpl_storage_t() {
      if (inited) (**this).~GameImpl();
    }
    GameImpl& operator*() {
      return *(GameImpl*)&buf;
    }
    template<typename... args_T>
    void construct(args_T&&... args) {
      if (inited) throw std::runtime_error("BroodwarImpl_storage_t::construct:: already constructed");
      inited = true;
      new ((GameImpl*)&buf) GameImpl(std::forward<args_T>(args)...);
    }
    void destroy() {
      if (!inited) throw std::runtime_error("BroodwarImpl_storage_t::destroy:: not constructed");
      inited = false;
      (**this).~GameImpl();
    }
  };
  // Dual-host: per-thread GameImpl storage — each in-process bot runs on its own dispatch
  // thread, so every by-name BroodwarImpl reference inside BWAPI resolves to that bot's own
  // mirror. Single-threaded hosts construct exactly one, on the main thread, as before.
  thread_local BroodwarImpl_storage_t BroodwarImpl_storage BWAPI_TLS_IE;
  thread_local GameImpl& BroodwarImpl BWAPI_TLS_IE = (GameImpl&)BroodwarImpl_storage.buf;
  
  BroodwarImpl_handle::BroodwarImpl_handle(BW::Game bwgame) {
    BroodwarImpl_storage.construct(bwgame);
  }
  BroodwarImpl_handle::~BroodwarImpl_handle() {
    BroodwarImpl_storage.destroy();
  }
  
  GameImpl& BroodwarImpl_handle::operator*()
  {
    return BroodwarImpl;
  }
  
  GameImpl* BroodwarImpl_handle::operator->()
  {
    return &BroodwarImpl;
  }

  //---------------------------------------------- CONSTRUCTOR -----------------------------------------------
  GameImpl::GameImpl(BW::Game bwgame) :
    bwgame(bwgame)
    , map(bwgame)
    , autoMenuManager(bwgame)
  {
    BWAPI::BroodwarPtr = static_cast<Game*>(this);

    BWtoBWAPI_init();

    // iterate through players and create PlayerImpl for each
    for (u8 i = 0; i < BW::PLAYER_COUNT; ++i)
      players[i] = new PlayerImpl(i, bwgame.getPlayer(i));

    // iterate through units and create UnitImpl for each
    for (u16 i = 0; i < BW::UNIT_ARRAY_MAX_LENGTH; ++i)
      unitArray[i] = new UnitImpl(bwgame.getUnit((size_t)i), i);

    // iterate through bullets and create BulletImpl for each
    for (u16 i = 0; i < BW::BULLET_ARRAY_MAX_LENGTH; ++i)
      bulletArray[i] = new BulletImpl(bwgame.getBullet((size_t)i));

    bwgame.setOnDraw([this](uint8_t*, size_t) {
      drawShapes();
    });

    this->initializeData();

    bwgame.setCustomActionCallback(std::bind(&GameImpl::onCustomAction, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
  }
  //----------------------------------------------- DESTRUCTOR -----------------------------------------------
  GameImpl::~GameImpl()
  {
    this->initializeData();

    // destroy all UnitImpl
    for (UnitImpl* u : unitArray)
    {
      if (u) delete u;
    }
    unitArray.fill(nullptr);

    // destroy all PlayerImpl
    for (size_t i = 0; i < std::extent<decltype(players)>::value; ++i)
    {
      if (players[i]) delete players[i];
      players[i] = nullptr;
    }

    // destroy all bullets
    for (BulletImpl* b : bulletArray)
    {
      if (b) delete b;
    }
    bulletArray.fill(nullptr);
  }
  //---------------------------------------- REFRESH SELECTION STATES ----------------------------------------
  void GameImpl::refreshSelectionStates()
  {
    for (UnitImpl* u : unitArray)
    {
      if (u) u->setSelected(false);
    }

    // fixme
//    selectedUnitSet.clear();
//    for (int i = 0; i < BW::BWDATA::ClientSelectionCount && i < BW::MAX_SELECTION_COUNT; ++i)
//    {
//      BWAPI::UnitImpl *u = UnitImpl::BWUnitToBWAPIUnit(BW::BWDATA::ClientSelectionGroup[i]);
//      if (u)
//      {
//        u->setSelected(true);
//        selectedUnitSet.insert(u);
//      }
//    }
  }
  void GameImpl::dropPlayers()
  {
    // fixme
//    for ( int i = 0; i < BW::PLAYABLE_PLAYER_COUNT; ++i )
//    {
//      if ( BW::BWDATA::playerStatusArray[i] & 0x10000 )
//      {
//        int iplr = this->stormIdToPlayerId(i);
//        if ( iplr != -1 && iplr != BW::BWDATA::g_LocalHumanID )
//        {
//          this->droppedPlayers.push_back(this->players[iplr]);
//          SNetDropPlayer(i, 0x40000006);  // The value used when dropping
//        }
//      }
//    }
  }
  //------------------------------------------------ MOUSE/KEY INPUT -----------------------------------------
  void GameImpl::pressKey(int key)
  {
    // Don't do anything if key is 0
    // used when auto-menu dialogs are not found, performance
    if ( !key )
      return;

    // Press and release the key
    //PostMessage(SDrawGetFrameWindow(), WM_CHAR, (WPARAM)key, NULL);
  }
  void GameImpl::mouseDown(int x, int y)
  {
    // Press the left mouse button
    //PostMessage(SDrawGetFrameWindow(), WM_LBUTTONDOWN, NULL, (LPARAM)MAKELONG(x,y));
  }
  void GameImpl::mouseUp(int x, int y)
  {
    // Release the left mouse button
    //PostMessage(SDrawGetFrameWindow(), WM_LBUTTONUP, NULL, (LPARAM)MAKELONG(x,y));
  }
  //------------------------------------------- PLAYER ID CONVERT --------------------------------------------
  int GameImpl::stormIdToPlayerId(int dwStormId)
  {
    /* Translates a storm ID to a player Index */
    // fixme
//    for (int i = 0; i < BW::PLAYER_COUNT; ++i)
//    {
//      if ( BW::BWDATA::Players[i].dwStormId == dwStormId )
//        return i;
//    }
    return -1;
  }
  //----------------------------------------------- PARSE TEXT -----------------------------------------------
  bool GameImpl::parseText(const std::string &text)
  {
    // analyze the string
    std::stringstream ss(text);
    std::string cmd;
    int n;

    ss >> cmd;

    // commands list
    if (cmd == "/leave")
    {
      this->leaveGame();
    }
    else if (cmd == "/speed")
    {
      n = -1;
      ss >> n;
      setLocalSpeedDirect(n);
      Broodwar << "Changed game speed" << std::endl;
    }
    else if (cmd == "/fs")
    {
      n = 1;
      ss >> n;
      setFrameSkip(n);
      Broodwar << "Altered frame skip" << std::endl;
    }
    else if (cmd == "/cheats")
    {
      sendText("power overwhelming");
      sendText("operation cwal");
      sendText("the gathering");
      sendText("medieval man");
      sendText("black sheep wall");
      sendText("food for thought");
      sendText("modify the phase variance");
      sendText("something for nothing");
      sendText("something for nothing");
      sendText("something for nothing");
      sendText("show me the money");
      sendText("show me the money");
      sendText("show me the money");
      sendText("show me the money");
      sendText("show me the money");
    }
    else if (cmd == "/restart")
    {
      restartGame();
    }
    else if (cmd == "/nogui")
    {
      setGUI(!data->hasGUI);
      Broodwar << "GUI: " << (data->hasGUI ? "enabled" : "disabled") << std::endl;
    }
    else if (cmd == "/wmode")
    {
      //SetWMode(BW::BWDATA::GameScreenBuffer.width(), BW::BWDATA::GameScreenBuffer.height(), !wmode);
      Broodwar << "Toggled windowed mode." << std::endl;
    }
    else if (cmd == "/grid")
    {
      grid = !grid;
      Broodwar << "Matrix grid " << (grid ? "enabled" : "disabled") << std::endl;
    }
    else if ( cmd == "/fps" )
    {
      this->showfps = !this->showfps;
      Broodwar << "FPS display " << (showfps ? "enabled" : "disabled") << std::endl;
    }
#ifdef _DEBUG
    else if (cmd == "/latency")
    {
      Broodwar << "Latency: " << getLatency() << std::endl;
      Broodwar << "New latency: " << getLatencyFrames() << " frames (" << getLatencyTime() << "ms)" << std::endl;
    }
    else if (cmd == "/test")
    {
    }
#endif
    else
    {
      return false;
    }
    return true;
  }
  //------------------------------------------- INTERFACE EVENT UPDATE ---------------------------------------
  // ---- interface-event walk skip (sb-perf) -----------------------------------------------
  // SB_INTERFACE_EVENT_SKIP: unset/1 = on (default), 0 = off (kill-switch), verify = walk
  // anyway and assert the invariant the skip rests on. Read once; getenv is not hot-path safe.
  static bool interfaceEventSkipEnabled()
  {
    static const bool on = [] {
      const char* v = std::getenv("SB_INTERFACE_EVENT_SKIP");
      return !v || (std::strcmp(v, "0") != 0);
    }();
    return on;
  }

  static bool interfaceEventVerifyMode()
  {
    static const bool verify = [] {
      const char* v = std::getenv("SB_INTERFACE_EVENT_SKIP");
      return v && std::strcmp(v, "verify") == 0;
    }();
    return verify;
  }

  long long GameImpl::interfaceEventWalkSkipped = 0;
  long long GameImpl::interfaceEventWalkRan     = 0;
  long long GameImpl::interfaceEventForeignSeen = 0;
  long long GameImpl::mirrorVerifyCompared      = 0;

  // ---- foreign-registration audit --------------------------------------------------------
  // The skip has exactly one blind spot, documented at length in Interface.h: a bot binary built
  // against UPSTREAM headers inlined a registerEvent() that does not increment liveEventCount, so
  // we would skip servicing its events forever and it would look like the bot simply did nothing.
  // Silent wrong answers are the failure mode this project likes least, so we hunt for it.
  //
  // Once every kAuditPeriod frames, for each family we are actually skipping, scan for the state
  // the counter says is impossible: a list that is non-empty while liveEventCount reads zero. On
  // a hit we say so loudly and LATCH the skip off for that family, so the bot works correctly
  // from then on rather than merely being diagnosed.
  //
  // Cost accounting, since the whole point of the skip is not to touch these objects: the audit
  // is the same traversal, run on 1 frame in kAuditPeriod, so it returns 1/kAuditPeriod of what
  // the skip saves. At 256 that is ~5 of the ~2,744 L2 misses/frame the cut removes -- under
  // 0.2% of the win. In the abnormal case we fall back to the upstream walk, which is what such
  // a bot needs anyway. The exposure is a bounded lateness, not a lost event: a foreign
  // registration is serviced within kAuditPeriod frames instead of on the next one.
  enum { kInterfaceEventAuditPeriod = 256 };

  // Per-family latch. Never cleared: once a foreign registration has been seen in this process,
  // assume it can recur and keep the upstream walk. Conservative in the safe direction.
  template <typename T>
  static bool& interfaceEventLatch()
  {
    static bool latched = false;
    return latched;
  }

  template <typename T, typename Container>
  static bool interfaceEventForeignRegistration(const Container& c, int frame, const char* family)
  {
    int found = 0;
    for (auto* o : c)
      if (!o->eventListEmpty())
        ++found;
    if (found == 0)
      return false;

    interfaceEventLatch<T>() = true;
    ++GameImpl::interfaceEventForeignSeen;
    std::printf(
      "INTERFACEEVENTS FOREIGN-REGISTRATION f=%d family=%s objects=%d\n"
      "  %d %s object(s) hold registered events that the live-event counter never saw. The AI\n"
      "  module was almost certainly built against UPSTREAM BWAPI headers, whose registerEvent()\n"
      "  does not participate in the counting this engine's walk-skip relies on\n"
      "  (docs/design/ENGINE_OPT_INTERFACE_EVENTS.md).\n"
      "  The skip is now LATCHED OFF for this family, so these events ARE being serviced from\n"
      "  here on -- but up to %d frames later than upstream would have serviced them. Rebuild the\n"
      "  bot against these headers, or run with SB_INTERFACE_EVENT_SKIP=0, to remove that delay.\n",
      frame, family, found, found, family, (int)kInterfaceEventAuditPeriod);
    std::fflush(stdout);
    return true;
  }

  // Skip predicate for one interface family. T::anyInterfaceEvents() counts live
  // InterfaceEvent<T> objects across every instance of T; zero means every list is empty, so
  // both branches of the walk below (updateEvents on an empty list, clearEvents on an empty
  // list) are no-ops and the walk has no observable effect at all. Rule 9 holds by
  // construction here rather than by measurement: there is no state to go stale.
  //
  // In verify mode we run the walk regardless AND check the invariant per object, so a
  // miscounted event surfaces as a loud INTERFACE-EVENT-VERIFY-DIFF instead of as a silently
  // dropped callback. The counter is allowed to drift high, so a false "must walk" is
  // expected and harmless; a false "may skip" is the bug this hunts.
  template <typename T, typename Container>
  static bool interfaceEventWalkNeeded(const Container& c, int frame, const char* family)
  {
    if (!interfaceEventSkipEnabled() || interfaceEventVerifyMode())
      return true;
    if (interfaceEventLatch<T>() || T::anyInterfaceEvents())
    {
      ++GameImpl::interfaceEventWalkRan;
      return true;
    }
    if (frame % (int)kInterfaceEventAuditPeriod == 0 &&
        interfaceEventForeignRegistration<T>(c, frame, family))
    {
      ++GameImpl::interfaceEventWalkRan;
      return true;
    }
    ++GameImpl::interfaceEventWalkSkipped;
    return false;
  }

  void GameImpl::processInterfaceEvents()
  {
    // GameImpl events
    this->updateEvents();

    const int ieFrame = this->getFrameCount();

    // UnitImpl events
    if (interfaceEventWalkNeeded<UnitInterface>(this->accessibleUnits, ieFrame, "Unit"))
    {
      for(Unit u : this->accessibleUnits)
      {
        if (interfaceEventVerifyMode() && !UnitInterface::anyInterfaceEvents() && !u->eventListEmpty())
          std::printf("INTERFACE-EVENT-VERIFY-DIFF f=%d unit=%d holds events while liveEventCount==0\n",
                      this->getFrameCount(), u->getID());
        u->exists() ? u->updateEvents() : u->clearEvents();
      }
    }

    // ForceImpl events
    if (interfaceEventWalkNeeded<ForceInterface>(this->forces, ieFrame, "Force"))
    {
      for (Force f : this->forces)
        f->updateEvents();
    }

    // BulletImpl events
    if (interfaceEventWalkNeeded<BulletInterface>(this->bullets, ieFrame, "Bullet"))
    {
      for (Bullet b : this->bullets)
      {
        b->exists() ? b->updateEvents() : b->clearEvents();
      }
    }

    // RegionImpl events
    if (interfaceEventWalkNeeded<RegionInterface>(this->regionsList, ieFrame, "Region"))
    {
      for (Region r : this->regionsList)
        r->updateEvents();
    }

    // PlayerImpl events
    if (interfaceEventWalkNeeded<PlayerInterface>(this->playerSet, ieFrame, "Player"))
    {
      for (Player p : this->playerSet)
        p->updateEvents();
    }
  }
  //------------------------------------------- GET PLAYER INTERNAL ------------------------------------------
  PlayerImpl *GameImpl::_getPlayer(int id)
  {
    if (id < 0 || id >= BW::PLAYER_COUNT)
      return players[BW::PLAYER_COUNT - 1];
    return players[id];
  }
  int GameImpl::_currentPlayerId()
  {
    return bwgame.g_LocalHumanID();
  }
  bool GameImpl::tournamentCheck(Tournament::ActionID type, void *parameter)
  {
    if ( this->tournamentController && !isTournamentCall )
    {
      isTournamentCall  = true;
      bool allow        = this->tournamentController->onAction(type, parameter);
      isTournamentCall  = false;
      return allow;
    }
    return true;
  }
  void GameImpl::initializeData()
  {
    // Delete forces
    for ( Forceset::iterator f = this->forces.begin(); f != this->forces.end(); ++f)
      delete (static_cast<ForceImpl*>(*f));
    this->forces.clear();

    // Remove player references
    this->BWAPIPlayer = nullptr;
    this->enemyPlayer = nullptr;

    // Set random seed
    srand((unsigned)std::chrono::high_resolution_clock::now().time_since_epoch().count());

    // clear all sets
    this->aliveUnits.clear();
    this->dyingUnits.clear();
    this->discoverUnits.clear();
    this->accessibleUnits.clear();
    this->evadeUnits.clear();
    this->selectedUnitSet.clear();
    this->startLocations.clear();
    this->playerSet.clear();
    this->minerals.clear();
    this->geysers.clear();
    this->neutralUnits.clear();
    this->bullets.clear();
    this->pylons.clear();
    this->staticMinerals.clear();
    this->staticGeysers.clear();
    this->staticNeutralUnits.clear();
    this->_allies.clear();
    this->_enemies.clear();
    this->_observers.clear();

    // Reset saved selection
    this->savedUnitSelection.fill(nullptr);
    this->wantSelectionUpdate = false;

    // Disable all game flags
    flags.fill(false);

    // Clear the latency buffer
    for(unsigned int j = 0; j < this->commandBuffer.size(); ++j)
      for (unsigned int i = 0; i < this->commandBuffer[j].size(); ++i)
        delete this->commandBuffer[j][i];
    this->commandBuffer.clear();
    this->commandBuffer.reserve(16);

    commandOptimizer.init();

    // Delete all dead units
    for ( Unitset::iterator d = this->deadUnits.begin(); d != this->deadUnits.end(); ++d )
      delete static_cast<UnitImpl*>(*d);
    this->deadUnits.clear();

    // Delete all regions
    for ( Regionset::iterator r = this->regionsList.begin(); r != this->regionsList.end(); ++r )
      delete static_cast<RegionImpl*>(*r);
    this->regionsList.clear();
    this->regionMap.clear();

    // Reset game speeds and text size
    this->setLocalSpeedDirect(this->speedOverride);
    this->setFrameSkip(1);
    this->setTextSize();
    this->setGUI(true);

    // Reset all Unit objects in the unit array
    for (UnitImpl* u : unitArray)
    {
      if (!u) continue;
      u->clear();
      u->userSelected = false;
      u->isAlive = false;
      u->wasAlive = false;
      u->wasCompleted = false;
      u->wasAccessible = false;
      u->wasVisible = false;
      u->nukeDetected = false;
      u->lastType = UnitTypes::Unknown;
      u->lastPlayer = nullptr;

      u->setID(-1);
    }

    this->bulletNextId = 0;
    this->cheatFlags  = 0;
    //this->frameCount  = -1;
    this->frameCount = 0;

    this->clientInfo.clear();
    this->clearEvents();

    //reload auto menu data (in case the AI set the location of the next map/replay)
    this->loadAutoMenuData();

    //clear everything in the server
    this->server.clearAll();

    // clear messages so they are not stored until the next match
    this->sentMessages.clear();

    // Some other variables
    apmCounter.init();
    fpsCounter.init();

    // @NOTE: Freeing libraries comes after because of some destructors for functionals in Interface Events

    // Destroy the AI Module client
    if ( this->client )
    {
      if (this->deleteClient) delete this->client;
      this->client = nullptr;
    }

    // Unload the AI Module library
    if ( hAIModule )
    {
#ifdef _WIN32
      FreeLibrary((HMODULE)hAIModule);
#else
      dlclose(hAIModule);
#endif
      hAIModule = nullptr;
    }

    this->startedClient = false;

    // Destroy the Tournament Module controller
    if ( this->tournamentController )
    {
      delete this->tournamentController;
      this->tournamentController = nullptr;
    }

    // Destroy the Tournament Module AI
    if ( this->tournamentAI )
    {
      delete this->tournamentAI;
      this->tournamentAI = nullptr;
    }

    // Destroy the Tournament Module Library
    if ( hTournamentModule )
    {
#ifdef _WIN32
      FreeLibrary((HMODULE)hTournamentModule);
#else
      dlclose(hTournamentModule);
#endif
      hTournamentModule = nullptr;
    }

    this->bTournamentMessageAppeared = false;
  }

  void GameImpl::queueSentMessage(std::string const &message)
  {
    if (!message.empty())
      this->sentMessages.emplace_back(message);
  }
};
