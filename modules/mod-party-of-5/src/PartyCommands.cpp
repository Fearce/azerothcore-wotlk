/*
 * WowPs Party-of-5 mod — `.party ...` chat commands
 *
 *  .party enroll [name]   — add a character on your account to the party at the next free slot
 *  .party list            — show your account's party
 *  .party leave           — remove your currently-logged-in character from its party
 *  .party slot <0-4>      — mark a slot as the active-on-login default for next login
 */

#include "PartyMgr.h"
#include "PartyRotation.h"
#include "PartyTestRunner.h"

#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "MotionMaster.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "AiObjectContext.h"
#include "Value.h"
#include "Pet.h"
#include "Group.h"
#include "GroupMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "WorldSession.h"

#include <algorithm>
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace
{
    static char const* ClassName(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return "Warrior";
            case CLASS_PALADIN:      return "Paladin";
            case CLASS_HUNTER:       return "Hunter";
            case CLASS_ROGUE:        return "Rogue";
            case CLASS_PRIEST:       return "Priest";
            case CLASS_DEATH_KNIGHT: return "Death Knight";
            case CLASS_SHAMAN:       return "Shaman";
            case CLASS_MAGE:         return "Mage";
            case CLASS_WARLOCK:      return "Warlock";
            case CLASS_DRUID:        return "Druid";
            default:                 return "?";
        }
    }
}

class party_commandscript : public CommandScript
{
public:
    party_commandscript() : CommandScript("party_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        // SEC_PLAYER (value 0): IsInvokerVisible falls through to IsAvailable(0),
        // which returns true for every connected account. RBAC_PERM_* values
        // (>= 200) go through HasPermission instead and require explicit DB
        // grants — and SEC_PLAYER accounts only get RBAC perm 195 by default,
        // which would silently hide every .party subcommand. See ChatCommand.cpp
        // line ~519. Player-facing commands belong on SEC_PLAYER.
        static ChatCommandTable partyCommandTable =
        {
            { "enroll", HandleEnrollCommand, SEC_PLAYER, Console::Yes },
            { "list",   HandleListCommand,   SEC_PLAYER, Console::Yes },
            { "leave",  HandleLeaveCommand,  SEC_PLAYER, Console::Yes },
            { "slot",   HandleSlotCommand,   SEC_PLAYER, Console::Yes },
            { "swap",   HandleSwapCommand,   SEC_PLAYER, Console::Yes },
            { "unswap", HandleUnswapCommand, SEC_PLAYER, Console::Yes },
            { "rez",          HandleRezCommand,          SEC_PLAYER, Console::Yes },
            { "setrotation",  HandleSetRotationCommand,  SEC_PLAYER, Console::Yes },
            { "getrotation",  HandleGetRotationCommand,  SEC_PLAYER, Console::Yes },
            { "clearrotation",HandleClearRotationCommand,SEC_PLAYER, Console::Yes },
            { "preset",       HandlePresetCommand,       SEC_PLAYER, Console::Yes },
            { "help",         HandleHelpCommand,         SEC_PLAYER, Console::Yes },
            { "goto",         HandleGotoCommand,         SEC_PLAYER, Console::Yes },
            { "stop",         HandleStopCommand,         SEC_PLAYER, Console::Yes },
            { "repair",       HandleRepairCommand,       SEC_PLAYER, Console::Yes },
            { "sellgrays",    HandleSellGraysCommand,    SEC_PLAYER, Console::Yes },
            { "hearth",       HandleHearthCommand,       SEC_PLAYER, Console::Yes },
            { "status",       HandleStatusCommand,       SEC_PLAYER, Console::Yes },
            { "reset",        HandleResetCommand,        SEC_PLAYER, Console::Yes },
        };
        // Admin/test commands. Console::Yes lets them be driven from the
        // worldserver console or SOAP without a player session. SEC_CONSOLE
        // restricts them to admin (so a SOAP'd test harness with admin
        // credentials can invoke them, regular players cannot).
        static ChatCommandTable adminCommandTable =
        {
            { "bootstrap",         HandleAdminBootstrapCommand,         SEC_CONSOLE, Console::Yes },
            { "verify_party",      HandleAdminVerifyPartyCommand,       SEC_CONSOLE, Console::Yes },
            { "run_handler_tests", HandleAdminRunHandlerTestsCommand,   SEC_CONSOLE, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "party",        partyCommandTable },
            { "wowps_admin",  adminCommandTable },
        };
        return commandTable;
    }

    // .party enroll [name]
    static bool HandleEnrollCommand(ChatHandler* handler, Optional<std::string> nameArg)
    {
        Player* requestor = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!requestor)
            return false;

        uint32 targetGuid = 0;
        std::string targetName;

