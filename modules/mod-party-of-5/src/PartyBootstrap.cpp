/*
 * WowPs Party-of-5 mod — bootstrap scripts
 *
 *   PartyBootstrapWorldScript  — loads module config on startup, prints banner.
 *   PartyBootstrapPlayerScript — on each player login, reports party-slot status
 *                                and, if enrolled, schedules a delayed call into
 *                                PartyMgr::OnActiveLogin to spawn the rest of the
 *                                account's party as mod-playerbots-controlled bots.
 *
 * The 5-second deferral on the spawn is intentional: AC PlayerScripts dispatch in
 * registration order, and our hook may run before mod-playerbots has constructed
 * the per-master PlayerbotMgr. Waiting one world tick is enough; 5s is generous.
 * The lambda captures ObjectGuid (POD, safe across logout) rather than Player*.
 */

#include "PartyMgr.h"
#include "PartyFollow.h"
#include "PartyPath.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <chrono>

namespace WowPsParty
{
    void PushControlledLoadoutTo(Player* requester, int slot);  // defined in PartyAddonProtocol.cpp
}

namespace WowPsParty
{
    struct ModuleConfig
    {
        bool enabled     = true;
        bool log_verbose = true;

        void Load()
        {
            enabled     = sConfigMgr->GetOption<bool>("WowPsParty.Enable",     true);
            log_verbose = sConfigMgr->GetOption<bool>("WowPsParty.LogVerbose", true);
        }
    };

    static ModuleConfig g_config;

    bool IsEnabled()     { return g_config.enabled; }
    bool IsLogVerbose()  { return g_config.log_verbose; }
}

class PartyBootstrapWorldScript : public WorldScript
{
public:
    PartyBootstrapWorldScript() : WorldScript("PartyBootstrapWorldScript", {
        WORLDHOOK_ON_BEFORE_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        WowPsParty::g_config.Load();
    }

    void OnStartup() override
    {
        if (!WowPsParty::g_config.enabled)
        {
            LOG_INFO("module", "[WowPsParty] mod-party-of-5 compiled in but DISABLED (WowPsParty.Enable=0)");
            return;
        }
        LOG_INFO("module", "[WowPsParty] mod-party-of-5 active. Verbose logging: {}",
                 WowPsParty::g_config.log_verbose ? "on" : "off");

        // Install the dedicated follow ticker. This is what makes party
        // members tail the controlled body across swaps -- bypasses
        // mod-playerbots' AI cache that was getting stuck on the original
        // session player. See PartyFollow.h for the rationale.
        WowPsParty::InstallFollowTicker();

        // Per-account feature toggles (solo / Po5). Create the table on the
        // live DB if a migration hasn't run yet.
        WowPsParty::EnsureSettingsTable();
    }
};

class PartyBootstrapPlayerScript : public PlayerScript
{
public:
    PartyBootstrapPlayerScript() : PlayerScript("PartyBootstrapPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_CREATE,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_DELETE
    }) { }

    // When a character is deleted, purge its party enrollment + saved loadout so
    // the slot frees up immediately. Without this the orphan account_party row
    // keeps counting toward the 5-slot cap (blocking new enrollments), the login
    // path keeps trying — and failing — to spawn the deleted guid as a bot
    // ("no PlayerbotMgr"), and the group-build purge can evict the live hero as
    // a "stale henchman" because the roster no longer matches reality.
    void OnPlayerDelete(ObjectGuid guid, uint32 accountId) override
    {
        if (!WowPsParty::IsEnabled())
            return;

        uint32 const low = guid.GetCounter();

        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append("DELETE FROM `account_party` WHERE `guid` = {}", low);
        tx->Append("DELETE FROM `party_loadout` WHERE `guid` = {}", low);
        CharacterDatabase.CommitTransaction(tx);

        LOG_INFO("module",
                 "[WowPsParty] char delete: purged enrollment + loadout for "
                 "guid={} (account={})", low, accountId);
    }

