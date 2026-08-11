#include "PlayerImpl.h"
#include "GameImpl.h"
#include "UnitImpl.h"

#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <Util/Convenience.h>

#include <BW/BWData.h>
#include "BW/Constants.h"

#include <BWAPI/PlayerType.h>

#include "../../../Debug.h"

namespace BWAPI
{
  //--------------------------------------------- CONSTRUCTOR ------------------------------------------------
  PlayerImpl::PlayerImpl(u8 index, BW::Player bwplayer)
    : bwplayer(std::move(bwplayer)), index(index)
  {
    resetResources();
    self->color = index < 12 ? bwplayer.playerColorIndex() : 0;
  }
  //--------------------------------------------- SET ID -----------------------------------------------------
  void PlayerImpl::setID(int newID)
  {
    id = newID;
  }
  //--------------------------------------------- GET INDEX --------------------------------------------------
  int PlayerImpl::getIndex() const
  {
    return index;
  }
  //--------------------------------------------- GET NAME ---------------------------------------------------
  std::string PlayerImpl::getName() const
  {
    if ( index == 11 )
      return std::string("Neutral");
    return std::string(bwplayer.szName());
  }
  //--------------------------------------------- GET RACE ---------------------------------------------------
  BWAPI::Race PlayerImpl::getRace() const
  {
    BroodwarImpl.setLastError();
    if ( this->index < BW::PLAYABLE_PLAYER_COUNT )
    {
      Race rlast = BroodwarImpl.lastKnownRaceBeforeStart[this->index];
      if (  rlast != Races::Zerg          &&
            rlast != Races::Terran        &&
            rlast != Races::Protoss       &&
            !this->wasSeenByBWAPIPlayer   && 
            !BroodwarImpl.isFlagEnabled(Flag::CompleteMapInformation) )
      {
        BroodwarImpl.setLastError(Errors::Access_Denied);
        return Races::Unknown;
      }
    }
    return BWAPI::Race( bwplayer.nRace() );
  }
  //--------------------------------------------- GET TYPE ---------------------------------------------------
  BWAPI::PlayerType PlayerImpl::getType() const
  {
    return BWAPI::PlayerType((int)(bwplayer.nType()));
  }
  //--------------------------------------------- GET FORCE --------------------------------------------------
  Force PlayerImpl::getForce() const
  {
    return force;
  }
  //--------------------------------------------- IS ALLIES WITH ---------------------------------------------
  bool PlayerImpl::isAlly(const Player player) const
  {
    if ( !player || this->isNeutral() || player->isNeutral() || this->isObserver() || player->isObserver() )
      return false;
    return bwplayer.playerAlliances(static_cast<PlayerImpl*>(player)->getIndex()) != 0;
  }
  //--------------------------------------------- IS ALLIES WITH ---------------------------------------------
  bool PlayerImpl::isEnemy(const Player player) const
  {
    if ( !player || this->isNeutral() || player->isNeutral() || this->isObserver() || player->isObserver() )
      return false;
    return bwplayer.playerAlliances(static_cast<PlayerImpl*>(player)->getIndex()) == 0;
  }
  //--------------------------------------------- IS NEUTRAL -------------------------------------------------
  bool PlayerImpl::isNeutral() const
  {
    return index == 11;
  }
  //--------------------------------------------- GET START POSITION -----------------------------------------
  TilePosition PlayerImpl::getStartLocation() const
  {
    // Clear last error
    BroodwarImpl.setLastError();

    // Return None if there is no start location
    if (index >= BW::PLAYABLE_PLAYER_COUNT || bwplayer.startPosition() == BW::Positions::Origin)
      return TilePositions::None;

    // Return unknown and set Access_Denied if the start location
    // should not be made available.
    if ( !BroodwarImpl.isReplay() &&
       BroodwarImpl.self()->isEnemy(const_cast<PlayerImpl*>(this)) &&
       !BroodwarImpl.isFlagEnabled(Flag::CompleteMapInformation) )
    {
      BroodwarImpl.setLastError(Errors::Access_Denied);
      return TilePositions::Unknown;
    }
    // return the start location as a tile position
    return TilePosition(bwplayer.startPosition() - BW::Position((TILE_SIZE * 4) / 2, (TILE_SIZE * 3) / 2));
  }
  //--------------------------------------------- IS VICTORIOUS ----------------------------------------------
  bool PlayerImpl::isVictorious() const
  {
    if ( index >= 8 ) 
      return false;
    return bwplayer.PlayerVictory() == 3;
  }
  //--------------------------------------------- IS DEFEATED ------------------------------------------------
  bool PlayerImpl::isDefeated() const
  {
    if ( index >= 8 ) 
      return false;
    return bwplayer.PlayerVictory() == 1 ||
           bwplayer.PlayerVictory() == 2 ||
           bwplayer.PlayerVictory() == 4 ||
           bwplayer.PlayerVictory() == 6;
  }
  //--------------------------------------------- UPDATE -----------------------------------------------------
  // SB_PLAYER_MIRROR_SKIP: unset/1 = on (default), 0 = off (kill-switch), verify = recompute
  // anyway and byte-compare the outputs. Read once; std::getenv is not hot-path safe.
  bool PlayerImpl::playerMirrorSkipEnabled()
  {
    static const bool on = [] {
      const char* v = std::getenv("SB_PLAYER_MIRROR_SKIP");
      return !v || (std::strcmp(v, "0") != 0);
    }();
    return on;
  }