        if (nameArg && !nameArg->empty())
        {
            targetName = *nameArg;
            ObjectGuid const objGuid = sCharacterCache->GetCharacterGuidByName(targetName);
            if (!objGuid)
            {
                handler->PSendSysMessage(
                    "|cffff5555[WowPsParty]|r No character named '{}' exists.",
                    targetName);
                handler->SetSentErrorMessage(true);
                return false;
            }
            targetGuid = objGuid.GetCounter();
        }
        else
        {
            targetGuid = requestor->GetGUID().GetCounter();
            targetName = requestor->GetName();
        }

        WowPsParty::EnrollResult const r = sPartyMgr.Enroll(requestor, targetGuid, targetName);
        switch (r)
        {
            case WowPsParty::EnrollResult::Ok:
                handler->PSendSysMessage(
                    "|cff66ccff[WowPsParty]|r Enrolled '{}' into your party. "
                    "Log in as this character (or any party member) to bring the rest "
                    "of your party online with you.",
                    targetName);
                return true;

            case WowPsParty::EnrollResult::AlreadyEnrolled:
                handler->PSendSysMessage(
                    "|cffffcc55[WowPsParty]|r '{}' is already in your party.",
                    targetName);
                return true;

            case WowPsParty::EnrollResult::TakenByAnotherAccount:
                handler->PSendSysMessage(
                    "|cffff5555[WowPsParty]|r '{}' isn't on your account.",
                    targetName);
                handler->SetSentErrorMessage(true);
                return false;

            case WowPsParty::EnrollResult::PartyFull:
                handler->PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Party is full (5/5). "
                    "Use |cffffff00.party leave|r on a member you want to swap out.");
                handler->SetSentErrorMessage(true);
                return false;

