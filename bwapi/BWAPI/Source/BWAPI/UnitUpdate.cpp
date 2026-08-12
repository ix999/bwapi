#include "UnitImpl.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

#include <Util/Convenience.h>

#include <BWAPI/Player.h>
#include <BWAPI/Order.h>
#include <BWAPI/GameImpl.h>
#include <BWAPI/PlayerImpl.h>
#include <BWAPI/WeaponType.h>

#include <BW/BWData.h>
#include <BW/Anims.h>
#include <BW/UnitTarget.h>
#include <BW/UnitStatusFlags.h>
#include <BW/MovementFlags.h>
#include <BW/UnitMovementState.h>
#include "Server.h"
#include "BWtoBWAPI.h"

#include "../../../Debug.h"

namespace BWAPI
{
  // Just hardcode some values that encompass the majority of the scanner graphic (as a hack for now)
  // Note that scanner sweeps are tricky, since the scanning graphic isn't associated with the scanner unit
  bool isScannerVisible(BW::Position position)
  {
    GameImpl& game = BroodwarImpl;  // hoist the thread_local deref (cut 3)
    int left = (position.x - 64) / 32;
    int top = (position.y - 64) / 32;
    int right = (position.x + 64) / 32;
    int bottom = (position.y + 64) / 32;
    for (int x = left; x <= right; ++x)
    {
      for (int y = top; y <= bottom; ++y)
      {
        if (game.isVisible(x, y))
          return true;
      }
    }
    return false;
  }

  // True when every input updateInternalData()/updateData() would read for this unit is
  // covered by the mirror fingerprint. The excluded cases each read state outside the
  // unit's own engine blocks: scanner sweeps read map tiles, fighters/loaded units read
  // their parent's position, nydus/addon/powerup paths read the linked unit's liveness,
  // and replay/CompleteMapInformation/UserInput modes read target-resolution/selection
  // state the fingerprint does not carry.
  bool UnitImpl::mirrorVerifyMode()
  {
    static const bool verify = [] {
      const char* v = std::getenv("SB_MIRROR_SKIP");
      return v && std::strcmp(v, "verify") == 0;
    }();
    return verify;
  }

  bool UnitImpl::mirrorSeqEnabled()
  {
    static const bool seq = [] {
      const char* v = std::getenv("SBBOT_MIRROR_SEQ");
      return v && *v && std::strcmp(v, "0") != 0;
    }();
    return seq;
  }

  bool UnitImpl::mirrorSnapRelocEnabled()
  {
    static const bool reloc = [] {
      const char* v = std::getenv("SBBOT_MIRROR_SNAP_RELOC");
      return v && *v && std::strcmp(v, "0") != 0;
    }();
    return reloc;
  }

  bool UnitImpl::mirrorSkipEligible(BW::Unit& o) const
  {
    GameImpl& game = BroodwarImpl;  // hoist the thread_local deref (cut 3): one TLS wrapper call per invocation, not per access
    // SB_MIRROR_SKIP: unset/1 = on (default), 0 = off (kill-switch), or a category —
    // "workers" | "buildings" | "army" — to restrict skipping to that class only
    // (the divergence-bisect instrument: category runs binary-search a missed input).
    static const int skipMode = [] {
      const char* v = std::getenv("SB_MIRROR_SKIP");
      if (!v || !*v || std::strcmp(v, "1") == 0) return 1;
      if (std::strcmp(v, "workers") == 0) return 2;
      if (std::strcmp(v, "buildings") == 0) return 3;
      if (std::strcmp(v, "army") == 0) return 4;
      if (std::strcmp(v, "verify") == 0) return 5;
      return 0;
    }();
    if (skipMode == 0) return false;
    if (skipMode != 1)
    {
      const UnitType ut(o.unitType());
      bool isWorker = ut.isWorker();
      bool isBuilding = ut.isBuilding();
      if (skipMode == 2 && !isWorker) return false;
      if (skipMode == 3 && !isBuilding) return false;
      if (skipMode == 4 && (isWorker || isBuilding)) return false;
    }
    if (game.isReplay() ||
        game.isFlagEnabled(Flag::CompleteMapInformation) ||
        game.isFlagEnabled(Flag::UserInput))
      return false;
    int t = o.unitType();
    if (t == UnitTypes::Enum::Spell_Scanner_Sweep ||
        t == UnitTypes::Enum::Protoss_Interceptor ||
        t == UnitTypes::Enum::Protoss_Scarab ||
        t == UnitTypes::Enum::Terran_Vulture_Spider_Mine ||
        t == UnitTypes::Enum::Zerg_Nydus_Canal ||
        // remainingTrainTime derives from orderQueueTimer, which cycles every frame and is
        // deliberately outside the fingerprint — never skip the larva-timing depots.
        t == UnitTypes::Enum::Zerg_Hatchery ||
        t == UnitTypes::Enum::Zerg_Lair ||
        t == UnitTypes::Enum::Zerg_Hive)
      return false;
    if (o.statusFlag(BW::StatusFlags::InTransport | BW::StatusFlags::InBuilding))
      return false;
    if (o.currentBuildUnit() || o.building_addon() || o.worker_pPowerup())
      return false;
    return true;
  }

