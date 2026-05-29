/*
 * WowPs Party-of-5 mod — PartyMgr implementation
 */

#include "PartyMgr.h"
#include "PartyFollow.h"

#include "Chat.h"
#include "CharacterCache.h"
#include "CharmInfo.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LootMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Unit.h"
#include "WorldSession.h"

// mod-playerbots (AC's modules build adds every subdir of every module to the
// include path, so no `Bot/` prefix is needed)
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "AiObjectContext.h"
#include "Value.h"

#include "MotionMaster.h"
#include "Pet.h"  // PET_FOLLOW_DIST

#include "WorldSession.h"
#include "WorldPacket.h"
#include "Opcodes.h"

#include <mutex>
#include <unordered_map>

// Forward declarations of helpers defined in PartyAddonProtocol.cpp / PartyRotation.cpp
namespace WowPsParty
{
    void SendRosterTo(Player* player);
    void RotationCacheRefreshFromDB(uint32 guid);
    void TargetModeRefreshFromDB(uint32 guidLow);
    void LeadDungeonRefreshFromDB(uint32 guidLow);
    void PushControlledLoadoutTo(Player* requester, int slot);
}

namespace WowPsParty
{
    PartyMgr& PartyMgr::Instance()
    {
        static PartyMgr instance;
        return instance;
    }

    // ----- per-account feature toggles ---------------------------------------
    static std::unordered_map<uint32, PartySettings> g_accountSettings;
    static std::mutex                                g_settingsMutex;