    // Phase-6 hardening: when the session player logs out while possessing a
    // party member, release the possess first so we don't leave a charm aura
    // dangling on the bot. Without this the bot could stay possessed-but-
    // ownerless until the next swap or restart, with broken AI ticks (the
    // POSSESSED flag pauses our patched UpdateAI but nothing reactivates it).
    void OnPlayerLogout(Player* player) override
    {
        if (!WowPsParty::IsEnabled() || !player)
            return;
        if (Unit* charm = player->GetCharm())
        {
            LOG_INFO("module",
                     "[WowPsParty] logout-release: guid={} releasing charm of guid={}",
                     player->GetGUID().GetCounter(), charm->GetGUID().GetCounter());
            charm->RemoveCharmedBy(player);
        }

        // Drop any in-progress path recording so the char doesn't persist
        // ghost mode (fly + 5x speed) and relog stuck in the air.
        WowPsParty::CancelPathRecording(player);

        // Henchmen are temporary — release them (log the random-pool bots out
        // + drop their follow directives) when the leader logs out.
        WowPsParty::DismissAllHenchmen(player);
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!WowPsParty::IsEnabled() || !player)
            return;

        // Skip mod-playerbots spawned bots. OnPlayerLogin fires for every
        // login including bot spawns; we only want the real human session
        // player to trigger OnActiveLogin (which spawns the rest of the
        // party). PlayerbotAI is only attached to spawned bots.
        if (sPlayerbotsMgr.GetPlayerbotAI(player))
            return;

        // Backfill abilities the human's own char missed. Quest skills FIRST
        // (Bear/Cat Form, stances, totems, pets, mounts) so any form-gated trainer
        // ability is then learnable, then EVERY trainer spell for its level —
        // because a char that leveled past a form (e.g. dinged 20 / Cat Form)
        // before this feature existed got the form on a prior login but never the
        // trainer-taught cat abilities (Claw / Prowl / Rip). The ding hook covers
        // the live case; this covers everything earned earlier. Bots get both on
        // the maintenance/ding tick; the controlled char isn't a bot, so catch it
        // here at login.
        WowPsParty::LearnClassQuestSkills(player);
        WowPsParty::LearnAllClassSpells(player);
        WowPsParty::LearnAllWeaponSkills(player);   // every class-usable weapon proficiency — no weapon-master trip

        // Cache the controlled character's party role (enrolled or solo/per-char) so
        // the human-tank wait-gate reads it even for a non-enrolled solo character.
        WowPsParty::SetLeaderRoleForChar(player->GetSession()->GetAccountId(), player->GetGUID());

        uint32 const guid = player->GetGUID().GetCounter();
        std::optional<uint8> const slot = sPartyMgr.GetSlotForGuid(guid);

        if (WowPsParty::IsLogVerbose())
        {
            LOG_INFO("module", "[WowPsParty] login: guid={} name={} party_slot={}",
                     guid, player->GetName(),
                     slot ? std::to_string(*slot) : std::string("-"));
        }

        uint32 const accountId = player->GetSession()->GetAccountId();

        // SOLO MODE: companions disabled means no party spawns (OnActiveLogin
        // below no-ops on the same flag), so suppress EVERY party-of-5 login
        // message — the "your party will spawn shortly" line and the rotation
        // reminder. The account just plays as a normal solo character.
        bool const partyActive = WowPsParty::GetAccountSettings(accountId).spawnCompanions;

