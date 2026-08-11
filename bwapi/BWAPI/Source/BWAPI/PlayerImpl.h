#pragma once
#include <Util/Types.h>
#include <string>
#include <memory>

#include <BWAPI/Player.h>
#include <BWAPI/Client/PlayerData.h>
#include <BWAPI/Unitset.h>

#include "UnitImpl.h"
#include "ForceImpl.h"

#include "BW/BWData.h"

#ifdef COMPAT
#include "CompatGameImpl.h"
#endif

namespace BWAPI
{
  // Forwards
  class ForceInterface;
  typedef ForceInterface* Force;
  class UnitType;
  class UpgradeType;
  class TechType;
  class PlayerType;
  class Race;

  /** Represents one player in game. Note that there is always player 12 who owns resources. */
  class PlayerImpl : public PlayerInterface
  {
    public:
      virtual int         getID() const override;
      virtual std::string getName() const override;
      virtual Race        getRace() const override;
      virtual PlayerType  getType() const override;
      virtual Force       getForce() const override;

      virtual bool isAlly(const Player player) const override;
      virtual bool isEnemy(const Player player) const override;
      virtual bool isNeutral() const override;

      virtual bool isVictorious() const override;
      virtual bool isDefeated() const override;
      virtual bool leftGame() const override;

      virtual const Unitset &getUnits() const override;
      virtual TilePosition  getStartLocation() const override;

      virtual int  minerals() const override;
      virtual int  gas() const override;

      virtual int  gatheredMinerals() const override;
      virtual int  gatheredGas() const override;
      virtual int  repairedMinerals() const override;
      virtual int  repairedGas() const override;
      virtual int  refundedMinerals() const override;
      virtual int  refundedGas() const override;
      virtual int  spentMinerals() const override;
      virtual int  spentGas() const override;

      virtual int  supplyTotal(Race race = Races::None) const override;
      virtual int  supplyUsed(Race race = Races::None) const override;

      virtual int  allUnitCount(UnitType unit) const override;
      virtual int  visibleUnitCount(UnitType unit) const override;
      virtual int  completedUnitCount(UnitType unit) const override;
      virtual int  deadUnitCount(UnitType unit) const override;
      virtual int  killedUnitCount(UnitType unit) const override;

      virtual int  getUpgradeLevel(UpgradeType upgrade) const override;
      virtual bool hasResearched(TechType tech) const override;
      virtual bool isResearching(TechType tech) const override;
      virtual bool isUpgrading(UpgradeType upgrade) const override;

      virtual BWAPI::Color getColor() const override;

      virtual int getUnitScore() const override;
      virtual int getKillScore() const override;
      virtual int getBuildingScore() const override;
      virtual int getRazingScore() const override;
      virtual int getCustomScore() const override;

      virtual bool isObserver() const override;

      virtual int  getMaxUpgradeLevel(UpgradeType upgrade) const override;
      virtual bool isResearchAvailable(TechType tech) const override;
      virtual bool isUnitAvailable(UnitType unit) const override;

      virtual void setRace(Race race) override;
      virtual void closeSlot() override;
      virtual void openSlot() override;

      virtual void setUpgradeLevel(UpgradeType upgrade, int level) override;
      virtual void setResearched(TechType tech, bool researched) override;
      virtual void setMinerals(int value) override;
      virtual void setGas(int value) override;

      //Internal BWAPI commands:
      /**
       * Constructs new player
       * @param id 0-based Index of the player (11 for resources as it is player 12)
       */
      PlayerImpl(u8 index, BW::Player bwplayer);
      int getIndex() const;      // Gets 0-based index of the player. (11 for neutral)


      void setID(int newID);

      /**
       * Updates localData according to bw values. (Should be called every frame before local data updates are
       * performed
       */
      void updateData();
      void onGameEnd();

      void setParticipating(bool isParticipating = true);
      void resetResources();
      
    // data members
      BW::Player bwplayer;
      
      ForceImpl*  force = nullptr;
      PlayerData  data = PlayerData();
      PlayerData* self = &data;
      Unitset     units;