  void UnitImpl::updateInternalData()
  {
    GameImpl& game = BroodwarImpl;  // hoist the thread_local deref (cut 3): one TLS wrapper call per invocation, not per access
    BW::Unit o = bwunit;
    if ( !o )
      return;
    // Mirror dirty-skip (sb-perf mirror cut 2): one seam call replaces the ~100 accessor
    // crossings below when the engine-side inputs are unchanged. Skip only from the second
    // consecutive unchanged frame so history fields (lastHitPoints, last*Cooldown,
    // lastFrameSet) have converged; updateData() honours mirrorSkip and keeps only its
    // externally re-marked resets.
    mirrorSkip = false;
    mirrorVerify = false;
    // Latency compensation writes PREDICTED values (order, targets, queue counts) into the
    // shared data at command-issue time; the engine-side fingerprint cannot see them. Any
    // recently-commanded unit recomputes until one post-latency refresh restores engine
    // truth — otherwise a denied/ignored command's prediction would persist and suppress
    // re-issues (the seed-101 scout Move omission).
    {
      int frame = game.getFrameCount();
      int horizon = game.getLatencyFrames() + 2;
      if (frame - lastCommandFrame < horizon || frame - lastImmediateCommandFrame < horizon)
      {
        mirrorSnapValid = false;
        mirrorStreak = 0;
      }
    }
    if (isAlive && mirrorSkipEligible(o))
    {
      BW::MirrorFingerprint now;
      o.mirrorFingerprint(&now);
      // SNAP_RELOC on: read/write the relocated contiguous snapshot (streams under the SEQ
      // index-order pass); off: the embedded member (baseline). Same bytes either way -> byte-exact.
      BW::MirrorFingerprint& snap = mirrorSnapRelocEnabled() ? game.unitMirrorSnap[getIndex()] : mirrorSnap;
      if (mirrorSnapValid && std::memcmp(&now, &snap, sizeof(now)) == 0)
      {
        if (mirrorStreak >= 1)
        {
          // verify mode: recompute anyway and diff the outputs at the end of updateData —
          // any difference IS the fingerprint hole, printed field-exact.
          if (mirrorVerifyMode())
          {
            mirrorVerify = true;
            if (!mirrorVerifySnap)
              mirrorVerifySnap.reset(new UnitData());
            *mirrorVerifySnap = *self;
          }
          else
          {
            mirrorSkip = true;
            return;
          }
        }
        else
          ++mirrorStreak;
      }
      else
      {
        snap = now;
        mirrorSnapValid = true;
        mirrorStreak = 0;
      }
    }
    else
    {
      mirrorSnapValid = false;
      mirrorStreak = 0;
    }
    int selfPlayerID = game.server.getPlayerID(game.self());
    self->replayID   = game.isFlagEnabled(Flag::CompleteMapInformation) ? BW::UnitTarget(o).getTarget() : 0;
    if (isAlive)
    {
      _getPlayer = game._getPlayer(o.playerID()); //_getPlayer
      //------------------------------------------------------------------------------------------------------
      //isVisible
      for ( int i = 0; i < 9; ++i )
      {
        if ( i == selfPlayerID )
          continue;
        PlayerImpl* player = static_cast<PlayerImpl*>(game.getPlayer(i));
        if ( !o.hasSprite() || !player )
          self->isVisible[i] = false;
        else if (!game.isReplay() && !game.isFlagEnabled(Flag::CompleteMapInformation))
          self->isVisible[i] = false;
        else if ( _getPlayer == player )
          self->isVisible[i] = true;
        else if ( player->isNeutral() )
          self->isVisible[i] = o.visibilityFlags() > 0;
        else
          self->isVisible[i] = (o.visibilityFlags() & (1 << player->getIndex())) != 0;
      }
      if (selfPlayerID >= 0)
      {
        if ( !o.hasSprite() )
        {
          self->isVisible[selfPlayerID] = false;
          self->isDetected              = false;
        }
        else if (_getPlayer == game.self())
        {
          self->isVisible[selfPlayerID] = true;
          self->isDetected              = true;
        }
        else if (o.unitType() == UnitTypes::Spell_Scanner_Sweep)
        {
          self->isVisible[selfPlayerID] = isScannerVisible(o.position());
          self->isDetected = true;
        }
        else
        {
          self->isVisible[selfPlayerID] = (o.visibilityFlags() & (1 << game.BWAPIPlayer->getIndex())) != 0;
          if (o.statusFlag(BW::StatusFlags::RequiresDetection))
          {
            self->isVisible[selfPlayerID] &= ((o.visibilityStatus() == 0xffffffff) ||
                                             ((o.visibilityStatus() & (1 << game.BWAPIPlayer->getIndex())) != 0) ||
                                               o.movementFlag(BW::MovementFlags::Moving | BW::MovementFlags::Accelerating) ||
                                               o.orderID() == Orders::Move ||
                                               o.groundWeaponCooldown() > 0 ||
                                               o.airWeaponCooldown() > 0 ||
                                              !o.statusFlag(BW::StatusFlags::Burrowed) );
          }
          bool canDetect = !o.statusFlag(BW::StatusFlags::RequiresDetection) ||
                           o.visibilityStatus() == 0xffffffff ||
                           ((o.visibilityStatus() & (1 << game.BWAPIPlayer->getIndex())) != 0);
          self->isDetected = self->isVisible[selfPlayerID] & canDetect;
        }
      }
      else
      {
        self->isDetected = false;
        for(unsigned int i = 0; i < 9; ++i)
        {
          if (self->isVisible[i])
          {
            self->isDetected = true;
            break;
          }
        }
      }
      //------------------------------------------------------------------------------------------------------
      //_getType
      _getType = UnitType(o.unitType());
      if ( _getType.isMineralField() )
        _getType = UnitTypes::Resource_Mineral_Field;

      getBuildQueueSlot = (u8)o.buildQueueSlot(); //getBuildQueueSlot
      for ( unsigned int i = 0; i < 5; ++i )
        getBuildQueue[i] = BWAPI::UnitType(o.buildQueue(i));  //getBuildQueue

      if (_getType.isBuilding())
      {
        if (o.orderID() == Orders::ZergBirth          ||
            o.orderID() == Orders::ZergBuildingMorph  ||
            o.orderID() == Orders::ZergUnitMorph      ||
            o.orderID() == Orders::Enum::IncompleteMorphing )
        {
          //if we have a morphing building, set unit type to the build type (what it is morphing to)
          if ( getBuildQueue[(getBuildQueueSlot % 5)] != UnitTypes::None )
            _getType = getBuildQueue[(getBuildQueueSlot % 5)];
        }
      }

      //------------------------------------------------------------------------------------------------------
      //_getTransport
      _getTransport = nullptr;
      if (_getType == UnitTypes::Protoss_Interceptor  ||
          _getType == UnitTypes::Protoss_Scarab       ||
          _getType == UnitTypes::Terran_Vulture_Spider_Mine)
      {
        if (o.fighter_inHanger() == false ||
            o.statusFlag(BW::StatusFlags::InTransport | BW::StatusFlags::InBuilding) )
          _getTransport = game.getUnitFromBWUnit(o.fighter_parent());
      }
      else if (o.statusFlag(BW::StatusFlags::InTransport | BW::StatusFlags::InBuilding) )
        _getTransport = game.getUnitFromBWUnit(o.connectedUnit());

      //------------------------------------------------------------------------------------------------------
      //_getPosition
      _getPosition = BWAPI::Position(_getTransport ? static_cast<UnitImpl*>(_getTransport)->bwunit.position() : o.position());

      _getHitPoints = (int)std::ceil(o.hitPoints() / 256.0); //_getHitPoints
      //------------------------------------------------------------------------------------------------------
      //_getResources
      _getResources = 0;
      if ( _getType.isResourceContainer() )
        _getResources = o.resourceCount();

      hasEmptyBuildQueue = getBuildQueueSlot < 5 ? (getBuildQueue[getBuildQueueSlot] == UnitTypes::None) : false;  //hasEmptyBuildQueue
      _isCompleted = o.statusFlag(BW::StatusFlags::Completed); //_isCompleted
    }
    else // not alive
    {
      //------------------------------------------------------------------------------------------------------
      //isVisible
      MemZero(self->isVisible);
      self->isDetected = false;

      _getPlayer          = nullptr;               //_getPlayer
      _getType            = UnitTypes::Unknown; //_getType
      _getTransport       = nullptr;               //_getTransport
      _getPosition        = Positions::Unknown; //_getPosition
      _getHitPoints       = 0;                  //_getHitPoints
      _getResources       = 0;                  //_getResources
      getBuildQueueSlot   = 0;                  //getBuildQueueSlot
      for ( unsigned int i = 0; i < 5; ++i )
        getBuildQueue[i]  = UnitTypes::None;               //getBuildQueue
      hasEmptyBuildQueue  = true;               //hasEmptyBuildQueue
      _isCompleted        = false;              //_isCompleted
    }
  }
  