  bool PlayerImpl::playerMirrorVerifyMode()
  {
    static const bool verify = [] {
      const char* v = std::getenv("SB_PLAYER_MIRROR_SKIP");
      return v && std::strcmp(v, "verify") == 0;
    }();
    return verify;
  }

  // Compare only the PlayerData that updateData() can produce or consume. allUnitCount and
  // visibleUnitCount are maintained elsewhere in BWAPI and are neither read nor written here,
  // so including them made the guard sensitive to changes it does not care about AND cost
  // 1,872 B per player per frame of pointless comparison — 45 KB across 12 players x 2 viewers
  // under dual-host, where L2 capacity is the binding constraint. completedUnitCount IS read
  // (the hidden-player branch) and stays covered: the two ranges below straddle exactly the
  // two excluded arrays. Verify mode still byte-compares the WHOLE struct, so a mistake here
  // surfaces as a PLAYER-MIRROR-VERIFY-DIFF rather than as silent staleness.
  static bool playerDataOutputsUnchanged(const PlayerData* a, const PlayerData* b)
  {
    static_assert(offsetof(PlayerData, visibleUnitCount) > offsetof(PlayerData, allUnitCount),
                  "allUnitCount/visibleUnitCount must be adjacent for the skipped range");
    static_assert(offsetof(PlayerData, completedUnitCount) > offsetof(PlayerData, visibleUnitCount),
                  "completedUnitCount must follow the skipped range");
    const char* pa = reinterpret_cast<const char*>(a);
    const char* pb = reinterpret_cast<const char*>(b);
    constexpr std::size_t head = offsetof(PlayerData, allUnitCount);
    constexpr std::size_t tail = offsetof(PlayerData, completedUnitCount);
    return std::memcmp(pa, pb, head) == 0 &&
           std::memcmp(pa + tail, pb + tail, sizeof(PlayerData) - tail) == 0;
  }

  // Skip-rate telemetry: a guard that never fires must be visible rather than inferred
  // from a flat wall-clock number. Printed once per game from onGameEnd.
  long long PlayerImpl::mirrorSkipCount = 0;
  long long PlayerImpl::mirrorDormantCount = 0;
  long long PlayerImpl::mirrorRecomputeCount = 0;