      s32 _repairedMinerals;
      s32 _repairedGas;
      s32 _refundedMinerals;
      s32 _refundedGas;

      bool wasSeenByBWAPIPlayer = false;

      // ---- player mirror skip (sb-perf) --------------------------------------------------
      // updateData() rebuilds this player's whole capability table every frame from ~700
      // engine accessor crossings, for data that changes a handful of times per game. The
      // engine-side fingerprint (BW::PlayerMirrorFingerprint) collapses those crossings to
      // one; when it AND the BWAPI-side inputs the engine cannot see are byte-unchanged, the
      // recompute is skipped. Same structure as the unit-side cut in UnitUpdate.cpp.
      //
      // MirrorExtra carries the INPUTS updateData() reads that are not engine state: the
      // repaired/refunded accumulators, which live on this object and are written by BWAPI
      // event handlers. Copied verbatim, never hashed — a hash would make rule 9
      // probabilistic.
      struct MirrorExtra {
        s32  repairedMinerals, repairedGas, refundedMinerals, refundedGas;
        u8   hideCapabilities;   // resolved branch decision, block A
        u8   hideScores;         // resolved branch decision, block B
        u8   neutral;
        u8   pad;
      };

      // mirrorOutSnap is the OUTPUT side of the guard, and it is what makes this safe against
      // writers we have not enumerated. BWAPI applies latency compensation to player
      // resources: issuing a build/train/research command immediately deducts the cost from
      // self->minerals (CommandTemp.h), and cancelling refunds it — writes the engine-side
      // fingerprint cannot see, exactly like the unit-side prediction hole. Rather than list
      // the fields CommandTemp.h touches and hope the list stays current, we remember the
      // exact PlayerData the last recompute produced and refuse to skip if anything has
      // changed it since. That covers every external writer by construction, present and
      // future. (Found by SB_PLAYER_MIRROR_SKIP=verify, which reported 22 diffs at
      // PlayerData offset 80 = minerals before this guard existed.)
      // The guard's snapshots are ~6.8 KB per player and were inline members, so all 12 slots
      // carried them whether or not anyone was in them. Held behind a pointer and allocated on
      // first non-dormant update instead: in a 1v1 only 2 slots plus neutral ever occupy a slot,
      // so 9 of 12 never allocate it at all. Dual-host holds two sets of these, and L2 capacity
      // is the binding constraint there (benchmarks/ENGINE_VS_ENGINE.md).
      struct MirrorGuard {
        BW::PlayerMirrorFingerprint snap{};
        MirrorExtra extraSnap{};
        PlayerData  outSnap{};
      };
      std::unique_ptr<MirrorGuard> mirrorGuard;
      bool mirrorSnapValid = false;
      int  mirrorStreak = 0;

      // Dormant-slot fast path. An unoccupied slot (nType None) has no engine-side player state
      // that anything can change, so once one settling recompute has zeroed its PlayerData it
      // needs neither the recompute nor the guard — and, more to the point, neither its 5.8 KB
      // of PlayerData nor its 6.8 KB of guard is touched again, so both leave the working set.
      // Keyed on the slot being EMPTY rather than on a hardcoded player count: the map pool has
      // 4-player maps, and this stays correct for any occupancy, including a player leaving
      // (nType flips to PlayerLeft and the slot wakes up).
      int mirrorDormantType = -1;   // nType() at the last settle; -1 = not settled dormant

      // SB_PLAYER_MIRROR_SKIP: unset/1 = on (default), 0 = off (kill-switch),
      // verify = recompute anyway and byte-compare the outputs (the microscope).
      static long long mirrorSkipCount;
      static long long mirrorDormantCount;
      static long long mirrorRecomputeCount;

      static bool playerMirrorSkipEnabled();
      static bool playerMirrorVerifyMode();

#ifdef COMPAT
      CompatPlayerImpl compatPlayerImpl{this};
#endif
	
    private :
      int id = -1;
      u8 index;  /**< Order of the player, is used to load player's information from the memory */
  };
};