  BW::Unit UnitImpl::implGetDamageDealer() const
  {
    if (bwunit.subUnit()) return bwunit.subUnit();
    return bwunit;
  }
  
  int UnitImpl::implGetGroundWeaponCooldown() const 
  {
    const BWAPI::UnitType type(bwunit.unitType());
    if (type == BWAPI::UnitTypes::Protoss_Reaver || type == BWAPI::UnitTypes::Hero_Warbringer)
    {
      return bwunit.mainOrderTimer();
    }
    return implGetDamageDealer().groundWeaponCooldown();
  }
  
  int UnitImpl::implGetAirWeaponCooldown() const
  {
    return implGetDamageDealer().airWeaponCooldown();
  }
  
  bool UnitImpl::implIsAttacking() const
  {
    BW::Anims::Enum animState = BW::Anims::Init;
    BW::Unit damageDealer = implGetDamageDealer();
    if (damageDealer.hasSprite() && damageDealer.hasSprite_pImagePrimary())
    {
      animState = (BW::Anims::Enum)damageDealer.sprite_pImagePrimary_anim();
    }
    return (animState == BW::Anims::GndAttkRpt ||  //isAttacking
            animState == BW::Anims::AirAttkRpt ||
            animState == BW::Anims::GndAttkInit ||
            animState == BW::Anims::AirAttkInit) && bwunit.orderTarget_pUnit();
  }
  