        // Unenrolled active player is fine — they just play as the "leader"
        // and the enrolled chars spawn as their party (up to 4 of them).
        // Tell the player what's happening so it's not mysterious.
        if (partyActive)
        {
            if (slot)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff66ccff[WowPsParty]|r Welcome back. You are slot {} of your account's party. "
                    "Your other party members will spawn beside you shortly.",
                    uint32(*slot));
            }
            else
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff66ccff[WowPsParty]|r You aren't enrolled in the party, but your "
                    "enrolled characters will still join you as bots. Press |cffffff00O|r "
                    "(or rebind |cffffff00Open party roster|r) to manage the roster.");
            }
        }

        // Rotation-setup reminder: party members without a configured rotation
        // will ONLY auto-attack what you attack — they will not cast spells on
        // their own. Nudge the player toward the editor so they don't expect
        // the bots to "just play their class" out of the box. (Solo: no bots,
        // so no reminder.)
        if (partyActive)
        {
            QueryResult q = CharacterDatabase.Query(
                "SELECT ap.guid, COALESCE(pl.priority_actions_json, '') "
                "FROM `account_party` ap "
                "LEFT JOIN `party_loadout` pl ON pl.guid = ap.guid "
                "WHERE ap.account = {}", accountId);
            uint32 missing = 0;
            if (q)
            {
                do
                {
                    uint32 const memberGuid = q->Fetch()[0].Get<uint32>();
                    std::string const dsl   = q->Fetch()[1].Get<std::string>();
                    if (memberGuid == guid) continue;  // skip the body the user drives
                    if (dsl.empty()) ++missing;
                } while (q->NextRow());
            }
            if (missing > 0)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffffaa00[WowPsParty]|r Reminder: {} of your companions have no rotation set. "
                    "Until you configure one, they will only auto-attack your target — they will not cast spells.",
                    missing);
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffffaa00[WowPsParty]|r Press |cffffff00Y|r to open the rotation editor "
                    "(rebind under Esc -> Key Bindings -> WowPsParty).");
            }
        }

        // Persist this session's active char so picking from char-select
        // next time defaults back here. Only updates is_active_on_login
        // when the player is actually enrolled.
        if (slot)
        {
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append("UPDATE `account_party` SET `is_active_on_login` = 0 WHERE `account` = {}", accountId);
            tx->Append("UPDATE `account_party` SET `is_active_on_login` = 1 "
                       "WHERE `account` = {} AND `guid` = {}", accountId, guid);
            CharacterDatabase.CommitTransaction(tx);
        }

        // Defer the actual spawn so mod-playerbots has had a tick to initialize
        // this session's PlayerbotMgr. Capture by GUID — Player* may be gone by
        // then if the user disconnects in the meantime.
        ObjectGuid const objGuid = player->GetGUID();
        int const initialSlot = slot ? int(*slot) : -1;
        player->m_Events.AddEventAtOffset([objGuid, initialSlot]()
        {
            if (Player* alive = ObjectAccessor::FindConnectedPlayer(objGuid))
            {
                sPartyMgr.OnActiveLogin(alive);
                // Push the controlled-body loadout only if the active player
                // is themselves enrolled — otherwise there's no slot mapping.
                if (initialSlot >= 0)
                    WowPsParty::PushControlledLoadoutTo(alive, initialSlot);
            }
        }, std::chrono::seconds(5));
    }

    // Auto-enroll a newly-created character into the next free party slot, as long
    // as the account has fewer than 5 slots filled. Saves the manual `.party enroll`
    // step for fresh accounts building out their party from scratch.
    void OnPlayerCreate(Player* player) override
    {
        if (!WowPsParty::IsEnabled() || !player || !player->GetSession())
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        std::string const name = player->GetName();

        WowPsParty::EnrollResult const r = sPartyMgr.Enroll(player, guid, name);
        if (r == WowPsParty::EnrollResult::Ok && WowPsParty::IsLogVerbose())
        {
            LOG_INFO("module", "[WowPsParty] auto-enrolled new character: guid={} name={}",
                     guid, name);
        }
        // PartyFull / AlreadyEnrolled are non-events here; ignored silently so we
        // don't spam new players who already have a full party.
    }
};

void AddPartyBootstrapScripts()
{
    new PartyBootstrapWorldScript();
    new PartyBootstrapPlayerScript();
}
