#pragma once
#include <cstddef>

namespace BWAPI
{
  /// Per-unit mirror record.
  ///
  /// FIELD ORDER IS A PERFORMANCE CONTRACT (sb-perf). The declaration order below is not
  /// cosmetic and is enforced by static_asserts at the bottom of this file. Read
  /// docs/design/ENGINE_OPT_UNITDATA.md before reordering anything.
  ///
  /// Why: this struct is 336 B -- 5.25 cache lines -- and a bot reaches it through the
  /// accessors many millions of times per game. Callgrind call counts over a 2,500-frame
  /// game put the traffic overwhelmingly in seven fields:
  ///
  ///     getID       24.9 M     getType     21.4 M     getPosition  15.1 M
  ///     exists      14.9 M     isVisible    9.8 M     isCompleted   5.2 M
  ///     getLeft/Top/Right/Bottom 13.9 M combined (position + type)
  ///
  /// In upstream's order those live on TWO cache lines 260 bytes apart -- id/type/position
  /// on line 0, exists/isCompleted/isVisible stranded on line 4 -- so touching a unit at all
  /// costs two lines. Worse, 24 B of line 0 (37% of it) went to `angle`/`velocityX`/
  /// `velocityY`, which are read far less often, and the first 4 B went to `clearanceLevel`,
  /// which has NO readers anywhere in the tree.
  ///
  /// The hot set is only ~40 B. Packed together it fits in ONE line, so the whole per-frame
  /// working set of a unit is one fetch instead of two. That is the entire change: no field
  /// added, none removed, none renamed, sizes and types identical -- only the order.
  ///
  /// COMPATIBILITY: in-process (DLL) bots reach every one of these through Game/Unit/Player
  /// accessors and are completely unaffected -- no recompile needed. The one exposure is the
  /// OUT-OF-PROCESS Client API, for which this struct is the shared-memory layout: a client
  /// built against the old header would read wrong offsets. That is why `GameData::revision`
  /// is now VALIDATED at connect (BWAPIClient/Source/Client.cpp) instead of merely reported --
  /// an out-of-date client is refused with an explanatory message rather than silently
  /// misreading unit state.
  struct UnitData
  {
    // ---------------------------------------------------------------------------------
    // CACHE LINE 0 (bytes 0-63): the per-frame hot set. Ordered by measured call count.
    // Everything a bot touches when it merely looks at a unit lives here.
    // ---------------------------------------------------------------------------------
    int  id;                  //   0   24.9 M
    int  player;              //   4   (getPlayer; also read by isVisible/canAccess paths)
    int  type;                //   8   21.4 M
    int  positionX;           //  12   15.1 M, plus getLeft/Top/Right/Bottom
    int  positionY;           //  16
    int  order;               //  20
    int  hitPoints;           //  24
    int  shields;             //  28
    int  energy;              //  32
    int  resources;           //  36
    bool exists;              //  40   14.9 M
    bool isCompleted;         //  41    5.2 M
    bool isVisible[9];        //  42    9.8 M
    bool isDetected;          //  51   (UnitImpl::canAccess reads this every unit, every frame)
    bool isIdle;              //  52
    bool isMorphing;          //  53
    bool isConstructing;      //  54
    bool isCloaked;           //  55
    bool isBurrowed;          //  56
    bool isLifted;            //  57
    bool isTraining;          //  58
    bool isMoving;            //  59
    bool isGathering;         //  60
    bool isAttacking;         //  61
    bool isSelected;          //  62
    bool isPowered;           //  63

    // ---------------------------------------------------------------------------------
    // COLD (byte 64 onward). Grouped doubles -> ints -> bools so the 8-byte alignment of
    // `angle` costs no padding and the int-stranded-among-bools holes upstream had
    // (carryResourceType, buttonset) are gone.
    // ---------------------------------------------------------------------------------
    double angle;
    double velocityX;
    double velocityY;

    int clearanceLevel;       // no readers in this tree; kept for API compatibility only
    int lastHitPoints;
    int resourceGroup;

    int killCount;
    int acidSporeCount;
    int scarabCount;
    int interceptorCount;
    int spiderMineCount;
    int groundWeaponCooldown;
    int airWeaponCooldown;
    int spellCooldown;
    int defenseMatrixPoints;

    int defenseMatrixTimer;
    int ensnareTimer;
    int irradiateTimer;
    int lockdownTimer;
    int maelstromTimer;
    int orderTimer;
    int plagueTimer;
    int removeTimer;
    int stasisTimer;
    int stimTimer;

    int buildType;
    int trainingQueueCount;
    int trainingQueue[5];
    int tech;
    int upgrade;
    int remainingBuildTime;
    int remainingTrainTime;
    int remainingResearchTime;
    int remainingUpgradeTime;
    int buildUnit;

    int target;
    int targetPositionX;
    int targetPositionY;
    int orderTarget;
    int orderTargetPositionX;
    int orderTargetPositionY;
    int secondaryOrder;
    int rallyPositionX;
    int rallyPositionY;
    int rallyUnit;
    int addon;
    int nydusExit;
    int powerUp;

    int transport;
    int carrier;
    int hatchery;

    int carryResourceType;
    int buttonset;
    int lastAttackerPlayer;
    int replayID;

    bool hasNuke;
    bool isAccelerating;
    bool isAttackFrame;
    bool isBeingGathered;
    bool isBlind;
    bool isBraking;
    bool isHallucination;
    bool isInterruptible;
    bool isInvincible;
    bool isParasited;
    bool isStartingAttack;
    bool isStuck;
    bool isUnderStorm;
    bool isUnderDarkSwarm;
    bool isUnderDWeb;
    bool recentlyAttacked;
  };

  // The performance contract, enforced. Every field the hot accessors read must sit inside
  // the first 64-byte line; if a future edit pushes one out, this fails at compile time
  // rather than quietly costing a second cache line per unit per frame.
  static_assert(offsetof(UnitData, id)          <  64, "UnitData: id left cache line 0");
  static_assert(offsetof(UnitData, player)      <  64, "UnitData: player left cache line 0");
  static_assert(offsetof(UnitData, type)        <  64, "UnitData: type left cache line 0");
  static_assert(offsetof(UnitData, positionX)   <  64, "UnitData: positionX left cache line 0");
  static_assert(offsetof(UnitData, positionY)   <  64, "UnitData: positionY left cache line 0");
  static_assert(offsetof(UnitData, exists)      <  64, "UnitData: exists left cache line 0");
  static_assert(offsetof(UnitData, isCompleted) <  64, "UnitData: isCompleted left cache line 0");
  static_assert(offsetof(UnitData, isDetected)  <  64, "UnitData: isDetected left cache line 0");
  // isVisible is an array: the WHOLE array must fit, not just its first byte.
  static_assert(offsetof(UnitData, isVisible) + sizeof(((UnitData*)0)->isVisible) <= 64,
                "UnitData: isVisible straddles the end of cache line 0");
  // The cold region must start on the next line, or the packing above achieved nothing.
  static_assert(offsetof(UnitData, angle) == 64, "UnitData: cold region must start at byte 64");
}
