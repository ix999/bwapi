#pragma once
/**
 *  Used in UnitCommand
 */

namespace BWAPIC
{
  namespace CommandType
  {
    enum Enum
    {
      None,
      SetScreenPosition,
      PingMinimap,
      EnableFlag,
      Printf,
      SendText,
      PauseGame,
      ResumeGame,
      LeaveGame,
      RestartGame,
      SetLocalSpeed,
      SetLatCom,
      SetGui,
      SetFrameSkip,
      SetMap,
      SetAllies,
      SetVision,
      SetCommandOptimizerLevel,
      SetRevealAll,
      // The lab beds' cheats for a client bot (the in-process Game/Player/Unit cheat calls, BOT_QA / BOT_CAST_BED).
      // Appended so every earlier value is unchanged. Arguments:
      //   CreateUnit       value1 = a strings[] index holding "player type x y"
      //   KillUnit         value1 = unit index
      //   SetMinerals      value1 = player id, value2 = amount
      //   SetGas           value1 = player id, value2 = amount
      //   SetResearched    value1 = player id, value2 = tech id | (researched << 16)
      //   SetUpgradeLevel  value1 = player id, value2 = upgrade id | (level << 16)
      //   SetEnergy        value1 = unit index, value2 = energy
      CreateUnit,
      KillUnit,
      SetMinerals,
      SetGas,
      SetResearched,
      SetUpgradeLevel,
      SetEnergy
    };
  }
}