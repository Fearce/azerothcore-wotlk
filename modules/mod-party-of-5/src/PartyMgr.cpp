/*
 * WowPs Party-of-5 mod — PartyMgr implementation
 */

#include "PartyMgr.h"
#include "PartyFollow.h"
#include "PartyRotation.h"

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

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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

    // ----- Henchmen ----------------------------------------------------------

    // Cached CSV of random-bot account ids ("rndbot*"), read once from the
    // auth DB. Used to scope the henchman candidate pool to random-pool chars.
    static std::string RndbotAccountCsv()
    {
        static std::string cached;
        if (!cached.empty()) return cached;   // only cache a non-empty result
        QueryResult q = LoginDatabase.Query(
            "SELECT `id` FROM `account` WHERE `username` LIKE 'rndbot%'");
        std::ostringstream csv;
        uint32 n = 0;
        bool first = true;
        if (q) do {
            if (!first) csv << ',';
            first = false;
            csv << q->Fetch()[0].Get<uint32>();
            ++n;
        } while (q->NextRow());
        cached = csv.str();
        LOG_INFO("module",
            "[WowPsParty Henchmen] random-bot account pool: {} accounts", n);
        return cached;   // empty stays uncached → retried next call
    }

    // class id -> default henchman role.
    static char const* ClassDefaultRole(uint8 cls)
    {
        switch (cls)
        {
            case 1:  return "tank";    // Warrior
            case 2:  return "tank";    // Paladin (prot-ish)
            case 6:  return "tank";    // Death Knight
            case 5:  return "healer";  // Priest
            case 7:  return "healer";  // Shaman
            case 11: return "healer";  // Druid
            default: return "dps";     // Hunter/Rogue/Mage/Warlock
        }
    }

    uint32 HenchmanHireCost(uint8 level)
    {
        // ~2.5 silver per level — "a little money" (L18 ≈ 45s, L80 = 2g).
        return uint32(level) * 250u;
    }

    // Canonical per-class starter rotation (spell NAMES, so the engine picks
    // the highest known rank at any level). Shared by `.party preset` and
    // henchman hire. Keep in sync — this is the single source.
    std::string DefaultRotationForClass(uint8 cls)
    {
        switch (cls)
        {
            case 1:  return "self_health<40|cast_self:Battle Shout|80;has_target|cast:Heroic Strike|50;has_target|cast:Rend|30";
            case 2:  return "self_health<35|cast_self:Holy Light|90;always|cast_self:Devotion Aura|70;has_target|cast:Judgement of Light|40";
            case 3:  return "has_target|cast:Serpent Sting|60;has_target|cast:Arcane Shot|40;has_target|cast:Auto Shot|20";
            case 4:  return "out_of_combat|cast_self:Stealth|95;has_target|cast:Sinister Strike|50";
            case 5:  return "party_lowest_health<55|cast_party_lowest:Lesser Heal|95;has_target|cast:Smite|30;always|buff_self:Power Word: Fortitude|70";
            case 6:  return "self_health<35|cast_self:Death Strike|90;has_target|cast:Plague Strike|60;has_target|cast:Blood Strike|40";
            case 7:  return "party_lowest_health<40|cast_party_lowest:Healing Wave|90;has_target|cast:Lightning Bolt|50;always|buff_self:Lightning Shield|70";
            case 8:  return "self_health<35|cast_self:Frost Nova|95;has_target|cast:Frostbolt|50;has_target|cast:Fireball|40";
            case 9:  return "has_target|cast:Corruption|60;has_target|cast:Shadow Bolt|40;self_health<30|cast:Drain Life|95";
            case 11: return "party_lowest_health<40|cast_party_lowest:Rejuvenation|90;has_target|cast:Wrath|50;has_target|cast:Moonfire|30";
            default: return "";
        }
    }

    // Query offline random-pool chars of the given classes near `level`.
    static void QueryHenchCandidates(std::string const& acctCsv,
        std::string const& classCsv, uint8 lo, uint8 hi, uint8 level,
        uint32 limit, std::vector<HenchmanCandidate>& out)
    {
        if (acctCsv.empty()) return;
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid`,`name`,`class`,`level` FROM `characters` "
            "WHERE `account` IN ({}) AND `online` = 0 AND `class` IN ({}) "
            "AND `level` BETWEEN {} AND {} "
            "ORDER BY ABS(CAST(`level` AS SIGNED) - {}) ASC, RAND() LIMIT {}",
            acctCsv, classCsv, uint32(lo), uint32(hi), uint32(level), limit);
        if (!q) return;
        do {
            Field* f = q->Fetch();
            HenchmanCandidate c;
            c.guid  = f[0].Get<uint32>();
            c.name  = f[1].Get<std::string>();
            c.cls   = f[2].Get<uint8>();
            c.level = f[3].Get<uint8>();
            c.role  = ClassDefaultRole(c.cls);
            out.push_back(std::move(c));
        } while (q->NextRow());
    }

    std::vector<HenchmanCandidate> BuildHenchmanCandidates(Player* requester)
    {
        std::vector<HenchmanCandidate> out;
        if (!requester) return out;
        std::string const acctCsv = RndbotAccountCsv();
        if (acctCsv.empty()) return out;

        uint8 const L  = requester->GetLevel();
        uint8 const lo = (L >= 80) ? 80 : uint8(std::max(1, int(L) - 2));
        uint8 const hi = (L >= 80) ? 80 : uint8(std::min(80, int(L) + 2));

        // 2 tanks (Warr/Pala/DK/Druid), 2 healers (Priest/Pala/Shaman/Druid),
        // 6 dps (any class). Exact-band first; the ORDER BY closeness keeps
        // them near the player's level.
        QueryHenchCandidates(acctCsv, "1,2,6,11", lo, hi, L, 2, out);
        QueryHenchCandidates(acctCsv, "2,5,7,11", lo, hi, L, 2, out);
        QueryHenchCandidates(acctCsv, "1,2,3,4,5,6,7,8,9,11", lo, hi, L, 6, out);

        // Force the role label per the slot it was drawn for (the same char
        // could appear via two class sets; dedupe by guid keeping first role).
        std::vector<HenchmanCandidate> deduped;
        for (auto& c : out)
        {
            bool seen = false;
            for (auto const& d : deduped) if (d.guid == c.guid) { seen = true; break; }
            if (!seen) deduped.push_back(c);
        }
        return deduped;
    }

    // Set the group's loot rule based on whether any henchman is present:
    // henchman in party → GROUP_LOOT (rolls), else FREE_FOR_ALL (premade).
    static void UpdateGroupLootForHenchmen(Player* leader)
    {
        if (!leader) return;
        Group* g = leader->GetGroup();
        if (!g) return;
        bool const hasHench = WowPsParty::CountHenchmenFor(leader->GetGUID()) > 0;
        g->SetLootMethod(hasHench ? GROUP_LOOT : FREE_FOR_ALL);
        g->SendUpdate();
        LOG_INFO("module", "[WowPsParty Henchmen] loot method -> {} (henchmen present={})",
                 hasHench ? "GROUP_LOOT" : "FREE_FOR_ALL", hasHench);
    }

    bool HireHenchman(Player* requester, uint32 candidateGuid,
                      std::string const& role, std::string& outMsg)
    {
        if (!requester || !requester->GetSession())
        { outMsg = "No session."; return false; }
        uint32 const account = requester->GetSession()->GetAccountId();

        // Validate candidate is an offline random-pool char.
        std::string const acctCsv = RndbotAccountCsv();
        QueryResult q = CharacterDatabase.Query(
            "SELECT `level`,`online`,`account`,`class` FROM `characters` WHERE `guid` = {}",
            candidateGuid);
        if (!q)
        {
            outMsg = "Henchman not found.";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: not in DB", candidateGuid);
            return false;
        }
        Field* f = q->Fetch();
        uint8 const level   = f[0].Get<uint8>();
        bool  const online  = f[1].Get<uint8>() != 0;
        uint8 const cls     = f[3].Get<uint8>();
        if (online)
        {
            outMsg = "That henchman is busy — pick another (Refresh the list).";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: already online", candidateGuid);
            return false;
        }

        // Party-space cap: leader + 4 companions max. Count current followers
        // (alts + henchmen) from the directive registry — works even before
        // the WoW group object exists (solo + first henchman).
        uint32 const followers = WowPsParty::CountFollowersFor(requester->GetGUID());
        if (followers >= 4)
        {
            outMsg = "Your party is full (4 companions).";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: party full (followers={})",
                     candidateGuid, followers);
            return false;
        }

        // Gold check + deduct (after all synchronous validation).
        uint32 const cost = HenchmanHireCost(level);
        if (requester->GetMoney() < cost)
        {
            outMsg = "Not enough gold to hire.";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: gold {} < cost {}",
                     candidateGuid, requester->GetMoney(), cost);
            return false;
        }

        PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(requester);
        if (!mgr)
        {
            outMsg = "Bot manager not ready — try again in a moment.";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: no PlayerbotMgr", candidateGuid);
            return false;
        }

        requester->ModifyMoney(-int32(cost));

        ObjectGuid const henchGuid = ObjectGuid::Create<HighGuid::Player>(candidateGuid);
        std::string const useRole = role.empty() ? "dps" : role;

        // Register as a henchman BEFORE spawning so the patched AddPlayerBot
        // permission check (WowPsParty_IsHenchman_Trampoline) lets the cross-
        // account random-pool char in.
        WowPsParty::AddHenchmanDirective(account, henchGuid, requester->GetGUID(), useRole);

        // Restore this henchman's persisted loadout. party_loadout is keyed by
        // character guid, so a henchman the player edited in a PRIOR hire keeps
        // those changes when re-invited (Kevin's "next time I invite this guy he
        // still has my rotation"). Falls back to sensible defaults when the bot
        // has never been customised:
        //   rotation  : saved priority_actions_json, else class default — so the
        //               henchman runs OUR combat AI (LoS approach, no melee-
        //               stacking) with sensible spells, same engine as heroes.
        //   targetmode: saved strategies_csv, else tank -> "loose" / "master".
        //   lead       : saved glyphs_csv ("0" = off), else ON.
        {
            QueryResult lq = CharacterDatabase.Query(
                "SELECT `priority_actions_json`,`strategies_csv`,`glyphs_csv` "
                "FROM `party_loadout` WHERE `guid` = {}", candidateGuid);
            std::string savedRot, savedMode, savedLead;
            if (lq)
            {
                Field* lf = lq->Fetch();
                savedRot  = lf[0].Get<std::string>();
                savedMode = lf[1].Get<std::string>();
                savedLead = lf[2].Get<std::string>();
            }

            WowPsParty::RotationCacheSet(candidateGuid,
                WowPsParty::ParseRotationString(
                    savedRot.empty() ? DefaultRotationForClass(cls) : savedRot));

            WowPsParty::TargetModeCacheSet(candidateGuid,
                !savedMode.empty() ? savedMode
                                   : (useRole == "tank" ? "loose" : "master"));

            WowPsParty::LeadDungeonCacheSet(candidateGuid, savedLead != "0");
        }

        mgr->AddPlayerBot(henchGuid, account);

        // The spawn is async (login query holder). After a short delay: if the
        // bot arrived, group it + set loot; if it never arrived, undo the hire
        // (drop the directive, refund the gold) so we never leak a directive or
        // charge the player for a no-show.
        ObjectGuid const leaderGuid = requester->GetGUID();
        requester->m_Events.AddEventAtOffset([leaderGuid, henchGuid, cost]()
        {
            Player* lead = ObjectAccessor::FindConnectedPlayer(leaderGuid);
            Player* hen  = ObjectAccessor::FindConnectedPlayer(henchGuid);
            if (!hen || !hen->IsInWorld())
            {
                WowPsParty::RemoveFollower(henchGuid);
                if (lead)
                {
                    lead->ModifyMoney(int32(cost));   // refund the no-show
                    UpdateGroupLootForHenchmen(lead);
                    if (lead->GetSession())
                        ChatHandler(lead->GetSession()).PSendSysMessage(
                            "|cffff5555[WowPsParty]|r Henchman didn't arrive — refunded.");
                }
                LOG_WARN("module",
                    "[WowPsParty Henchmen] spawn no-show hench_guid={} — refunded {} "
                    "(bot not in world after delay)", henchGuid.GetCounter(), cost);
                return;
            }
            if (!lead) return;
            Group* g = lead->GetGroup();
            if (!g)
            {
                g = new Group();
                if (!g->Create(lead)) { delete g; return; }
                sGroupMgr->AddGroup(g);
            }
            if (!g->IsMember(henchGuid))
            {
                if (hen->GetGroup()) hen->RemoveFromGroup();
                g->AddMember(hen);
            }
            UpdateGroupLootForHenchmen(lead);
            // Loot/gather like the rest of the party.
            if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(hen))
            {
                ai->ChangeStrategy("+loot",   BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);
            }
        }, std::chrono::seconds(8));

        LOG_INFO("module",
            "[WowPsParty Henchmen] HIRE account={} hench_guid={} role={} level={} cost={}",
            account, candidateGuid, useRole, uint32(level), cost);
        outMsg = "Hired!";
        return true;
    }

    void DismissHenchman(Player* requester, uint32 henchGuid)
    {
        if (!requester || !requester->GetSession()) return;
        ObjectGuid const g = ObjectGuid::Create<HighGuid::Player>(henchGuid);
        if (!WowPsParty::IsHenchman(g)) return;   // only dismiss henchmen
        WowPsParty::RemoveFollower(g);
        WowPsParty::RotationCacheClear(henchGuid);
        WowPsParty::TargetModeCacheSet(henchGuid, "master");   // drop tank "loose"
        if (Player* hen = ObjectAccessor::FindConnectedPlayer(g))
            if (hen->GetGroup()) hen->RemoveFromGroup();
        // Log the bot out via the master's mgr regardless of whether it's
        // currently found in world — it may still be mid-spawn; not doing so
        // would orphan a default-AI companion with no directive until restart.
        if (PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(requester))
            mgr->LogoutPlayerBot(g);
        UpdateGroupLootForHenchmen(requester);
        LOG_INFO("module", "[WowPsParty Henchmen] DISMISS hench_guid={}", henchGuid);
    }

    // Dismiss a henchman identified only by guid — used by the group-removal
    // hook so that uninviting a henchman from the party (by ANY means, incl.
    // the stock WoW group UI) makes it stop following and despawn ("instant
    // hearth"). Drops the directive immediately (stops our follow ticker) and
    // defers the logout one tick — we may be called mid-group-removal, and
    // LogoutPlayerBot would otherwise re-enter group teardown for this member.
    void DismissHenchmanByGuid(ObjectGuid henchGuid)
    {
        if (!WowPsParty::IsHenchman(henchGuid)) return;
        ObjectGuid const leaderGuid = WowPsParty::GetLeaderFor(henchGuid);
        WowPsParty::RemoveFollower(henchGuid);
        WowPsParty::RotationCacheClear(henchGuid.GetCounter());
        WowPsParty::TargetModeCacheSet(henchGuid.GetCounter(), "master");
        LOG_INFO("module",
            "[WowPsParty Henchmen] henchman left party — dismissing hench_guid={}",
            henchGuid.GetCounter());
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader) return;
        leader->m_Events.AddEventAtOffset([leaderGuid, henchGuid]()
        {
            Player* l = ObjectAccessor::FindConnectedPlayer(leaderGuid);
            if (!l) return;
            if (PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(l))
                mgr->LogoutPlayerBot(henchGuid);
            UpdateGroupLootForHenchmen(l);
        }, std::chrono::milliseconds(200));
    }

    void DismissAllHenchmen(Player* requester)
    {
        if (!requester) return;
        // Collect this leader's henchmen, then dismiss each.
        std::vector<ObjectGuid> hench;
        {
            std::vector<ObjectGuid> guids;
            WowPsParty::GetPartyGuidsFor(requester->GetGUID(), guids);
            for (ObjectGuid const& gg : guids)
                if (WowPsParty::IsHenchman(gg)) hench.push_back(gg);
        }
        for (ObjectGuid const& gg : hench)
            DismissHenchman(requester, gg.GetCounter());
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

            auto const rows = FetchPartyRows(account);
            std::unordered_set<uint32> enrolled;
            for (auto const& row : rows) enrolled.insert(row.second);

            // Purge leftover henchmen BEFORE (re)building the group. Henchmen are
            // temporary (gone on dismiss/logout), but the WoW group is saved to
            // DB, so on the next login it still lists last session's henchmen as
            // offline members — the player would have to kick them by hand. Their
            // directives were already cleared on logout, so we can't use
            // IsHenchman here; instead remove any group member that isn't the
            // leader and isn't one of this account's enrolled alts. (This party
            // group only ever holds the player + their alts + henchmen, so
            // "not enrolled" == henchman.) Done first because RemoveMember can
            // DISBAND the group when it drops below 2 — we re-acquire it after.
            if (Group* existing = leader->GetGroup())
            {
                std::vector<ObjectGuid> toRemove;
                for (auto const& slot : existing->GetMemberSlots())
                    if (slot.guid != leader->GetGUID() &&
                        !enrolled.count(slot.guid.GetCounter()))
                        toRemove.push_back(slot.guid);
                for (ObjectGuid const& g : toRemove)
                {
                    existing->RemoveMember(g);  // may delete the group object
                    LOG_INFO("module",
                        "[WowPsParty] login purge: removed stale henchman guid={} "
                        "from party group", g.GetCounter());
                }
            }

            // Re-acquire the group — the purge above may have disbanded it.
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