  /// @todo TODO Refactor this entirely
  void UnitImpl::updateData()
  {
    GameImpl& game = BroodwarImpl;  // hoist the thread_local deref (cut 3): one TLS wrapper call per invocation, not per access
    BW::Unit o = bwunit;
    // Mirror dirty-skip: all outputs below are functions of the unchanged fingerprint and
    // already hold their converged values. Only the swarm/dweb flags are reset — they are
    // re-marked every frame by the overlap pass in updateUnits, not by this function.
    if (mirrorSkip)
    {
      self->isUnderDarkSwarm = false;
      self->isUnderDWeb      = false;
      return;
    }
    self->isUnderDarkSwarm = false;
    self->isUnderDWeb      = false;
    if (canAccess())
    {
      self->positionX = _getPosition.x; //getPosition
      self->positionY = _getPosition.y; //getPosition
      //------------------------------------------------------------------------------------------------------
      //getAngle
      int d = o.currentDirection1();
      d -= 64;
      if (d < 0)
        d += 256;

      self->angle     = (double)d * 3.14159265358979323846 / 128.0;
      self->velocityX = (double)o.current_speed_x() / 256.0; //getVelocityX
      self->velocityY = (double)o.current_speed_y() / 256.0; //getVelocityY
      //------------------------------------------------------------------------------------------------------
      self->groundWeaponCooldown = implGetGroundWeaponCooldown(); //getGroundWeaponCooldown
      self->airWeaponCooldown = implGetAirWeaponCooldown(); //getAirWeaponCooldown
      self->spellCooldown = o.spellCooldown();  //getSpellCooldown

      self->isAttacking = implIsAttacking();
      
      // startingAttack
      int airWeaponCooldown = implGetAirWeaponCooldown();
      int groundWeaponCooldown = implGetGroundWeaponCooldown();
      bool startingAttack = (airWeaponCooldown > lastAirWeaponCooldown || groundWeaponCooldown > lastGroundWeaponCooldown) && self->isAttacking;
      lastAirWeaponCooldown = airWeaponCooldown;
      lastGroundWeaponCooldown = groundWeaponCooldown;

      self->isStartingAttack = startingAttack;  //isStartingAttack

      //isAttackFrame
      self->isAttackFrame = false;
      BW::Unit damageDealer = implGetDamageDealer();
      if (damageDealer.hasSprite() && damageDealer.hasSprite_pImagePrimary())
      { 
        int restFrame = _getType.isValid() ? AttackAnimationRestFrame[_getType] : -1;
        self->isAttackFrame = startingAttack || 
                             (self->isAttacking && 
                              restFrame != -1 && 
                              (damageDealer.sprite_pImagePrimary_frameSet() != restFrame ||
                              lastFrameSet != restFrame) );
        lastFrameSet = damageDealer.sprite_pImagePrimary_frameSet();
      }

      self->isBurrowed  = o.statusFlag(BW::StatusFlags::Burrowed);  //isBurrowed
      self->isCloaked   = o.statusFlag(BW::StatusFlags::Cloaked) && !o.statusFlag(BW::StatusFlags::Burrowed); //isCloaked
      self->isCompleted = _isCompleted; //isCompleted
      self->isMoving    = o.movementFlag(BW::MovementFlags::Moving | BW::MovementFlags::Accelerating) ||
                          self->order == Orders::Move; //isMoving
    }
    else
    {
      self->positionX             = BWAPI::Positions::Unknown.x;  //getPosition
      self->positionY             = BWAPI::Positions::Unknown.y;  //getPosition
      self->angle                 = 0;      //getAngle
      self->velocityX             = 0;      //getVelocityX
      self->velocityY             = 0;      //getVelocityY
      self->groundWeaponCooldown  = 0;      //getGroundWeaponCooldown
      self->airWeaponCooldown     = 0;      //getAirWeaponCooldown
      self->spellCooldown         = 0;      //getSpellCooldown
      self->isAttacking           = false;  //isAttacking
      self->isBurrowed            = false;  //isBurrowed
      self->isCloaked             = false;  //isCloaked
      self->isCompleted           = false;  //isCompleted
      self->isMoving              = false;  //isMoving
      self->isStartingAttack      = false;  //isStartingAttac
    }

    self->scarabCount = 0;
    self->interceptorCount = 0;
    self->spiderMineCount = 0;
    self->carrier = -1;
    self->hatchery = -1;
    if (canAccessDetected())
    {
      self->lastHitPoints       = wasAccessible ? self->hitPoints : _getHitPoints;  //getHitPoints
      self->hitPoints           = _getHitPoints;  //getHitPoints
      self->shields             = _getType.maxShields() > 0 ? (int)std::ceil(o.shieldPoints()/256.0) : 0;  //getShields
      self->energy              = _getType.isSpellcaster()  ? (int)std::ceil(o.energy()/256.0)       : 0;  //getEnergy
      self->resources           = _getResources;                        //getResources
      //self->resourceGroup       = _getType.isResourceContainer() ? o.resourceGroup() : 0; //getResourceGroup
      self->resourceGroup       = 0; //getResourceGroup
      self->killCount           = o.killCount();        //getKillCount
      self->acidSporeCount      = o.acidSporeCount();   //getAcidSporeCount
      self->defenseMatrixPoints = o.defenseMatrixDamage()/256;  //getDefenseMatrixPoints
      self->defenseMatrixTimer  = o.defenseMatrixTimer(); //getDefenseMatrixTimer
      self->ensnareTimer        = o.ensnareTimer();     //getEnsnareTimer
      self->irradiateTimer      = o.irradiateTimer();   //getIrradiateTimer
      self->lockdownTimer       = o.lockdownTimer();    //getLockdownTimer
      self->maelstromTimer      = o.maelstromTimer();   //getMaelstromTimer
      self->orderTimer          = o.mainOrderTimer();   //getOrderTimer
      self->plagueTimer         = o.plagueTimer();      //getPlagueTimer
      self->removeTimer         = o.removeTimer();      //getRemoveTimer
      self->stasisTimer         = o.stasisTimer();      //getStasisTimer
      self->stimTimer           = o.stimTimer();        //getStimTimer
      self->order               = o.orderID();          //getOrder
      self->secondaryOrder      = o.secondaryOrderID(); //getSecondaryOrder
      self->buildUnit           = o.currentBuildUnit() ? game.server.getUnitID(game.getUnitFromBWUnit(o.currentBuildUnit())) : -1; //getBuildUnit
      //------------------------------------------------------------------------------------------------------
      //isTraining
      if (_getType == UnitTypes::Terran_Nuclear_Silo &&
          o.secondaryOrderID() == Orders::Train)
        self->isTraining = true;
      else if (!_getType.canProduce())
        self->isTraining = false;
      else if (_getType.getRace() == Races::Zerg && _getType.isResourceDepot())
        self->isTraining = false;
      else
        self->isTraining = !hasEmptyBuildQueue;
      //------------------------------------------------------------------------------------------------------
      //isMorphing
      self->isMorphing = self->order == Orders::ZergBirth ||
                         self->order == Orders::ZergBuildingMorph ||
                         self->order == Orders::ZergUnitMorph ||
                         self->order == Orders::Enum::IncompleteMorphing;

      if (self->isCompleted && self->isMorphing)
      {
        self->isCompleted = false;
        _isCompleted      = false;
      }
      //------------------------------------------------------------------------------------------------------
      //isConstructing
      self->isConstructing =  self->isMorphing                                    ||
                              self->order == Orders::ConstructingBuilding         ||
                              self->order == Orders::PlaceBuilding                ||
                              self->order == Orders::Enum::DroneBuild             ||
                              self->order == Orders::Enum::DroneStartBuild        ||
                              self->order == Orders::Enum::DroneLand              ||
                              self->order == Orders::Enum::PlaceProtossBuilding   ||
                              self->order == Orders::Enum::CreateProtossBuilding  ||
                              self->order == Orders::Enum::IncompleteBuilding     ||
                              self->order == Orders::Enum::IncompleteWarping      ||
                              self->order == Orders::Enum::IncompleteMorphing     ||
                              self->order == Orders::BuildNydusExit               ||
                              self->order == Orders::BuildAddon                   ||
                              self->secondaryOrder == Orders::BuildAddon          ||
                              (!self->isCompleted && self->buildUnit != -1);
      //------------------------------------------------------------------------------------------------------
      //isIdle
      if (self->isTraining ||
          self->isConstructing ||
          self->isMorphing ||
          self->order == Orders::ResearchTech ||
          self->order == Orders::Upgrade )
        self->isIdle = false;
      else
        self->isIdle = self->order == Orders::PlayerGuard  ||
                       self->order == Orders::Guard        ||
                       self->order == Orders::Stop         ||
                       self->order == Orders::PickupIdle   ||
                       self->order == Orders::Nothing      ||
                       self->order == Orders::Medic        ||
                       self->order == Orders::Carrier      ||
                       self->order == Orders::Reaver       ||
                       self->order == Orders::Critter      ||
                       self->order == Orders::Neutral      ||
                       self->order == Orders::TowerGuard   ||
                       self->order == Orders::Burrowed     ||
                       self->order == Orders::NukeTrain    ||
                       self->order == Orders::Larva;
      self->target               = game.server.getUnitID(game.getUnitFromBWUnit(o.moveTarget_pUnit())); //getTarget
      self->targetPositionX      = o.moveTarget().x;  //getTargetPosition
      self->targetPositionY      = o.moveTarget().y;  //getTargetPosition
      self->orderTargetPositionX = o.orderTarget().x;
      self->orderTargetPositionY = o.orderTarget().y;
      self->orderTarget          = game.server.getUnitID(game.getUnitFromBWUnit(o.orderTarget_pUnit()));  //getOrderTarget
      //------------------------------------------------------------------------------------------------------
      //getAddon
      self->addon = -1;
      if (_getType.isBuilding())
      {
        UnitImpl* addon = game.getUnitFromBWUnit(o.currentBuildUnit());
        if ( addon && addon->isAlive && UnitType(addon->bwunit.unitType()).isAddon() )
          self->addon = game.server.getUnitID(addon);
        else
        {
          addon = game.getUnitFromBWUnit(o.building_addon());
          if ( addon && addon->isAlive && UnitType(addon->bwunit.unitType()).isAddon() )
            self->addon = game.server.getUnitID(addon);
        }
      }
      //------------------------------------------------------------------------------------------------------
      //getNydusExit
      self->nydusExit = -1;
      if ( _getType == UnitTypes::Zerg_Nydus_Canal )
      {
        UnitImpl* nydus = game.getUnitFromBWUnit(o.nydus_exit());
        if ( nydus && nydus->isAlive && nydus->bwunit.unitType() == UnitTypes::Zerg_Nydus_Canal )
          self->nydusExit = game.server.getUnitID(nydus);
      }
      //------------------------------------------------------------------------------------------------------
      //getPowerUp
      self->powerUp = -1;
      UnitImpl* powerUp = game.getUnitFromBWUnit(o.worker_pPowerup());
      if (powerUp && powerUp->isAlive)
        self->powerUp = game.server.getUnitID(powerUp);

      self->isAccelerating  = o.movementFlag(BW::MovementFlags::Accelerating);  //isAccelerating
      self->isBeingGathered = _getType.isResourceContainer() && (o.gatherQueueCount() || o.nextGatherer());  //isBeingGathered
      self->isBlind         = o.isBlind() != 0;   //isBlind
      self->isBraking       = o.movementFlag(BW::MovementFlags::Braking);   //isBraking
      //------------------------------------------------------------------------------------------------------
      //isCarryingGas, isCarryingMinerals
      self->carryResourceType = _getType.isWorker() ? o.resourceType() : 0;

      self->isGathering     = _getType.isWorker() && o.statusFlag(BW::StatusFlags::IsGathering);   //isGatheringMinerals; isGatheringGas
      self->isLifted        = o.statusFlag(BW::StatusFlags::InAir) &&
                              UnitType(o.unitType()).isBuilding(); //isLifted
      self->isParasited     = o.parasiteFlags() != 0; //isParasited
      self->isSelected      = game.isFlagEnabled(BWAPI::Flag::UserInput) && userSelected; //isSelected
      self->isUnderStorm    = o.stormTimer() != 0; //isUnderStorm
      self->isPowered       = !(_getType.getRace() == Races::Protoss && _getType.isBuilding() && o.statusFlag(BW::StatusFlags::DoodadStatesThing)); // !isUnpowered
      self->isStuck         = o.movementState() == BW::UM_MoveToLegal;
      self->isInterruptible = !o.statusFlag(BW::StatusFlags::CanNotReceiveOrders); //isInterruptible
      self->isInvincible    = o.statusFlag(BW::StatusFlags::Invincible); //isInvincible
      //self->buttonset       = o.currentButtonSet();
      self->buttonset       = 0;
      //self->lastAttackerPlayer = o.lastAttackingPlayer();
      self->lastAttackerPlayer = 0;
      //self->recentlyAttacked = o.lastEventColor == 174 ? o.lastEventTimer != 0 : false;
      self->recentlyAttacked = false;

      switch (_getType)
      {
      case UnitTypes::Enum::Protoss_Reaver:
      case UnitTypes::Enum::Hero_Warbringer:
        self->scarabCount = o.carrier_inHangerCount();
        break;
      case UnitTypes::Enum::Terran_Vulture:
      case UnitTypes::Enum::Hero_Jim_Raynor_Vulture:
        self->spiderMineCount = o.vulture_spiderMineCount();
        break;
      case UnitTypes::Enum::Protoss_Carrier:
      case UnitTypes::Enum::Hero_Gantrithor:
        self->interceptorCount = o.carrier_inHangerCount() + o.carrier_outHangerCount();
        break;
      case UnitTypes::Enum::Protoss_Interceptor:
        self->carrier = game.server.getUnitID(game.getUnitFromBWUnit(o.fighter_parent()));
        break;
      case UnitTypes::Enum::Zerg_Larva:
        self->hatchery = game.server.getUnitID(game.getUnitFromBWUnit(o.connectedUnit()));
        break;
      default:
        break;
      }
    }
    else
    {
      self->lastHitPoints       = 0;      //getHitPoints
      self->hitPoints           = 0;      //getHitPoints
      self->shields             = 0;      //getShields
      self->energy              = 0;      //getEnergy
      //self->resources           = 0;      //getResources
      self->resourceGroup       = 0;      //getResourceGroup
      self->killCount           = 0;      //getKillCount
      self->defenseMatrixPoints = 0;      //getDefenseMatrixPoints
      self->defenseMatrixTimer  = 0;      //getDefenseMatrixTimer
      self->ensnareTimer        = 0;      //getEnsnareTimer
      self->irradiateTimer      = 0;      //getIrradiateTimer
      self->lockdownTimer       = 0;      //getLockdownTimer
      self->maelstromTimer      = 0;      //getMaelstromTimer
      self->orderTimer          = 0;      //getOrderTimer
      self->plagueTimer         = 0;      //getPlagueTimer
      self->removeTimer         = 0;      //getRemoveTimer
      self->stasisTimer         = 0;      //getStasisTimer
      self->stimTimer           = 0;      //getStimTimer
      self->order               = Orders::Unknown;  //getOrder
      self->secondaryOrder      = Orders::Unknown;  //getSecondaryOrder
      self->buildUnit           = -1;     //getBuildUnit
      self->isTraining          = false;  //isTraining
      self->isMorphing          = false;  //isMorphing
      self->isConstructing      = false;  //isConstructing
      self->isIdle              = false;  //isIdle
      self->target              = -1;     //getTarget
      self->targetPositionX     = Positions::Unknown.x; //getTargetPosition
      self->targetPositionY     = Positions::Unknown.y; //getTargetPosition
      self->orderTarget         = -1;     //getOrderTarget
      self->orderTargetPositionX = Positions::Unknown.x;
      self->orderTargetPositionY = Positions::Unknown.y;
      self->addon               = -1;     //getAddon
      self->nydusExit           = -1;     //getNydusExit
      self->powerUp             = -1;     //getPowerUp
      self->isAccelerating      = false;  //isAccelerating
      self->isBeingGathered     = false;  //isBeingGathered
      self->isBlind             = false;  //isBlind
      self->isBraking           = false;  //isBraking
      self->carryResourceType   = 0;      //isCarryingMinerals;isCarryingGas
      self->isLifted            = false;  //isLifted
      self->isParasited         = false;  //isParasited
      self->isSelected          = false;  //isSelected
      self->isUnderStorm        = false;  //isUnderStorm
      self->isUnderDarkSwarm    = false;
      self->isUnderDWeb         = false;
      self->isPowered            = true;   //!isUnpowered
      self->isStuck             = false;  //isStuck
      self->isInterruptible     = false;  //isInterruptible
      self->buttonset           = UnitTypes::None;
      self->lastAttackerPlayer  = -1;
      self->recentlyAttacked    = false;
    }
    if (canAccess())
    {
      self->exists = true;
      self->player = game.server.getPlayerID(_getPlayer);
      self->type   = _getType;
    }
    else
    {
      self->exists = false;
      self->player = game.server.getPlayerID(game._getPlayer(11));
      self->type   = UnitTypes::Unknown;
    }
    if (canAccessInside())
    {
      // Default assignments
      self->trainingQueueCount    = 0;
      self->remainingTrainTime    = 0;
      self->hasNuke               = false;
      self->buildType             = UnitTypes::None;
      self->tech                  = TechTypes::None;
      self->remainingResearchTime = 0;
      self->upgrade               = UpgradeTypes::None;
      self->remainingUpgradeTime  = 0;
      self->remainingBuildTime    = 0;
      self->rallyUnit             = -1;

      //------------------------------------------------------------------------------------------------------
      // getTrainingQueue
      if ( !hasEmptyBuildQueue )
      {
        for(int i = getBuildQueueSlot % 5; getBuildQueue[i] != UnitTypes::None && self->trainingQueueCount < 5; i = (i + 1) % 5)
        {
          self->trainingQueue[self->trainingQueueCount] = getBuildQueue[i];
          self->trainingQueueCount++;
        }
      }
      //------------------------------------------------------------------------------------------------------
      // getRemainingTrainTime
      if ( o.currentBuildUnit() )
        self->remainingTrainTime = o.currentBuildUnit().remainingBuildTime();

      //------------------------------------------------------------------------------------------------------
      // Unit Type switch; special cases
      switch ( _getType )
      {
      case UnitTypes::Enum::Terran_Nuclear_Silo:
        if (o.secondaryOrderID() == Orders::Train)
        {
          self->trainingQueue[0]   = UnitTypes::Enum::Terran_Nuclear_Missile;
          self->trainingQueueCount = 1;
        }
        self->hasNuke = (o.silo_bReady() != 0);
        break;
      case UnitTypes::Enum::Zerg_Hatchery:
      case UnitTypes::Enum::Zerg_Lair:
      case UnitTypes::Enum::Zerg_Hive:
        if ( !self->isCompleted && self->buildType == UnitTypes::Enum::Zerg_Hatchery )
          self->remainingTrainTime = self->remainingBuildTime;
        else
          self->remainingTrainTime = o.building_larvaTimer() * 9 + ((o.orderQueueTimer() + 8) % 9);
        break;
      default:
        break;
      }

      //------------------------------------------------------------------------------------------------------
      // Order Type switch; special cases
      switch ( self->order )
      {
        case Orders::Enum::IncompleteBuilding:
        case Orders::Enum::IncompleteWarping:
          self->buildType = self->type;
          break;
        case Orders::Enum::ConstructingBuilding:
          if ( self->buildUnit != -1 )
            self->buildType = static_cast<UnitImpl*>(getBuildUnit())->bwunit.unitType();
          break;
        case Orders::Enum::IncompleteMorphing:
          {
            UnitType type = getBuildQueue[getBuildQueueSlot % 5];
            self->buildType = type == UnitTypes::None ? self->type : (int)type;
          }
          break;
        case Orders::Enum::PlaceBuilding:
        case Orders::Enum::PlaceProtossBuilding:
        case Orders::Enum::ZergUnitMorph:
        case Orders::Enum::ZergBuildingMorph:
        case Orders::Enum::DroneLand:
          self->buildType = getBuildQueue[(getBuildQueueSlot % 5)];
          break;
        case Orders::Enum::ResearchTech:
          self->tech = o.building_techType();
          self->remainingResearchTime = o.building_upgradeResearchTime();
          break;
        case Orders::Enum::Upgrade:
          self->upgrade = o.building_upgradeType();
          self->remainingUpgradeTime = o.building_upgradeResearchTime();
          break;
      }

      //getBuildType
      if ( !hasEmptyBuildQueue &&
           !self->isIdle       &&
           self->secondaryOrder == Orders::BuildAddon )
        self->buildType = getBuildQueue[(getBuildQueueSlot % 5)];

      //------------------------------------------------------------------------------------------------------
      //getRemainingBuildTime
      if ( !self->isCompleted && (!self->isMorphing || self->buildType != UnitTypes::None) )
        self->remainingBuildTime = o.remainingBuildTime();
      //------------------------------------------------------------------------------------------------------
      //getRallyPosition
      if (this->_getType.canProduce())
      {
        self->rallyPositionX = o.rally_position().x;
        self->rallyPositionY = o.rally_position().y;
      }
      else
      {
        self->rallyPositionX = Positions::None.x;
        self->rallyPositionY = Positions::None.y;
      }
      //------------------------------------------------------------------------------------------------------
      //getRallyUnit
      if ( this->_getType.canProduce() )
        self->rallyUnit = game.server.getUnitID(game.getUnitFromBWUnit(o.rally_unit()));

      self->transport       = game.server.getUnitID(_getTransport);   //getTransport
      self->isHallucination = o.statusFlag(BW::StatusFlags::IsHallucination);  //isHallucination
    }
    else
    {
      self->buildType             = UnitTypes::None;     //getBuildType
      self->trainingQueueCount    = 0;                    //getTrainingQueue
      self->tech                  = TechTypes::None;     //getTech
      self->upgrade               = UpgradeTypes::None;  //getUpgrade
      self->remainingBuildTime    = 0;                    //getRemainingBuildTime
      self->remainingTrainTime    = 0;                    //getRemainingTrainTime
      self->remainingResearchTime = 0;                    //getRemainingResearchTime
      self->remainingUpgradeTime  = 0;                    //getRemainingUpgradeTime
      self->rallyPositionX        = Positions::None.x;  //getRallyPosition
      self->rallyPositionY        = Positions::None.y;  //getRallyPosition
      self->rallyUnit             = -1;                   //getRallyUnit
      self->transport             = -1;                   //getTransport
      self->hasNuke               = false;                //hasNuke
      self->isHallucination       = false;                //isHallucination
    }
    if ( self->order >= 0 && self->order < Orders::Enum::MAX )
      self->order = BWtoBWAPI_Order[self->order];
    if ( self->secondaryOrder >= 0 && self->secondaryOrder < Orders::Enum::MAX )
      self->secondaryOrder = BWtoBWAPI_Order[self->secondaryOrder];

    if (mirrorVerify && mirrorVerifySnap)
    {
      mirrorVerify = false;
      ++GameImpl::mirrorVerifyCompared;
      const unsigned char* a = reinterpret_cast<const unsigned char*>(mirrorVerifySnap.get());
      const unsigned char* b = reinterpret_cast<const unsigned char*>(self);
      for (size_t i = 0; i != sizeof(UnitData); ++i)
        if (a[i] != b[i])
          std::printf("MIRROR-VERIFY-DIFF f=%d id=%d type=%d offset=%zu stale=%02x fresh=%02x\n",
                      game.getFrameCount(), id, (int)o.unitType(), i, a[i], b[i]);
    }
  }
}