  void PlayerImpl::updateData()
  {
    GameImpl& game = BroodwarImpl;  // hoist the thread_local deref (cut 3)

    // Bounds the fingerprint shares with BW::Constants.h. A mismatch would silently
    // fingerprint fewer entries than the recompute reads, which is exactly the class of
    // hole the unit-side cut had to hunt down.
    static_assert(BW::PlayerMirrorFingerprint::kRaces        == BW::RACE_COUNT,         "fingerprint race bound");
    static_assert(BW::PlayerMirrorFingerprint::kUnitTypes    == BW::UNIT_TYPE_COUNT,    "fingerprint unit-type bound");
    static_assert(BW::PlayerMirrorFingerprint::kTechTypes    == BW::TECH_TYPE_COUNT,    "fingerprint tech bound");
    static_assert(BW::PlayerMirrorFingerprint::kUpgradeTypes == BW::UPGRADE_TYPE_COUNT, "fingerprint upgrade bound");

    // The two branch decisions below, resolved once and reused: the skip must key on them,
    // because the same engine state produces different outputs on either side of them.
    // Expressions and short-circuit order are copied verbatim from the bodies they guard.
    const bool hiddenToUs = !game.isReplay() &&
                            game.self()->isEnemy(this) &&
                            !game.isFlagEnabled(Flag::CompleteMapInformation);
    const bool hideCapabilities = this->isNeutral() || index >= BW::PLAYER_COUNT || hiddenToUs;
    const bool hideScores       = hiddenToUs || index >= BW::PLAYER_COUNT;

    // Dormant-slot fast path. An empty slot has no engine-side player, so nothing can change
    // its PlayerData once the first update has zeroed it — and skipping here means neither its
    // PlayerData nor its guard is touched again, so both fall out of the working set entirely.
    // In a 1v1 that is 9 of 12 slots, and dual-host carries two sets. Verify mode deliberately
    // does NOT take this path, so a wrong dormancy call shows up as a byte diff.
    if (index < BW::PLAYER_COUNT && !playerMirrorVerifyMode())
    {
      const int ptype = bwplayer.nType();
      if (ptype == (int)PlayerTypes::Enum::None && mirrorDormantType == ptype)
      {
        ++mirrorDormantCount;
        return;
      }
    }

    // Slots with no engine-side player never touch bwplayer (its accessors are .at()-checked
    // and would throw), so they take neither the fingerprint nor the skip. They are also the
    // cheap case already — everything below is zeroed for them.
    bool verifying = false;
    PlayerData verifySnap;
    if (playerMirrorSkipEnabled() && index < BW::PLAYER_COUNT)
    {
      if (!mirrorGuard) mirrorGuard.reset(new MirrorGuard());

      BW::PlayerMirrorFingerprint now;
      bwplayer.mirrorFingerprint(&now, !hideCapabilities, !hideScores);

      MirrorExtra extra{};
      extra.repairedMinerals  = _repairedMinerals;
      extra.repairedGas       = _repairedGas;
      extra.refundedMinerals  = _refundedMinerals;
      extra.refundedGas       = _refundedGas;
      extra.hideCapabilities = hideCapabilities ? 1 : 0;
      extra.hideScores       = hideScores ? 1 : 0;
      extra.neutral          = this->isNeutral() ? 1 : 0;

      // Three conditions, and the third is the one that makes this safe: engine inputs
      // unchanged, hidden BWAPI-side inputs unchanged, AND nothing has written our own
      // outputs since we produced them (latency-compensated resources, chiefly).
      if (mirrorSnapValid &&
          std::memcmp(&now,   &mirrorGuard->snap,      sizeof(now))   == 0 &&
          std::memcmp(&extra, &mirrorGuard->extraSnap, sizeof(extra)) == 0 &&
          playerDataOutputsUnchanged(self, &mirrorGuard->outSnap))
      {
        // Skip only from the SECOND consecutive unchanged frame, matching the unit-side
        // rule: one full recompute must have run against exactly these inputs before its
        // outputs can be assumed still current.
        if (mirrorStreak >= 1)
        {
          if (playerMirrorVerifyMode())
          {
            verifying = true;
            verifySnap = *self;   // retained outputs; recompute below must reproduce them
          }
          else
          {
            // The visible branch sets this every frame; preserve that observable.
            if (!hideCapabilities) this->wasSeenByBWAPIPlayer = true;
            ++mirrorSkipCount;
            return;
          }
        }
        else
          ++mirrorStreak;
      }
      else
      {
        mirrorGuard->snap      = now;
        mirrorGuard->extraSnap = extra;
        mirrorSnapValid = true;
        mirrorStreak    = 0;
      }
    }
    else
    {
      mirrorSnapValid = false;
      mirrorStreak    = 0;
    }

    self->color = index < BW::PLAYER_COUNT ? bwplayer.playerColorIndex() : 0;

    // Get upgrades, tech, resources
    if ( hideCapabilities )
    {
      self->minerals           = 0;
      self->gas                = 0;
      self->gatheredMinerals   = 0;
      self->gatheredGas        = 0;
      self->repairedMinerals   = 0;
      self->repairedGas        = 0;
      self->refundedMinerals   = 0;
      self->refundedGas        = 0;

      // Reset values
      MemZero(self->upgradeLevel);
      MemZero(self->hasResearched);
      MemZero(self->isUpgrading);
      MemZero(self->isResearching);
    
      MemZero(self->maxUpgradeLevel);
      MemZero(self->isResearchAvailable);
      MemZero(self->isUnitAvailable);

      if (!this->isNeutral() && index < BW::PLAYER_COUNT)
      {
        // set upgrade level for visible enemy units
        for(int i = 0; i < BW::UPGRADE_TYPE_COUNT; ++i)
        {
          for(UnitType t : UpgradeType(i).whatUses())
          {
            if ( self->completedUnitCount[t] > 0 )
              self->upgradeLevel[i] = bwplayer.currentUpgradeLevel(i);
          }
        }
      }
    }
    else
    {
      this->wasSeenByBWAPIPlayer = true;

      // set resources
      self->minerals           = bwplayer.minerals();
      self->gas                = bwplayer.gas();
      self->gatheredMinerals   = bwplayer.cumulativeMinerals();
      self->gatheredGas        = bwplayer.cumulativeGas();
      self->repairedMinerals   = this->_repairedMinerals;
      self->repairedGas        = this->_repairedGas;
      self->refundedMinerals   = this->_refundedMinerals;
      self->refundedGas        = this->_refundedGas;

      // set upgrade level
      for(int i = 0; i < BW::UPGRADE_TYPE_COUNT; ++i)
      {
        self->upgradeLevel[i]     = bwplayer.currentUpgradeLevel(i);
        self->maxUpgradeLevel[i]  = bwplayer.maxUpgradeLevel(i);
      }

      // set abilities researched
      for(int i = 0; i < BW::TECH_TYPE_COUNT; ++i)
      {
        self->hasResearched[i]        = TechType(i).whatResearches() == UnitTypes::None ? true : bwplayer.techResearched(i);
        self->isResearchAvailable[i]  = bwplayer.techAvailable(i);
      }

      // set upgrades in progress
      for (int i = 0; i < BW::UPGRADE_TYPE_COUNT; ++i)
        self->isUpgrading[i]   = bwplayer.upgradeInProgress(i);
      
      // set research in progress
      for (int i = 0; i < BW::TECH_TYPE_COUNT; ++i)
        self->isResearching[i] = bwplayer.techResearchInProgress(i);

      for (int i = 0; i < BW::UNIT_TYPE_COUNT; ++i)
        self->isUnitAvailable[i] = bwplayer.unitAvailability(i);

      self->hasResearched[TechTypes::Enum::Nuclear_Strike] = self->isUnitAvailable[UnitTypes::Enum::Terran_Nuclear_Missile];
    }

    // Get Scores, supply
    if ( hideScores )
    {
      MemZero(self->supplyTotal);
      MemZero(self->supplyUsed);
      MemZero(self->deadUnitCount);
      MemZero(self->killedUnitCount);

      self->totalUnitScore      = 0;
      self->totalKillScore      = 0;
      self->totalBuildingScore  = 0;
      self->totalRazingScore    = 0;
      self->customScore         = 0;
    }
    else
    {
      // set supply
      for (u8 i = 0; i < BW::RACE_COUNT; ++i)
      {
        self->supplyTotal[i]  = bwplayer.suppliesAvailable(i);
        if (self->supplyTotal[i] > bwplayer.suppliesMax(i))
          self->supplyTotal[i]  = bwplayer.suppliesMax(i);
        self->supplyUsed[i]   = bwplayer.suppliesUsed(i);
      }
      // set total unit counts
      for (int i = 0; i < BW::UNIT_TYPE_COUNT; ++i)
      {
        self->deadUnitCount[i]   = bwplayer.unitCountsDead(i);
        self->killedUnitCount[i] = bwplayer.unitCountsKilled(i);
      }
      // set macro dead unit counts
      self->deadUnitCount[UnitTypes::AllUnits]    = bwplayer.allUnitsLost() + bwplayer.allBuildingsLost();
      self->deadUnitCount[UnitTypes::Men]         = bwplayer.allUnitsLost();
      self->deadUnitCount[UnitTypes::Buildings]   = bwplayer.allBuildingsLost();
      self->deadUnitCount[UnitTypes::Factories]   = bwplayer.allFactoriesLost();

      // set macro kill unit counts
      self->killedUnitCount[UnitTypes::AllUnits]  = bwplayer.allUnitsKilled() + bwplayer.allBuildingsRazed();
      self->killedUnitCount[UnitTypes::Men]       = bwplayer.allUnitsKilled();
      self->killedUnitCount[UnitTypes::Buildings] = bwplayer.allBuildingsRazed();
      self->killedUnitCount[UnitTypes::Factories] = bwplayer.allFactoriesRazed();
      
      // set score counts
      self->totalUnitScore      = bwplayer.allUnitScore();
      self->totalKillScore      = bwplayer.allKillScore();
      self->totalBuildingScore  = bwplayer.allBuildingScore();
      self->totalRazingScore    = bwplayer.allRazingScore();
      self->customScore         = bwplayer.customScore();
    }

    if (index < BW::PLAYER_COUNT && (bwplayer.nType() == PlayerTypes::PlayerLeft ||
        bwplayer.nType() == PlayerTypes::ComputerLeft ||
       (bwplayer.nType() == PlayerTypes::Neutral && !isNeutral())))
    {
      self->leftGame = true;
    }

    // verify mode: the recompute just ran on a frame the fingerprint said was skippable, so
    // its outputs MUST equal the retained ones. Any difference IS a fingerprint hole — an
    // input updateData() reads that the fingerprint (or MirrorExtra) does not cover. Printed
    // byte-exact so the missing field can be identified from the PlayerData offset.
    ++mirrorRecomputeCount;

    // Remember exactly what this recompute produced, so the next frame can tell whether
    // anyone else has written it since (see mirrorOutSnap in PlayerImpl.h).
    if (playerMirrorSkipEnabled() && index < BW::PLAYER_COUNT && mirrorGuard)
      mirrorGuard->outSnap = *self;

    // Record dormancy only after a full recompute has settled this slot's outputs.
    mirrorDormantType = (index < BW::PLAYER_COUNT && bwplayer.nType() == PlayerTypes::Enum::None)
                          ? (int)PlayerTypes::Enum::None : -1;

    if (verifying && std::memcmp(&verifySnap, self, sizeof(PlayerData)) != 0)
    {
      const char* a = reinterpret_cast<const char*>(&verifySnap);
      const char* b = reinterpret_cast<const char*>(self);
      for (std::size_t i = 0; i != sizeof(PlayerData); ++i)
      {
        if (a[i] != b[i])
        {
          std::printf("PLAYER-MIRROR-VERIFY-DIFF f=%d player=%d offset=%zu retained=%d fresh=%d\n",
                      game.getFrameCount(), index, i,
                      (int)(unsigned char)a[i], (int)(unsigned char)b[i]);
          break;
        }
      }
    }
  }
  //----------------------------------------------------------------------------------------------------------
  void PlayerImpl::onGameEnd()
  {
    if (mirrorSkipCount + mirrorRecomputeCount + mirrorDormantCount > 0)
    {
      const long long total = mirrorSkipCount + mirrorRecomputeCount;
      std::printf("PLAYERMIRROR skipped=%lld dormant=%lld recomputed=%lld rate=%.1f%%\n",
                  mirrorSkipCount, mirrorDormantCount, mirrorRecomputeCount,
                  100.0 * mirrorSkipCount / total);
      mirrorSkipCount = 0;
      mirrorDormantCount = 0;
      mirrorRecomputeCount = 0;
    }
    this->units.clear();
    this->clientInfo.clear();
    this->interfaceEvents.clear();

    self->leftGame = false;
    this->wasSeenByBWAPIPlayer = false;
  }
  void PlayerImpl::setParticipating(bool isParticipating)
  {
    self->isParticipating = isParticipating;
  }
  void PlayerImpl::resetResources()
  {
    _repairedMinerals = 0;
    _repairedGas      = 0;
    _refundedMinerals = 0;
    _refundedGas      = 0;
  }
  void PlayerImpl::setRace(Race race)
  {
    if (BroodwarImpl.isInGame()) return;
    bwplayer.setRace((int)race);
  }
  void PlayerImpl::closeSlot()
  {
    if (BroodwarImpl.isInGame()) return;
    bwplayer.closeSlot();
  }
  void PlayerImpl::openSlot()
  {
    if (BroodwarImpl.isInGame()) return;
    bwplayer.openSlot();
  }
  void PlayerImpl::setUpgradeLevel(UpgradeType upgrade, int level)
  {
    bwplayer.setUpgradeLevel((int)upgrade, level);
  }

  void PlayerImpl::setResearched(TechType tech, bool researched)
  {
    bwplayer.setResearched((int)tech, researched);
  }

  void PlayerImpl::setMinerals(int value)
  {
    bwplayer.setMinerals(value);
  }

  void PlayerImpl::setGas(int value)
  {
    bwplayer.setGas(value);
  }
}