    void EnsureSettingsTable()
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `party_account_settings` ("
            "`account` INT UNSIGNED NOT NULL PRIMARY KEY, "
            "`spawn_companions` TINYINT NOT NULL DEFAULT 1, "
            "`shared_inventory` TINYINT NOT NULL DEFAULT 1, "
            "`shared_gear` TINYINT NOT NULL DEFAULT 1, "
            "`shared_progression` TINYINT NOT NULL DEFAULT 1)");
    }

    void AccountSettingsRefreshFromDB(uint32 account)
    {
        PartySettings s;  // all-ON default
        QueryResult q = CharacterDatabase.Query(
            "SELECT `spawn_companions`,`shared_inventory`,`shared_gear`,"
            "`shared_progression` FROM `party_account_settings` WHERE `account` = {}",
            account);
        if (q)
        {
            Field* f = q->Fetch();
            s.spawnCompanions   = f[0].Get<uint8>() != 0;
            s.sharedInventory   = f[1].Get<uint8>() != 0;
            s.sharedGear        = f[2].Get<uint8>() != 0;
            s.sharedProgression = f[3].Get<uint8>() != 0;
        }
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        g_accountSettings[account] = s;
    }

    PartySettings GetAccountSettings(uint32 account)
    {
        {
            std::lock_guard<std::mutex> lock(g_settingsMutex);
            auto it = g_accountSettings.find(account);
            if (it != g_accountSettings.end()) return it->second;
        }
        AccountSettingsRefreshFromDB(account);
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        return g_accountSettings[account];
    }

    void SetAccountSetting(uint32 account, std::string const& key, bool value)
    {
        static const std::unordered_map<std::string, int> cols = {
            {"spawn_companions", 0}, {"shared_inventory", 1},
            {"shared_gear", 2}, {"shared_progression", 3} };
        if (cols.find(key) == cols.end()) return;
        uint8 const v = value ? 1 : 0;
        // Upsert the single column; unspecified columns keep their table
        // default (1) on insert, which matches "all ON by default".
        CharacterDatabase.Execute(
            "INSERT INTO `party_account_settings` (`account`, `{}`) VALUES ({}, {}) "
            "ON DUPLICATE KEY UPDATE `{}` = {}",
            key, account, uint32(v), key, uint32(v));
        // Refresh the cached struct field.
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        PartySettings& s = g_accountSettings[account];
        if      (key == "spawn_companions")   s.spawnCompanions   = value;
        else if (key == "shared_inventory")   s.sharedInventory   = value;
        else if (key == "shared_gear")        s.sharedGear        = value;
        else if (key == "shared_progression") s.sharedProgression = value;
    }

    static uint32 FetchAccountForGuid(uint32 guid)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `account` FROM `characters` WHERE `guid` = {}", guid);
        if (!q)
            return 0;
        return q->Fetch()[0].Get<uint32>();
    }

    static std::vector<std::pair<uint8 /*slot*/, uint32 /*guid*/>> FetchPartyRows(uint32 accountId)
    {
        std::vector<std::pair<uint8, uint32>> rows;
        QueryResult q = CharacterDatabase.Query(
            "SELECT `slot`, `guid` FROM `account_party` WHERE `account` = {} ORDER BY `slot`", accountId);
        if (!q)
            return rows;
        do
        {
            Field* f = q->Fetch();
            rows.emplace_back(f[0].Get<uint8>(), f[1].Get<uint32>());
        } while (q->NextRow());
        return rows;
    }

    EnrollResult PartyMgr::Enroll(Player* requestor, uint32 targetGuid, std::string const& targetName)
    {
        if (!requestor || !requestor->GetSession())
            return EnrollResult::DatabaseError;

        uint32 const requestorAccount = requestor->GetSession()->GetAccountId();

        // Default: enroll the requestor's own currently-logged-in character.
        if (targetGuid == 0)
            targetGuid = requestor->GetGUID().GetCounter();

        // Verify the target character belongs to the requestor's account.
        uint32 const targetAccount = FetchAccountForGuid(targetGuid);
        if (targetAccount == 0)
            return EnrollResult::TargetNotFound;
        if (targetAccount != requestorAccount)
            return EnrollResult::TakenByAnotherAccount;

        // Already enrolled?
        QueryResult existing = CharacterDatabase.Query(
            "SELECT `account`, `slot` FROM `account_party` WHERE `guid` = {}", targetGuid);
        if (existing)
        {
            uint32 const ownerAccount = existing->Fetch()[0].Get<uint32>();
            if (ownerAccount == requestorAccount)
                return EnrollResult::AlreadyEnrolled;
            return EnrollResult::TakenByAnotherAccount;
        }

        // Find next free slot 0..4 in this account.
        auto const rows = FetchPartyRows(requestorAccount);
        if (rows.size() >= PARTY_SIZE)
            return EnrollResult::PartyFull;

        uint8 nextSlot = 0;
        for (uint8 candidate = 0; candidate < PARTY_SIZE; ++candidate)
        {
            bool taken = false;
            for (auto const& row : rows)
            {
                if (row.first == candidate) { taken = true; break; }
            }
            if (!taken) { nextSlot = candidate; break; }
        }

        // Transactional insert: account_party row + characters.party_slot column.
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append(
            "INSERT INTO `account_party` (`account`, `slot`, `guid`, `is_active_on_login`) "
            "VALUES ({}, {}, {}, {})",
            requestorAccount, nextSlot, targetGuid,
            (nextSlot == 0 ? 1u : 0u));
        tx->Append(
            "UPDATE `characters` SET `party_slot` = {} WHERE `guid` = {}",
            nextSlot, targetGuid);
        CharacterDatabase.CommitTransaction(tx);

        LOG_INFO("module",
                 "[WowPsParty] enroll: account={} guid={} name={} slot={}",
                 requestorAccount, targetGuid, targetName, nextSlot);

        return EnrollResult::Ok;
    }

    bool PartyMgr::Leave(Player* requestor)
    {
        if (!requestor || !requestor->GetSession())
            return false;

        uint32 const guid = requestor->GetGUID().GetCounter();
        uint32 const account = requestor->GetSession()->GetAccountId();

        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append(
            "DELETE FROM `account_party` WHERE `account` = {} AND `guid` = {}",
            account, guid);
        tx->Append(
            "UPDATE `characters` SET `party_slot` = NULL WHERE `guid` = {}",
            guid);
        // Also clear any loadout for this character.
        tx->Append(
            "DELETE FROM `party_loadout` WHERE `guid` = {}", guid);
        CharacterDatabase.CommitTransaction(tx);

        LOG_INFO("module", "[WowPsParty] leave: account={} guid={}", account, guid);
        return true;
    }

    std::vector<PartyMember> PartyMgr::GetParty(uint32 accountId)
    {
        std::vector<PartyMember> out;

        QueryResult q = CharacterDatabase.Query(
            "SELECT ap.`slot`, ap.`guid`, c.`name`, c.`class`, c.`level` "
            "FROM `account_party` ap "
            "JOIN `characters` c ON c.`guid` = ap.`guid` "
            "WHERE ap.`account` = {} ORDER BY ap.`slot`", accountId);
        if (!q)
            return out;

        do
        {
            Field* f = q->Fetch();
            PartyMember m;
            m.slot    = f[0].Get<uint8>();
            m.guid    = f[1].Get<uint32>();
            m.name    = f[2].Get<std::string>();
            m.classId = f[3].Get<uint8>();
            m.level   = f[4].Get<uint8>();
            m.online  = ObjectAccessor::FindPlayerByLowGUID(m.guid) != nullptr;
            out.push_back(std::move(m));
        } while (q->NextRow());

        return out;
    }

    std::optional<uint8> PartyMgr::GetSlotForGuid(uint32 guid)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `party_slot` FROM `characters` WHERE `guid` = {}", guid);
        if (!q)
            return std::nullopt;

        Field* f = q->Fetch();
        if (f[0].IsNull())
            return std::nullopt;
        return f[0].Get<uint8>();
    }

    void PartyMgr::OnActiveLogin(Player* active)
    {
        if (!active || !active->GetSession())
            return;

        uint32 const account = active->GetSession()->GetAccountId();
        uint32 const activeGuid = active->GetGUID().GetCounter();

        // Load this account's feature toggles up front (cached). Solo mode
        // (companions off) keeps the enrolled roster + rotations/talents on
        // disk — we just don't spawn the bots or form the party group.
        AccountSettingsRefreshFromDB(account);
        if (!GetAccountSettings(account).spawnCompanions)
        {
            LOG_INFO("module",
                "[WowPsParty] OnActiveLogin: account={} companions OFF (solo) — "
                "not spawning party.", account);
            return;
        }

        auto const rows = FetchPartyRows(account);
        if (rows.empty())
            return;  // not enrolled — nothing to spawn

        // mod-playerbots' PlayerScript::OnLogin creates the PlayerbotMgr. AC
        // dispatches PlayerScript hooks in REGISTRATION order, not
        // alphabetical (earlier comment was wrong) -- and our PartyHooks
        // script registers first, so on first login `botMgr` may be null
        // here. Defer the bot-spawn body by 1s via the player's event queue
        // so mod-playerbots has a chance to wire up its manager.
        ObjectGuid const activeObjGuid2 = active->GetGUID();
        active->m_Events.AddEventAtOffset([activeObjGuid2, account, activeGuid, rows]()
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(activeObjGuid2);
            if (!p) return;
            PlayerbotMgr* botMgr = sPlayerbotsMgr.GetPlayerbotMgr(p);
            if (!botMgr)
            {
                LOG_WARN("module",
                         "[WowPsParty] OnActiveLogin (deferred): still no PlayerbotMgr "
                         "for guid={}; mod-playerbots is genuinely missing. Idle party "
                         "members will NOT spawn. Re-login should fix.", activeGuid);
                return;
            }
            // WoW party is capped at 5 members. Active player counts as 1,
            // so spawn at most 4 bots. Extra enrolled chars beyond that just
            // sit out this session.
            uint8 spawned = 0;
            for (auto const& row : rows)
            {
                uint32 const guid = row.second;
                RotationCacheRefreshFromDB(guid);
                TargetModeRefreshFromDB(guid);
                LeadDungeonRefreshFromDB(guid);
                if (guid == activeGuid) continue;
                if (spawned >= 4) break;
                ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
                botMgr->AddPlayerBot(og, account);
                ++spawned;
            }
            LOG_INFO("module",
                "[WowPsParty] OnActiveLogin (deferred): spawned {} idle bot(s)", spawned);

            // Install follow directives now that the bots are loaded. This
            // ALSO acts as the "is a party-of-5 bot" gate used by the
            // patched PlayerbotAI::UpdateAI — without it, the AI doesn't
            // know to suppress the default rotation and bots fall back to
            // their class strategy (priests Smite, mages Fireball, etc.).
            WowPsParty::SetActiveFollowers(account,
                ObjectGuid::Create<HighGuid::Player>(activeGuid));
        }, std::chrono::seconds(1));

        ObjectGuid const activeObjGuid = active->GetGUID();

        // Ensure all 5 are in one Group with the active as leader and FFA
        // loot. Run after a short delay so mod-playerbots' bot-spawn callbacks
        // have settled into the world.
        active->m_Events.AddEventAtOffset([activeObjGuid]()
        {
            Player* leader = ObjectAccessor::FindConnectedPlayer(activeObjGuid);
            if (!leader || !leader->GetSession()) return;
            uint32 const account = leader->GetSession()->GetAccountId();

            Group* group = leader->GetGroup();
            if (!group)
            {
                group = new Group();
                if (!group->Create(leader))
                {
                    delete group;
                    return;
                }
                sGroupMgr->AddGroup(group);
            }
            group->SetLootMethod(FREE_FOR_ALL);

            // Walk the party and invite every member that isn't already in.
            auto const rows = FetchPartyRows(account);
            for (auto const& row : rows)
            {
                ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(row.second);
                if (og == leader->GetGUID()) continue;
                Player* mem = ObjectAccessor::FindConnectedPlayer(og);
                if (!mem || mem->GetGroup() == group) continue;
                if (mem->GetGroup())
                    mem->RemoveFromGroup();
                group->AddMember(mem);
            }

            // Enable auto-loot on every bot in the party so kills get looted
            // without the user having to right-click every corpse. mod-playerbots'
            // "+loot" strategy in BOT_STATE_NON_COMBAT triggers LootAction on
            // nearby unlooted corpses; "+gather" picks up herbs/ore. Idempotent —
            // ChangeStrategy on an already-enabled strategy is a no-op.
            for (auto const& row : rows)
            {
                ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(row.second);
                Player* mem = ObjectAccessor::FindConnectedPlayer(og);
                if (!mem) continue;
                if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(mem))
                {
                    ai->ChangeStrategy("+loot", BOT_STATE_NON_COMBAT);
                    ai->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);
                }
            }
        }, std::chrono::seconds(6));

        LOG_INFO("module",
                 "[WowPsParty] OnActiveLogin: account={} active_guid={} -- bot spawn deferred by 1s",
                 account, activeGuid);
    }

}
