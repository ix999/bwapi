#pragma once

#include "BW/Position.h"
#include "Util/Types.h"

#include <cstdint>
#include <memory>
#include <functional>
#include <iterator>
#include <type_traits>
#include <vector>

namespace bwgame {
  struct unit_t;
  struct bullet_t;
}

namespace BW {


struct Player;
struct Unit;
struct Bullet;
struct Game;
struct Region;

struct openbwapi_impl;

struct GameOwner_impl;

void sacrificeThreadForUI(std::function<void()> f);

// Dual-host: bind the calling thread to a viewer for command attribution (default 0).
void set_thread_viewer(int viewer);

struct GameOwner {
  std::unique_ptr<GameOwner_impl> impl;
  GameOwner();
  ~GameOwner();

  Game getGame();
  Game getGame(int viewer);
  void setPrintTextCallback(std::function<void(const char*)> func);
};

struct activeTile {
  u8 bVisibilityFlags = 0;
  u8 bExploredFlags = 0;
  bool bTemporaryCreep = false;
  bool bCurrentlyOccupied = false;
  bool bHasCreep = false;
};

struct UnitFinderEntry {
  void* ptr;
  int unitIndex() const;
  int searchValue() const;
};

template<typename T>
struct ValuePointer {
  T value;
  T* operator->() {
    return &value;
  }
  T operator*() {
    return value;
  }
};

struct UnitFinderIterator {
  void* ptr;

  using value_type = UnitFinderEntry;
  using pointer = ValuePointer<value_type>;
  using reference = value_type;
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::random_access_iterator_tag;

  using It = UnitFinderIterator;

  value_type operator*() const;
  It& operator++();
  bool operator==(It n) const;

  bool operator!=(It n)const ;
  pointer operator->() const;
  It operator++(int);

  It& operator--();
  It operator--(int);

  It& operator+=(difference_type n);
  It operator+(difference_type n) const;
  It& operator-=(difference_type n);
  It operator-(difference_type n) const;
  difference_type operator-(It n) const;
  value_type operator[](difference_type n) const;
  bool operator<(It n) const;
  bool operator>(It n) const;
  bool operator<=(It n) const;
  bool operator>=(It n) const;
};

template<size_t size, size_t alignment>
struct some_object {
  typename std::aligned_storage<size, alignment>::type obj;
  template<typename T, typename... args_T>
  void construct(args_T&&... args) {
    static_assert(sizeof(T) <= size || alignof(T) <= alignment, "some_object size or alignment too small");
    new ((T*)&obj) T(std::forward<args_T>(args)...);
  }
  template<typename T>
  void destroy() {
    as<T>().~T();
  }
  template<typename T>
  T& as() {
    static_assert(sizeof(T) <= size || alignof(T) <= alignment, "some_object size or alignment too small");
    return (T&)obj;
  }
  template<typename T>
  const T& as() const {
    static_assert(sizeof(T) <= size || alignof(T) <= alignment, "some_object size or alignment too small");
    return (const T&)obj;
  }
};

template<typename T>
struct DefaultIterator {
  some_object<sizeof(void*), alignof(void*)> obj;
  openbwapi_impl* impl = nullptr;

  using value_type = T;
  using pointer = ValuePointer<value_type>;
  using reference = value_type;
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::forward_iterator_tag;

  using It = DefaultIterator;

  DefaultIterator();
  ~DefaultIterator();

  value_type operator*() const;
  It& operator++();
  bool operator==(const It& n) const;

  bool operator!=(const It& n)const ;
  pointer operator->() const;
  It operator++(int);
};

using UnitIterator = DefaultIterator<Unit>;
using BulletIterator = DefaultIterator<Bullet>;

struct snapshot_impl;

struct Snapshot {
  Snapshot();
  Snapshot(Snapshot&&);
  ~Snapshot();
  Snapshot& operator=(Snapshot&&);
  std::unique_ptr<snapshot_impl> impl;
};

struct Game {
  openbwapi_impl* impl = nullptr;
  // Dual-host: which in-process viewer (bot) this handle represents; single mode uses 0.
  int viewer_index = 0;

  void overrideEnvVar(std::string var, std::string value);

  int g_LocalHumanID() const;

  Player getPlayer(int n) const;