            case WowPsParty::EnrollResult::TargetNotFound:
                handler->PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Character '{}' not found.",
                    targetName);
                handler->SetSentErrorMessage(true);
                return false;

            case WowPsParty::EnrollResult::DatabaseError:
                handler->PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Database error during enrollment. "
                    "Check the server log.");
                handler->SetSentErrorMessage(true);
                return false;
        }
        return false;
    }

    // .party list
    static bool HandleListCommand(ChatHandler* handler, Optional<std::string> /*unused*/)
    {
        WorldSession* session = handler->GetSession();
        if (!session)
            return false;

        uint32 const accountId = session->GetAccountId();
        auto const party = sPartyMgr.GetParty(accountId);

        if (party.empty())
        {
            handler->PSendSysMessage(
                "|cffffcc55[WowPsParty]|r Your account has no party yet. "
                "Use |cffffff00.party enroll|r to add the character you're playing now.");
            return true;
        }

        handler->PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Your account party ({}/{}):",
            uint32(party.size()), uint32(WowPsParty::PARTY_SIZE));
        for (auto const& m : party)
        {
            handler->PSendSysMessage(
                "  |cffaaaaff[{}]|r {} — level {} {}{}",
                uint32(m.slot),
                m.name,
                uint32(m.level),
                ClassName(m.classId),
                m.online ? " |cff44ff44(online)|r" : "");
        }
        return true;
    }

    // .party leave
    static bool HandleLeaveCommand(ChatHandler* handler, Optional<std::string> /*unused*/)
    {
        Player* requestor = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!requestor)
            return false;

        bool const removed = sPartyMgr.Leave(requestor);
        if (removed)
        {
            handler->PSendSysMessage(
                "|cff66ccff[WowPsParty]|r '{}' has left your party.",
                requestor->GetName());
        }
        else
        {
            handler->PSendSysMessage(
                "|cffffcc55[WowPsParty]|r '{}' wasn't enrolled in a party.",
                requestor->GetName());
        }
        return true;
    }

    // .party swap <0-4> — seamlessly take control of the party member at the slot
    static bool HandleSwapCommand(ChatHandler* handler, Optional<uint32> slotArg)
    {
        Player* requestor = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!requestor)
            return false;

        if (!slotArg)
        {
            handler->PSendSysMessage(
                "|cffff5555[WowPsParty]|r Usage: |cffffff00.party swap <0-4>|r");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const slot = *slotArg;
        if (slot >= WowPsParty::PARTY_SIZE)
        {
            handler->PSendSysMessage(
                "|cffff5555[WowPsParty]|r Slot must be 0-4 (got {}).", slot);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // RELOGIN APPROACHES FAILED. Tried both full LogoutPlayer +
        // HandlePlayerLoginOpcode (client went to char-select, DC'd) and
        // quiet logout that skipped SMSG_LOGOUT_COMPLETE (still DC'd).
        // WoW 3.3.5a client doesn't accept SMSG_LOGIN_VERIFY_WORLD mid-
        // session regardless. Back to POSSESS-based swap; the trade-off
        // is tank stands still during possess but at least swap WORKS.
        WowPsParty::SwapResult const r = sPartyMgr.SwapTo(requestor, static_cast<uint8>(slot));
        switch (r)
        {
            case WowPsParty::SwapResult::Ok:
                handler->PSendSysMessage(
                    "|cff66ccff[WowPsParty]|r Controlling slot {}. "
                    "Your previous body becomes a follower bot.", slot);
                handler->PSendSysMessage(
                    "|cffffcc55[WowPsParty]|r WARNING: until the addon ships in Phase 3, the "
                    "action bar shows a pet-style bar and your original spellbook is unsafe to "
                    "use (casting your old spells may disconnect you). Type |cffffff00.party "
                    "unswap|r to return to your real body if things break.");
                return true;

            case WowPsParty::SwapResult::InvalidSlot:
                handler->PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Slot {} is empty. "
                    "Use |cffffff00.party list|r to see your roster.", slot);
                handler->SetSentErrorMessage(true);
                return false;

            case WowPsParty::SwapResult::TargetNotInWorld:
                handler->PSendSysMessage(
                    "|cffff5555[WowPsParty]|r The character in slot {} isn't currently in the world. "
                    "Try again in a few seconds — they may still be spawning.", slot);
                handler->SetSentErrorMessage(true);
                return false;

            case WowPsParty::SwapResult::TargetIsDead:
                handler->PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Slot {} is dead. Rez them first.", slot);
                handler->SetSentErrorMessage(true);
                return false;

            case WowPsParty::SwapResult::AlreadyControllingTarget:
                handler->PSendSysMessage(
                    "|cffffcc55[WowPsParty]|r You're already controlling slot {}.", slot);
                return true;

            case WowPsParty::SwapResult::InBattleground:
                handler->PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Swaps are disabled in battlegrounds and arenas.");
                handler->SetSentErrorMessage(true);
                return false;

            case WowPsParty::SwapResult::VehicleSetupFailed:
                handler->PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Swap failed at the server (charm/possess error). "
                    "Check the worldserver log; this is the spike path mentioned in the architecture.");
                handler->SetSentErrorMessage(true);
                return false;
        }
        return false;
    }

    // Resolve a slot 0-4 to that party member's guid. Returns 0 if invalid or empty.
    static uint32 GuidForSlot(uint32 accountId, uint32 slot)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
            accountId, slot);
        return q ? q->Fetch()[0].Get<uint32>() : 0;
    }

    // .party help
    static bool HandleHelpCommand(ChatHandler* handler, Optional<std::string> /*unused*/)
    {
        handler->PSendSysMessage("|cff66ccff===== WowPsParty =====|r");
        handler->PSendSysMessage(".party enroll [name]   |cff888888- add char to party|r");
        handler->PSendSysMessage(".party list            |cff888888- show roster|r");
        handler->PSendSysMessage(".party leave           |cff888888- remove this char|r");
        handler->PSendSysMessage(".party slot <0-4>      |cff888888- mark active-on-login|r");
        handler->PSendSysMessage(".party swap <0-4>      |cff888888- take control of slot|r");
        handler->PSendSysMessage(".party unswap          |cff888888- return to your body|r");
        handler->PSendSysMessage(".party rez [slot]      |cff888888- revive dead member(s)|r");
        handler->PSendSysMessage(".party preset <slot> <class>  |cff888888- apply default rotation|r");
        handler->PSendSysMessage(".party setrotation <slot> <dsl>");
        handler->PSendSysMessage(".party getrotation [slot]");
        handler->PSendSysMessage(".party clearrotation <slot>");
        handler->PSendSysMessage("|cff66ccffAddon UI:|r F1-F5 swap, /wowps all, /wowps editor");
        return true;
    }

    // .party preset <slot> <class>
    //
    // Apply a sensible default rotation for the given class. Saves the user
    // from typing the DSL by hand when they just want "this slot does priest
    // healer stuff." Valid class names: warrior, paladin, hunter, rogue,
    // priest, deathknight, shaman, mage, warlock, druid.
    static bool HandlePresetCommand(ChatHandler* handler, uint32 slot, Tail classArg)
    {
        WorldSession* session = handler->GetSession();
        if (!session) return false;
        if (slot >= WowPsParty::PARTY_SIZE)
        {
            handler->PSendSysMessage("|cffff5555[WowPsParty]|r Slot must be 0-4.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string cls{ classArg };
        std::transform(cls.begin(), cls.end(), cls.begin(),
                       [](unsigned char c){ return std::tolower(c); });

        // Each preset is a ;-separated DSL (same format as setrotation). Most
        // tuned for level 15 because that's the test party — adjust spell
        // names if you're running higher levels with different spell ranks.
        static const std::unordered_map<std::string, std::string> presets = {
            { "warrior",     "self_health<40|cast_self:Battle Shout|80;has_target|cast:Heroic Strike|50;has_target|cast:Rend|30" },
            { "paladin",     "self_health<35|cast_self:Holy Light|90;always|cast_self:Devotion Aura|70;has_target|cast:Judgement of Light|40" },
            { "hunter",      "self_health<30|cast_self:Healing Potion|95;has_target|cast:Serpent Sting|60;has_target|cast:Arcane Shot|40" },
            { "rogue",       "out_of_combat|cast_self:Stealth|95;has_target|cast:Sinister Strike|50" },
            { "priest",      "self_health<55|cast_self:Lesser Heal|95;has_target|cast:Smite|30;always|cast_self:Power Word: Fortitude|70" },
            { "deathknight", "self_health<35|cast_self:Death Strike|90;has_target|cast:Plague Strike|60;has_target|cast:Blood Strike|40" },
            { "shaman",      "self_health<40|cast_self:Healing Wave|90;has_target|cast:Lightning Bolt|50;always|cast_self:Lightning Shield|70" },
            { "mage",        "self_health<35|cast_self:Frost Nova|95;has_target|cast:Frostbolt|50;has_target|cast:Fireball|40" },
            { "warlock",     "self_health<30|cast_self:Drain Life|95;has_target|cast:Corruption|60;has_target|cast:Shadow Bolt|40" },
            { "druid",       "self_health<40|cast_self:Rejuvenation|90;has_target|cast:Wrath|50;has_target|cast:Moonfire|30" },
        };

        auto it = presets.find(cls);
        if (it == presets.end())
        {
            handler->PSendSysMessage(
                "|cffff5555[WowPsParty]|r Unknown class '{}'. Available: warrior, paladin, hunter, "
                "rogue, priest, deathknight, shaman, mage, warlock, druid.", cls);
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const accountId = session->GetAccountId();
        uint32 const guid = GuidForSlot(accountId, slot);
        if (!guid)
        {
            handler->PSendSysMessage("|cffff5555[WowPsParty]|r Slot {} is empty.", slot);
            handler->SetSentErrorMessage(true);
            return false;
        }

        auto rules = WowPsParty::ParseRotationString(it->second);
        std::string const stored = WowPsParty::SerialiseRotationRules(rules);

        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append(
            "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
            "`gear_lock_json`, `priority_actions_json`) VALUES ({}, '', '', '', '', '{}') "
            "ON DUPLICATE KEY UPDATE `priority_actions_json` = VALUES(`priority_actions_json`)",
            guid, stored);
        CharacterDatabase.CommitTransaction(tx);
        WowPsParty::RotationCacheSet(guid, rules);

        handler->PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Applied {} preset ({} rule(s)) to slot {}.",
            cls, uint32(rules.size()), slot);
        return true;
    }

    // .party setrotation <slot> <dsl>
    // dsl: semicolon-separated rules; each rule = condition|action|priority
    // Example: .party setrotation 2 self_health<40|cast:Lesser Heal|90;always|cast:Smite|10
    static bool HandleSetRotationCommand(ChatHandler* handler, uint32 slot, Tail dslArg)
    {
        WorldSession* session = handler->GetSession();
        if (!session)
            return false;
        if (slot >= WowPsParty::PARTY_SIZE)
        {
            handler->PSendSysMessage("|cffff5555[WowPsParty]|r Slot must be 0-4.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string const dsl{ dslArg };
        if (dsl.empty())
        {
            handler->PSendSysMessage(
                "|cffff5555[WowPsParty]|r Usage: |cffffff00.party setrotation <slot> <rules>|r  "
                "rules = condition|action|priority separated by semicolons. "
                "Conditions: always, in_combat, out_of_combat, has_target, no_target, "
                "self_health<N, self_health>N, self_mana<N/>N, target_health<N/>N. "
                "Actions: cast:<spell name>, cast_self:<spell name>.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const accountId = session->GetAccountId();
        uint32 const guid = GuidForSlot(accountId, slot);
        if (!guid)
        {
            handler->PSendSysMessage("|cffff5555[WowPsParty]|r Slot {} is empty.", slot);
            handler->SetSentErrorMessage(true);
            return false;
        }

        auto rules = WowPsParty::ParseRotationString(dsl);
        if (rules.empty())
        {
            handler->PSendSysMessage(
                "|cffff5555[WowPsParty]|r No valid rules parsed from input.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string const stored = WowPsParty::SerialiseRotationRules(rules);
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append(
            "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
            "`gear_lock_json`, `priority_actions_json`) VALUES ({}, '', '', '', '', '{}') "
            "ON DUPLICATE KEY UPDATE `priority_actions_json` = VALUES(`priority_actions_json`)",
            guid, stored);
        CharacterDatabase.CommitTransaction(tx);

        WowPsParty::RotationCacheSet(guid, rules);
        handler->PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Set {} rule(s) on slot {}.", uint32(rules.size()), slot);
        return true;
    }

    // .party getrotation [slot] — show parsed rules. No-arg = show all 5 slots.
    static bool HandleGetRotationCommand(ChatHandler* handler, Optional<uint32> slotArg)
    {
        WorldSession* session = handler->GetSession();
        if (!session)
            return false;
        uint32 const accountId = session->GetAccountId();

        auto const dump = [&](uint32 slot, uint32 guid)
        {
            QueryResult q = CharacterDatabase.Query(
                "SELECT `priority_actions_json` FROM `party_loadout` WHERE `guid` = {}", guid);
            std::string dsl = q ? q->Fetch()[0].Get<std::string>() : "";
            auto rules = WowPsParty::ParseRotationString(dsl);
            if (rules.empty())
            {
                handler->PSendSysMessage("|cffaaaaff[slot {}]|r |cff888888(no rotation)|r", slot);
                return;
            }
            handler->PSendSysMessage("|cffaaaaff[slot {}]|r {} rule(s):", slot, uint32(rules.size()));
            for (auto const& r : rules)
                handler->PSendSysMessage(
                    "    prio={}  if {}  then {}",
                    r.priority, r.condition, r.action);
        };

        if (slotArg)
        {
            if (*slotArg >= WowPsParty::PARTY_SIZE)
            {
                handler->PSendSysMessage("|cffff5555[WowPsParty]|r Slot must be 0-4.");
                handler->SetSentErrorMessage(true);
                return false;
            }
            uint32 const guid = GuidForSlot(accountId, *slotArg);
            if (!guid)
            {
                handler->PSendSysMessage("|cffffcc55[WowPsParty]|r Slot {} is empty.", *slotArg);
                return true;
            }
            dump(*slotArg, guid);
            return true;
        }

        for (uint8 slot = 0; slot < WowPsParty::PARTY_SIZE; ++slot)
        {
            uint32 const guid = GuidForSlot(accountId, slot);
            if (!guid) continue;
            dump(slot, guid);
        }
        return true;
    }

    // .party clearrotation <slot>
    static bool HandleClearRotationCommand(ChatHandler* handler, uint32 slot)
    {
        WorldSession* session = handler->GetSession();
        if (!session)
            return false;
        if (slot >= WowPsParty::PARTY_SIZE)
        {
            handler->PSendSysMessage("|cffff5555[WowPsParty]|r Slot must be 0-4.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const accountId = session->GetAccountId();
        uint32 const guid = GuidForSlot(accountId, slot);
        if (!guid)
        {
            handler->PSendSysMessage("|cffffcc55[WowPsParty]|r Slot {} is empty.", slot);
            return true;
        }

        CharacterDatabase.Execute(
            "UPDATE `party_loadout` SET `priority_actions_json` = '' WHERE `guid` = {}", guid);
        WowPsParty::RotationCacheClear(guid);
        handler->PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Cleared rotation on slot {}.", slot);
        return true;
    }

    // .party reset — panic button. Revive everyone, teleport to Sentinel Hill,
    // full HP/mana, drop any charm so the user is back in their session body.
    static bool HandleResetCommand(ChatHandler* handler, Optional<std::string> /*unused*/)
    {
        Player* who = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!who) return false;
        sPartyMgr.Unswap(who);

        uint32 const acct = who->GetSession()->GetAccountId();
        auto const party = sPartyMgr.GetParty(acct);
        for (auto const& m : party)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(m.guid));
            if (!p) continue;
            if (!p->IsAlive())
            {
                p->SpawnCorpseBones();
                p->ResurrectPlayer(1.0f);
            }
            p->SetFullHealth();
            if (p->getPowerType() == POWER_MANA)
                p->SetPower(POWER_MANA, p->GetMaxPower(POWER_MANA));
            // Sentinel Hill (Westfall) safe spot
            p->TeleportTo(0, -10641.0f, 1080.0f, 36.0f, 1.5f);
        }
        handler->PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Party reset: alive, full HP/mana, teleported to Sentinel Hill.");
        return true;
    }

    // .party goto <x> <y> [z]   — set the active body's movement target. Bots
    // already follow the master via mod-playerbots' follow strategy, so the
    // whole party walks there (pathfinding around terrain best-effort with
    // whatever mmaps are loaded). Combat along the way is handled by bot AI.
    static bool HandleGotoCommand(ChatHandler* handler, float x, float y, Optional<float> zOpt)
    {
        Player* who = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!who) return false;
        Player* body = who;
        if (Unit* charm = who->GetCharm())
            if (charm->IsPlayer())
                body = charm->ToPlayer();

        float z;
        if (zOpt) z = *zOpt;
        else
        {
            // Find ground at (x, y) on the body's current map. Falls back to
            // body's current Z if the map lookup fails.
            z = body->GetMap()->GetHeight(body->GetPhaseMask(), x, y, MAX_HEIGHT);
            if (z <= INVALID_HEIGHT) z = body->GetPositionZ();
        }

        body->UpdateAllowedPositionZ(x, y, z);

        // Same scout-based GoTo as the addon's GOTO_DELTA path (see
        // PartyAddonProtocol.cpp::HandleGotoDelta for rationale): pick a
        // non-controlled bot to walk the route, promote it to group leader,
        // other bots follow via mod-playerbots' follow strategy. User
        // walks their own body manually.
        uint32 const acct = who->GetSession()->GetAccountId();
        auto const party = sPartyMgr.GetParty(acct);

        Player* scout = nullptr;
        for (auto const& m : party)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(m.guid));
            if (!p || p == body) continue;
            if (!p->IsInWorld() || p->GetMapId() != body->GetMapId()) continue;
            scout = p; break;
        }
        if (!scout)
        {
            handler->PSendSysMessage("|cffff5555[WowPsParty]|r No party member available to scout.");
            return false;
        }
        if (Group* g = body->GetGroup())
        {
            if (g->GetLeaderGUID() != scout->GetGUID() && g->IsMember(scout->GetGUID()))
            {
                g->ChangeLeader(scout->GetGUID());
                g->SendUpdate();
            }
        }
        scout->StopMoving();
        scout->GetMotionMaster()->Clear();
        // generatePath defaults true on this AC fork; omit the trailing arg
        // to avoid the ForcedMovement enum overload ambiguity.
        scout->GetMotionMaster()->MovePoint(0, x, y, z);

        uint8 followers = 0;
        for (auto const& m : party)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(m.guid));
            if (!p || p == body || p == scout || !p->IsInWorld()) continue;
            if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(p))
            {
                ai->SetMaster(scout);
                if (auto* ctx = ai->GetAiObjectContext())
                {
                    if (auto* v = ctx->GetValue<Unit*>("group leader")) v->Set(scout);
                    if (auto* v = ctx->GetValue<Unit*>("master target")) v->Set(scout);
                }
            }
            p->GetMotionMaster()->Clear();
            p->GetMotionMaster()->MoveFollow(scout, PET_FOLLOW_DIST, p->GetFollowAngle());
            ++followers;
        }

        handler->PSendSysMessage(
            "|cff66ccff[WowPsParty]|r {} scouts to ({:.0f},{:.0f}); {} bots follow + fight. Walk your body manually.",
            scout->GetName(), x, y, uint32(followers));
        return true;
    }

    // .party stop — clear any active movement target.
    static bool HandleStopCommand(ChatHandler* handler, Optional<std::string> /*unused*/)
    {
        Player* who = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!who) return false;
        Player* body = who;
        if (Unit* charm = who->GetCharm())
            if (charm->IsPlayer()) body = charm->ToPlayer();
        body->GetMotionMaster()->Clear();
        body->StopMoving();
        handler->PSendSysMessage("|cff66ccff[WowPsParty]|r Stopped.");
        return true;
    }

    // .party repair — full-repair every party member at zero cost (since they
    // share the gold pool anyway and finding 5 repair NPCs is tedious).
    static bool HandleRepairCommand(ChatHandler* handler, Optional<std::string> /*unused*/)
    {
        WorldSession* session = handler->GetSession();
        if (!session) return false;
        uint32 const acct = session->GetAccountId();
        auto const party = sPartyMgr.GetParty(acct);
        uint8 repaired = 0;
        for (auto const& m : party)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(m.guid));
            if (!p) continue;
            p->DurabilityRepairAll(false, 0.0f, false);
            ++repaired;
        }
        handler->PSendSysMessage("|cff66ccff[WowPsParty]|r Repaired {} party member(s).", uint32(repaired));
        return true;
    }

    // .party sellgrays — vendor every grey-quality item across all 5 chars'
    // bags. Loops bags only (not equipment slots).
    static bool HandleSellGraysCommand(ChatHandler* handler, Optional<std::string> /*unused*/)
    {
        WorldSession* session = handler->GetSession();
        if (!session) return false;
        uint32 const acct = session->GetAccountId();
        auto const party = sPartyMgr.GetParty(acct);

        uint32 totalCopper = 0;
        uint32 totalItems = 0;
        for (auto const& m : party)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(m.guid));
            if (!p) continue;

            auto trySell = [&](uint8 bag, uint8 slot)
            {
                Item* item = p->GetItemByPos(bag, slot);
                if (!item) return;
                ItemTemplate const* tmpl = item->GetTemplate();
                if (!tmpl) return;
                if (tmpl->Quality != ITEM_QUALITY_POOR) return;
                if (tmpl->SellPrice == 0) return;
                uint32 const proceeds = tmpl->SellPrice * item->GetCount();
                totalCopper += proceeds;
                totalItems += item->GetCount();
                p->DestroyItem(bag, slot, true);
                p->ModifyMoney(int32(proceeds));
            };

            for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
                trySell(INVENTORY_SLOT_BAG_0, i);
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            {
                Bag* bg = p->GetBagByPos(b);
                if (!bg) continue;
                for (uint32 j = 0; j < bg->GetBagSize(); ++j)
                    trySell(b, j);
            }
        }
        handler->PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Sold {} grey item(s) for {}g {}s {}c (shared pool).",
            uint32(totalItems), totalCopper / 10000, (totalCopper / 100) % 100, totalCopper % 100);
        return true;
    }

    // .party hearth — fire each party member's hearthstone (spell 8690).
    static bool HandleHearthCommand(ChatHandler* handler, Optional<std::string> /*unused*/)
    {
        WorldSession* session = handler->GetSession();
        if (!session) return false;
        uint32 const acct = session->GetAccountId();
        auto const party = sPartyMgr.GetParty(acct);
        uint8 fired = 0;
        for (auto const& m : party)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(m.guid));
            if (!p || !p->IsAlive()) continue;
            p->CastSpell(p, 8690, false);  // Hearthstone teleport
            ++fired;
        }
        handler->PSendSysMessage("|cff66ccff[WowPsParty]|r Hearthstone cast on {} member(s).", uint32(fired));
        return true;
    }

    // .party status — one-line summary per member: name, level, HP/Mana, position.
    static bool HandleStatusCommand(ChatHandler* handler, Optional<std::string> /*unused*/)
    {
        WorldSession* session = handler->GetSession();
        if (!session) return false;
        uint32 const acct = session->GetAccountId();
        auto const party = sPartyMgr.GetParty(acct);
        for (auto const& m : party)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(m.guid));
            if (!p)
            {
                handler->PSendSysMessage("  |cffaaaaff[{}]|r {} |cff888888(offline)|r", uint32(m.slot), m.name);
                continue;
            }
            uint32 hp = p->GetHealth(), hpMax = p->GetMaxHealth();
            uint32 mp = p->GetPower(POWER_MANA), mpMax = p->GetMaxPower(POWER_MANA);
            char const* aliveColor = p->IsAlive() ? "ff44ff44" : "ffff4444";
            handler->PSendSysMessage(
                "  |cffaaaaff[{}]|r {} L{} |c{}{}%%|r HP / {}%% MP, map {} ({:.0f},{:.0f})",
                uint32(m.slot), m.name, uint32(p->GetLevel()),
                aliveColor,
                hpMax ? (hp * 100 / hpMax) : 0,
                mpMax ? (mp * 100 / mpMax) : 0,
                p->GetMapId(), p->GetPositionX(), p->GetPositionY());
        }
        return true;
    }

    // .party rez [slot] — instantly resurrect a dead party member with full HP/mana.
    // No-arg form: rez every dead member of your party (useful after a wipe).
    static bool HandleRezCommand(ChatHandler* handler, Optional<uint32> slotArg)
    {
        WorldSession* session = handler->GetSession();
        if (!session)
            return false;

        uint32 const account = session->GetAccountId();
        auto const party = sPartyMgr.GetParty(account);
        if (party.empty())
        {
            handler->PSendSysMessage("|cffffcc55[WowPsParty]|r You don't have a party.");
            return true;
        }

        uint8 rezzed = 0;
        for (auto const& m : party)
        {
            if (slotArg && uint32(m.slot) != *slotArg)
                continue;

            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(m.guid);
            Player* p = ObjectAccessor::FindConnectedPlayer(guid);
            if (!p || p->IsAlive())
                continue;

            p->SpawnCorpseBones();
            p->ResurrectPlayer(1.0f);  // 100% health
            p->SetFullHealth();
            if (p->getPowerType() == POWER_MANA)
                p->SetPower(POWER_MANA, p->GetMaxPower(POWER_MANA));
            // ResurrectPlayer doesn't tear down whatever death-state
            // motion was left on the MotionMaster. Without a clean slate
            // here, MoveFollow can't actually drive the bot — they just
            // sit still and the catch-up-teleport stuck-detector pops
            // them every few seconds (= the "teleport every 5 yards"
            // pattern Kevin reported).
            p->GetMotionMaster()->Clear();
            p->GetMotionMaster()->MoveIdle();
            p->StopMoving();
            ++rezzed;
        }

        if (rezzed == 0)
        {
            handler->PSendSysMessage("|cffffcc55[WowPsParty]|r No dead party members to revive.");
        }
        else
        {
            handler->PSendSysMessage("|cff66ccff[WowPsParty]|r Revived {} party member(s).", uint32(rezzed));
        }
        return true;
    }

    // .party unswap — drop any active possess and return to your real body.
    static bool HandleUnswapCommand(ChatHandler* handler, Optional<std::string> /*unused*/)
    {
        Player* requestor = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!requestor)
            return false;

        if (sPartyMgr.Unswap(requestor))
        {
            handler->PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Released. You're back in your own body.");
        }
        else
        {
            handler->PSendSysMessage(
                "|cffffcc55[WowPsParty]|r Nothing to release — you weren't possessing anyone.");
        }
        return true;
    }

    // .party slot <0-4> — mark which slot becomes the active-on-login default
    static bool HandleSlotCommand(ChatHandler* handler, Optional<uint32> slotArg)
    {
        WorldSession* session = handler->GetSession();
        if (!session || !slotArg)
        {
            handler->PSendSysMessage(
                "|cffff5555[WowPsParty]|r Usage: |cffffff00.party slot <0-4>|r");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const slot = *slotArg;
        if (slot >= WowPsParty::PARTY_SIZE)
        {
            handler->PSendSysMessage(
                "|cffff5555[WowPsParty]|r Slot must be 0-4 (got {}).", slot);
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const accountId = session->GetAccountId();
        QueryResult exists = CharacterDatabase.Query(
            "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
            accountId, slot);
        if (!exists)
        {
            handler->PSendSysMessage(
                "|cffff5555[WowPsParty]|r Slot {} is empty.", slot);
            handler->SetSentErrorMessage(true);
            return false;
        }

        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append(
            "UPDATE `account_party` SET `is_active_on_login` = 0 WHERE `account` = {}",
            accountId);
        tx->Append(
            "UPDATE `account_party` SET `is_active_on_login` = 1 "
            "WHERE `account` = {} AND `slot` = {}",
            accountId, slot);
        CharacterDatabase.CommitTransaction(tx);

        handler->PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Slot {} marked active-on-login.", slot);
        return true;
    }

    // .wowps_admin bootstrap <account>
    // Session-independent: runs the same logic as the WPSP BOOTSTRAP_PARTY
    // handler for the named account. Drivable from SOAP/console with no
    // player session required — that's the whole point of this command
    // (lets the test harness in scripts/test-protocol.ps1 verify the
    // bootstrap path end-to-end without needing a logged-in client).
    static bool HandleAdminBootstrapCommand(ChatHandler* handler, uint32 account)
    {
        std::vector<std::string> messages;
        uint32 const enrolled = WowPsParty::BootstrapPartyForAccount(account, messages);
        for (auto const& m : messages)
            handler->PSendSysMessage("[wowps_admin] {}", m);
        handler->PSendSysMessage("[wowps_admin] result: enrolled={}", enrolled);
        return true;
    }

    // .wowps_admin run_handler_tests
    // Triggers the in-process handler test runner. Spawns 3 mod-playerbots
    // bots (if not already in world), drives every WPSP handler against them,
    // writes results to handler-test-results.log + the AC log.
    static bool HandleAdminRunHandlerTestsCommand(ChatHandler* handler, Optional<std::string> /*ignored*/)
    {
        handler->PSendSysMessage("[wowps_admin] kicking off handler test run...");
        WowPsParty::RunHandlerTests();
        handler->PSendSysMessage("[wowps_admin] see handler-test-results.log for output");
        return true;
    }

    // .wowps_admin verify_party <account>
    // Prints the current account_party row count + slot/guid pairs for the
    // account. Used by the test harness to assert state.
    static bool HandleAdminVerifyPartyCommand(ChatHandler* handler, uint32 account)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT slot, guid FROM account_party WHERE account = {} ORDER BY slot",
            account);
        if (!q)
        {
            handler->PSendSysMessage("[wowps_admin] account={} rows=0", account);
            return true;
        }
        uint32 count = 0;
        do
        {
            Field* f = q->Fetch();
            handler->PSendSysMessage("[wowps_admin] account={} slot={} guid={}",
                                     account, uint32(f[0].Get<uint8>()), f[1].Get<uint32>());
            ++count;
        } while (q->NextRow());
        handler->PSendSysMessage("[wowps_admin] account={} rows={}", account, count);
        return true;
    }
};

void AddPartyCommandScripts()
{
    new party_commandscript();
}