  int gameType() const;
  int Latency() const;
  int ReplayHead_frameCount() const;
  int MouseX() const;
  int MouseY() const;
  int ScreenX() const;
  int ScreenY() const;
  int screenWidth() const;
  int screenHeight() const;
  void setScreenPosition(int x, int y);
  int NetMode() const;
  bool isGamePaused() const;
  bool InReplay() const;
  int getInstanceNumber() const;
  bool getScenarioChk(std::vector<char>& data) const;
  bool gameOver() const;
  void printText(const char* str) const;
  void nextFrame();
  void setGUI(bool enabled);
  void enableCheats() const;
  void saveReplay(const std::string& filename);
  std::tuple<int, int, void*> GameScreenBuffer();
  void setOnDraw(std::function<void(uint8_t*, size_t)> onDraw);
  std::tuple<int, int, uint32_t*> drawGameScreen(int x, int y, int width, int height);

  template<typename T, typename... args_T>
  void QueueCommand(args_T&&... args) {
    T buf(std::forward<args_T>(args)...);
    QueueCommand(&buf, sizeof(T));
  }
  void QueueCommand(const void* buf, size_t size);

  void leaveGame();
  bool gameClosed() const;

  u32 ReplayVision() const;
  void setReplayVision(u32);
  void setGameSpeedModifiers(int n, int value);
  void setAltSpeedModifiers(int n, int value);
  void setFrameSkip(int value);
  int getLatencyFrames() const;
  int GameSpeed() const;
  int gameSpeedModifiers(int speed) const;
  int lastTurnFrame() const;
  bool setMapFileName(const std::string& filename);
  int elapsedTime() const;
  int countdownTimer() const;
  void setReplayRevealAll(bool);
  u32 ReplayHead_gameSeed_randSeed() const;

  int mapTileSize_x() const;
  int mapTileSize_y() const;
  const char* mapFileName() const;
  const char* mapTitle() const;
  activeTile getActiveTile(int tile_x, int tile_y) const;
  bool buildable(int tile_x, int tile_y) const;
  bool walkable(int walk_x, int walk_y) const;
  bool hasCreep(int tile_x, int tile_y) const;
  bool isOccupied(int tile_x, int tile_y) const;
  int groundHeight(int tile_x, int tile_y) const;
  u8 bVisibilityFlags(int tile_x, int tile_y) const;
  u8 bExploredFlags(int tile_x, int tile_y) const;

  Unit getUnit(size_t index) const;
  Bullet getBullet(size_t index) const;

  bool triggersCanAllowGameplayForPlayer(int player) const;
  std::array<int, 12> bRaceInfo() const;
  std::array<int, 12> bOwnerInfo() const;
  const char* forceNames(size_t n) const;

  BulletIterator BulletNodeTable_begin() const;
  BulletIterator BulletNodeTable_end() const;
  UnitIterator UnitNodeList_VisibleUnit_begin() const;
  UnitIterator UnitNodeList_VisibleUnit_end() const;
  UnitIterator UnitNodeList_HiddenUnit_begin() const;
  UnitIterator UnitNodeList_HiddenUnit_end() const;
  UnitIterator UnitNodeList_ScannerSweep_begin() const;
  UnitIterator UnitNodeList_ScannerSweep_end() const;

  void setCharacterName(const std::string& name);
  void setGameTypeMelee();
  void setGameTypeUseMapSettings();
  void createSinglePlayerGame(std::function<void()> setupFunction);
  void createMultiPlayerGame(std::function<void()> setupFunction);
  // Dual-host: one state, two in-process local clients (ENGINE_OPT_DUALHOST.md).
  void createDualPlayerGame(std::function<void()> setupFunction);
  void setDualSecondaryRace(int race);
  int dualSecondarySlot() const;
  int dualPickedRace(int slot) const;
  bool dualSecondaryUidSortsFirst() const;
  void dualLocalOccupySlot(int n);
  void dualSecondaryOccupySlot(int n);
  void startGame();
  void switchToSlot(int n);
  int connectedPlayerCount();

  UnitFinderIterator UnitOrderingX() const;
  UnitFinderIterator UnitOrderingY() const;
  size_t UnitOrderingCount() const;

  size_t regionCount() const;
  Region getRegion(size_t index) const;
  Region getRegionAt(int x, int y) const;

  Unit createUnit(int owner, int unitType, int x, int y);
  void killUnit(Unit u);
  void removeUnit(Unit u);

  Snapshot saveSnapshot();
  void loadSnapshot(const Snapshot&);
  void setRandomSeed(uint32_t value);
  void disableTriggers();

  void sendCustomAction(const void* data, size_t size);
  void setCustomActionCallback(std::function<void(int player, const char* data, size_t size)> callback);
};

struct Player {
  int owner = -1;
  openbwapi_impl* impl = nullptr;

  int playerColorIndex() const;
  const char* szName() const;
  int nRace() const;
  int pickedRace() const;
  int nType() const;
  int nTeam() const;
  int playerAlliances(int n) const;
  Position startPosition() const;
  int PlayerVictory() const;
  int currentUpgradeLevel(int n) const;
  int maxUpgradeLevel(int n) const;
  bool techResearched(int n) const;
  bool techAvailable(int n) const;
  bool upgradeInProgress(int n) const;
  bool techResearchInProgress(int n) const;
  bool unitAvailability(int n) const;
  int minerals() const;
  int gas() const;
  int cumulativeMinerals() const;
  int cumulativeGas() const;
  int suppliesAvailable(int n) const;
  int suppliesMax(int n) const;
  int suppliesUsed(int n) const;
  int unitCountsDead(int n) const;
  int unitCountsKilled(int n) const;
  int unitCountsAll(int n) const;
  int allUnitsLost() const;
  int allBuildingsLost() const;
  int allFactoriesLost() const;
  int allUnitsKilled() const;
  int allBuildingsRazed() const;
  int allFactoriesRazed() const;
  int allUnitScore() const;
  int allKillScore() const;
  int allBuildingScore() const;
  int allRazingScore() const;
  int customScore() const;
  u32 playerVision() const;
  int downloadStatus() const;

  void setRace(int race);
  void closeSlot();
  void openSlot();

  void setUpgradeLevel(int upgrade, int level);
  void setResearched(int tech, bool researched);
  void setMinerals(int value);
  void setGas(int value);

  void mirrorFingerprint(struct PlayerMirrorFingerprint* dst, bool needCapabilities, bool needScores) const;
};

// Player mirror fingerprint (sb-perf player mirror skip): the exact engine-side inputs
// consumed by PlayerImpl::updateData(). That function rebuilds a player's whole capability
// table every frame — 61 upgrades x3, 44 techs x3, 228 unit-availability and 228x2 unit
// counts — i.e. ~700 accessor crossings per player per frame, x12 player slots, for data
// that changes a handful of times per game. One fill call replaces all of them; byte-equal
// fingerprints across consecutive frames prove the outputs identical, so the recompute can
// be skipped (guard logic lives BWAPI-side, as for the unit fingerprint above).
//
// The fill calls the very same Player:: accessors the mirror calls, from inside this
// library, so the fingerprint reads exactly what the recompute would read BY CONSTRUCTION
// — there is no second implementation to drift. The win is purely call-count across the
// library boundary, which is the same win the unit-side cut measured.
//
// Array bounds mirror BW::Constants.h and are static_asserted against it BWAPI-side.
// Trivially copyable, zero-filled before fill — memcmp-comparable including padding.
struct PlayerMirrorFingerprint {
  enum { kRaces = 3, kUnitTypes = 228, kTechTypes = 44, kUpgradeTypes = 61 };

  s32 color, race, type;
  s32 minerals, gas, cumulative_minerals, cumulative_gas;
  s32 supplies_available[kRaces], supplies_max[kRaces], supplies_used[kRaces];
  s32 upgrade_level[kUpgradeTypes];
  s32 max_upgrade_level[kUpgradeTypes];
  u8  upgrade_in_progress[kUpgradeTypes];
  u8  tech_researched[kTechTypes];
  u8  tech_available[kTechTypes];
  u8  tech_in_progress[kTechTypes];
  u8  unit_available[kUnitTypes];
  // unitCountsDead/Killed are `return 0` in this engine (BWData.cpp, marked fixme) and are
  // index-independent, so one probe each covers the whole array. Keeping the full 228-entry
  // arrays cost 1,816 B per player of fingerprint that is always zero — 42 KB of per-frame
  // memcmp traffic across 12 players x 2 viewers under dual-host, where L2 is the binding
  // constraint. If they are ever implemented per type, SB_PLAYER_MIRROR_SKIP=verify reports it
  // immediately as a deadUnitCount/killedUnitCount diff.
  s32 units_dead_probe;
  s32 units_killed_probe;
  s32 all_units_lost, all_buildings_lost, all_factories_lost;
  s32 all_units_killed, all_buildings_razed, all_factories_razed;
  s32 unit_score, kill_score, building_score, razing_score, custom_score;
};

// Mirror fingerprint (sb-perf mirror cut 2): the exact engine-side inputs consumed by
// UnitImpl::updateInternalData()/updateData(). One fill call replaces the ~100 accessor
// crossings those functions make; byte-equal fingerprints across consecutive frames prove
// their outputs identical, so the recompute can be skipped (guard logic lives BWAPI-side).
// raw_blocks carries verbatim copies of the type-specific union and building blocks, plus
// the single live worker field (worker.powerup), so every union/building-derived read is
// covered without per-type field logic. The rest of the 64-byte worker block is omitted:
// worker.powerup is the ONLY worker-block field the mirror boundary reads (Unit::getPowerUp),
// so the other 56 bytes can never make the mirror stale. Trivially copyable, zero-filled
// before fill — memcmp-comparable including padding.
struct MirrorFingerprint {
  // Width-narrowed with CHECKED conversions (see fp_u8/fp_u16/fp_s16 in BWData.cpp). The engine
  // declares every one of these `int`, so the narrower width is justified per field from what can
  // actually be assigned — never from what a sample game happened to contain. A silent truncation
  // would produce an EQUAL fingerprint for a CHANGED unit, i.e. a stale mirror, so every narrowed
  // field is range-checked at fill time and fails loudly instead. Bounds are recorded in
  // docs/design/ENGINE_OPT_MIRROR.md; two look like u8 candidates and are NOT:
  //   remove_timer         up to 1800  (Broodling 1800, hallucination 1350, callers 900/360/90)
  //   remaining_build_time up to 65535 (assigned 0xffff directly)
  //
  // Layout is packing-ordered (8, then 4, then 2, then 1 byte) so there is no interior padding:
  // this struct is memcmp'd every frame for every unit in every viewer, and that compare is 38.7%
  // of all last-level read misses under dual-host, so its WIDTH is the cost that matters.
  //
  // kRawBlocks is the EXACT sum of the covered blocks (union 48 + worker.powerup 8 + building
  // 104); BWData.cpp static_asserts equality in both directions. The full worker struct is 64,
  // but only its 8-byte powerup pointer is ever read, so 56 dead bytes are dropped here.
  enum { kRawBlocks = 160 };

  u64 sprite, main_image, unit_type, order_type, sec_order_type;
  u64 mt_unit, ot_unit, subunit, connected_unit, current_build_unit;
  u64 sub_sprite, sub_main_image;
  u64 build_queue[5];

  // 32-bit by necessity: wide bitfields and fp8 fixed-point values.
  s32 status_flags;        // ~28-bit mask
  u32 detected_flags;      // full 0xFFFFFFFF
  s32 hp_raw, shield_raw, energy_raw, dm_hp_raw;
  s32 kill_count;          // only `= 0` and `+= target->kill_count`, no clamp anywhere: unbounded

  u16 pos_x, pos_y, mt_x, mt_y, ot_x, ot_y;   // map max 256 tiles x 32 px = 8192
  u16 remove_timer;                            // <= 1800
  u16 remaining_build_time;                    // <= 65535, no headroom
  u16 sprite_visibility, image_frameset, sub_frameset;
  s16 vel_x, vel_y;                            // fp8 velocity

  u8 owner;                                    // 0-11
  u8 main_order_timer;                         // <= 93
  u8 ground_cd, air_cd, sub_ground_cd, sub_air_cd;  // clamped [5,250] + rand[-1,+2] => <= 252
  u8 spell_cd;
  u8 carrying_flags, movement_state, movement_flags;
  u8 acid_spore_count;                         // <= 9 (std::array<int,9> acid_spore_time)
  u8 parasite_flags, blinded_by;
  u8 storm_timer, dm_timer, stim_timer, ensnare_timer;
  u8 lockdown_timer, irradiate_timer, stasis_timer, plague_timer, maelstrom_timer;
  u8 image_anim, sub_anim;
  s8 heading;                                  // direction_t is 8-bit
  u8 build_queue_size;                         // fixed-capacity 5 container
  u8 sub_present;

  u8 raw_blocks[kRawBlocks];
};

struct Unit {
  bwgame::unit_t* u = nullptr;
  openbwapi_impl* impl = nullptr;

  explicit operator bool() const;

  size_t getIndex() const;
  u16 getUnitID() const;

  Position position() const;
  bool hasSprite() const;
  int visibilityFlags() const;
  int unitType() const;
  bool statusFlag(int flag) const;
  u32 visibilityStatus() const;
  bool movementFlag(int flag) const;
  int orderID() const;
  int groundWeaponCooldown() const;
  int airWeaponCooldown() const;
  int playerID() const;
  size_t buildQueueSlot() const;
  int buildQueue(size_t index) const;
  bool fighter_inHanger() const;
  Unit fighter_parent() const;
  Unit connectedUnit() const;
  int hitPoints() const;
  int resourceCount() const;
  u8 currentDirection1() const;
  int current_speed_x() const;
  int current_speed_y() const;
  Unit subUnit() const;
  int mainOrderTimer() const;
  int spellCooldown() const;
  bool hasSprite_pImagePrimary() const;
  int sprite_pImagePrimary_anim() const;
  Unit orderTarget_pUnit() const;
  int sprite_pImagePrimary_frameSet() const;
  int shieldPoints() const;
  int energy() const;
  int killCount() const;
  int acidSporeCount() const;
  int defenseMatrixDamage() const;
  int defenseMatrixTimer() const;
  int ensnareTimer() const;
  int irradiateTimer() const;
  int lockdownTimer() const;
  int maelstromTimer() const;
  int plagueTimer() const;
  int removeTimer() const;
  int stasisTimer() const;
  int stimTimer() const;
  int secondaryOrderID() const;
  Unit currentBuildUnit() const;
  Unit moveTarget_pUnit() const;
  Position moveTarget() const;
  Position orderTarget() const;
  Unit building_addon() const;
  Unit nydus_exit() const;
  Unit worker_pPowerup() const;
  int gatherQueueCount() const;
  Unit nextGatherer() const;
  int isBlind() const;
  void mirrorFingerprint(MirrorFingerprint* dst) const;
  int resourceType() const;
  int parasiteFlags() const;
  int stormTimer() const;
  int movementState() const;
  int carrier_inHangerCount() const;
  int carrier_outHangerCount() const;
  int vulture_spiderMineCount() const;
  int remainingBuildTime() const;
  bool silo_bReady() const;
  int building_larvaTimer() const;
  int orderQueueTimer() const;
  int building_techType() const;
  int building_upgradeResearchTime() const;
  int building_upgradeType() const;
  Position rally_position() const;
  Unit rally_unit() const;
  int orderState() const;

  void setHitPoints(int value);
  void setShields(int value);
  void setEnergy(int value);
};

struct Bullet {
  bwgame::bullet_t* b = nullptr;
  openbwapi_impl* impl = nullptr;

  explicit operator bool() const;

  size_t getIndex() const;
  bool hasSprite() const;
  Position spritePosition() const;
  Position position() const;
  Unit sourceUnit() const;
  Unit attackTargetUnit() const;
  int type() const;
  int currentDirection() const;
  int current_speed_x() const;
  int current_speed_y() const;
  Position targetPosition() const;
  int time_remaining() const;
};

struct Region {
  size_t index = (size_t)-1;
  openbwapi_impl* impl = nullptr;

  explicit operator bool() const;
  size_t getIndex() const;
  size_t groupIndex() const;
  Position getCenter() const;
  int accessabilityFlags() const;
  int rgnBox_left() const;
  int rgnBox_right() const;
  int rgnBox_top() const;
  int rgnBox_bottom() const;
  size_t neighborCount() const;
  Region getNeighbor(size_t n) const;
};

}
