/*
 * WowPs Party-of-5 — Battleground on-demand fill (phase 2).
 *
 * A one-faction party (e.g. 5 Alliance) can never pop a BG: the enemy side is
 * empty. When a managed party HUMAN leader queues Warsong Gulch, spawn parked
 * RNDBOT-pool chars of the OPPOSITE faction at the party's bracket and queue them
 * for WSG so the match pops; they play via mod-playerbots' own BattleGroundTactics.
 * When the match ends (or the leader cancels the queue), log the fill bots out.
 *
 * Design notes / why this shape (see memory wowps-battleground-ondemand-fill):
 *  - Fill bots are INDEPENDENT, not party followers — an opposite-faction bot
 *    can't join the Alliance group (cross-faction group invite is rejected). They
 *    are spawned with master account 0 so PlayerbotMgr::AddPlayerBot treats them as
 *    random-pool bots (isRndbot = !masterAccountId) and skips the cross-account
 *    guard WITHOUT a follow directive or group membership.
 *  - The queue join replicates BGJoinAction::JoinQueue: a CMSG_BATTLEMASTER_JOIN
 *    packet carrying the bot's own GUID, QueuePacket'd — mod-playerbots' patched
 *    handler lets a bot queue without standing at a battlemaster.
 *  - Detection uses the OnPlayerJoinBG PlayerScript hook (fires on queue-join);
 *    the 1Hz WorldScript then drives each fill bot: queue -> accept invite ->
 *    cleanup. PHASE-2 IS LIVE-TESTED: deploy -> queue WSG -> watch the `module`
 *    logger -> fix. Several behaviours (master-0 spawn ownership, invite timing)
 *    are only confirmable in-game; this file logs each step so the log reads clearly.
 */

#include "PartyMgr.h"
#include "PartyFollow.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr uint8  BG_FILL_MIN_LEVEL = 70;   // 70-79 WSG bracket (this build's target)
    constexpr uint8  BG_FILL_MAX_LEVEL = 79;
    constexpr uint32 BG_FILL_MAX       = 5;    // fill the enemy team up to a 5v5

    // fillBotGuidLow -> leaderGuidLow
    std::unordered_map<uint32, uint32> g_fillBots;
    // leaders we've already spawned a fill for (one fill per queue session)
    std::unordered_set<uint32>         g_activeLeaders;
    std::mutex                         g_mutex;

    // The WSG queue type id (no arena).
    BattlegroundQueueTypeId WsgQueueType()
    {
        return BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_WS, 0);
    }

    // CSV of RNDBOT account ids (the same parked pool the henchman hire draws from).
    std::string RndbotAccountCsv()
    {
        std::string csv;
        if (QueryResult q = LoginDatabase.Query("SELECT `id` FROM `account` WHERE `username` LIKE 'RNDBOT%'"))
        {
            do {
                if (!csv.empty()) csv += ',';
                csv += std::to_string(q->Fetch()[0].Get<uint32>());
            } while (q->NextRow());
        }
        return csv;
    }

    // Spawn opposite-faction parked rndbot chars at the party's bracket as
    // independent bots (master 0) and register them as fill bots for this leader.
    void SpawnFillBots(Player* leader)
    {
        PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(leader);
        if (!mgr) { LOG_INFO("module", "[WowPsParty BGFill] no PlayerbotMgr for {} — abort", leader->GetName()); return; }

        std::string const acctCsv = RndbotAccountCsv();
        if (acctCsv.empty()) { LOG_INFO("module", "[WowPsParty BGFill] no rndbot pool — abort"); return; }

        // Opposite faction to the party.
        std::string const enemyRaces = (leader->GetTeamId() == TEAM_ALLIANCE)
            ? "2,5,6,8,10"    // Horde: Orc, Undead, Tauren, Troll, Blood Elf
            : "1,3,4,7,11";   // Alliance: Human, Dwarf, Night Elf, Gnome, Draenei

        // Size the fill to the party (min 2 to pop, cap BG_FILL_MAX).
        std::vector<ObjectGuid> party;
        WowPsParty::GetPartyGuidsFor(leader->GetGUID(), party);
        uint32 const want = std::min<uint32>(BG_FILL_MAX, std::max<uint32>(2, uint32(party.size())));

        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid` FROM `characters` WHERE `account` IN ({}) AND `online` = 0 "
            "AND `race` IN ({}) AND `level` BETWEEN {} AND {} ORDER BY RAND() LIMIT {}",
            acctCsv, enemyRaces, uint32(BG_FILL_MIN_LEVEL), uint32(BG_FILL_MAX_LEVEL), want);
        if (!q) { LOG_INFO("module", "[WowPsParty BGFill] no rndbot {}-{} of enemy faction found", BG_FILL_MIN_LEVEL, BG_FILL_MAX_LEVEL); return; }

        uint32 const leaderLow = leader->GetGUID().GetCounter();
        uint32 spawned = 0;
        do {
            uint32 const g = q->Fetch()[0].Get<uint32>();
            ObjectGuid const botGuid = ObjectGuid::Create<HighGuid::Player>(g);
            mgr->AddPlayerBot(botGuid, 0);   // master 0 -> isRndbot bypass, no group/follow
            { std::lock_guard<std::mutex> lk(g_mutex); g_fillBots[g] = leaderLow; }
            ++spawned;
        } while (q->NextRow());

        LOG_INFO("module", "[WowPsParty BGFill] {} queued WSG -> spawning {} enemy-faction fill bot(s)",
                 leader->GetName(), spawned);
        ChatHandler(leader->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Recruiting {} opponents to fill your Warsong Gulch match…", spawned);
    }

    void QueueFillBotForWsg(Player* bot)
    {
        WorldPacket* p = new WorldPacket(CMSG_BATTLEMASTER_JOIN, 20);
        *p << bot->GetGUID() << uint32(BATTLEGROUND_WS) << uint32(0) /*instanceId: first available*/ << uint8(0) /*joinAsGroup*/;
        bot->GetSession()->QueuePacket(p);   // patched handler lets a bot queue without a battlemaster
        LOG_INFO("module", "[WowPsParty BGFill] fill bot {} queued for WSG", bot->GetName());
    }

    // Accept a pending BG invite for a fill bot (mirrors phase-1 TickAcceptBgInvite).
    void AcceptFillBotInvite(Player* bot)
    {
        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const qt = bot->GetBattlegroundQueueTypeId(i);
            if (qt == BATTLEGROUND_QUEUE_NONE) continue;
            BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(qt);
            GroupQueueInfo ginfo;
            if (!bgQueue.GetPlayerGroupInfoData(bot->GetGUID(), &ginfo)) continue;
            if (!ginfo.IsInvitedToBGInstanceGUID || !ginfo.RemoveInviteTime) continue;

            BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(qt);
            uint8 const arenaType = BattlegroundMgr::BGArenaType(qt);
            WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
            packet << uint8(arenaType) << uint8(0) << uint32(bgTypeId) << uint16(0x1F90) << uint8(1);
            bot->GetSession()->HandleBattleFieldPortOpcode(packet);
            LOG_INFO("module", "[WowPsParty BGFill] fill bot {} accepted WSG invite", bot->GetName());
            return;
        }
    }

    void RetireFillBot(uint32 botLow, Player* bot /*may be null*/)
    {
        uint32 leaderLow = 0;
        bool stillHasSiblings = false;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_fillBots.find(botLow);
            if (it == g_fillBots.end()) return;
            leaderLow = it->second;
            g_fillBots.erase(it);
            for (auto const& kv : g_fillBots) if (kv.second == leaderLow) { stillHasSiblings = true; break; }
            if (!stillHasSiblings) g_activeLeaders.erase(leaderLow);
        }
        if (bot && bot->GetSession())
        {
            LOG_INFO("module", "[WowPsParty BGFill] retiring fill bot {} (match/queue over)", bot->GetName());
            bot->GetSession()->LogoutPlayer(true);
        }
    }
}

class PartyBgFillPlayerScript : public PlayerScript
{
public:
    PartyBgFillPlayerScript() : PlayerScript("PartyBgFillPlayerScript", {
        PLAYERHOOK_ON_PLAYER_JOIN_BG,
        PLAYERHOOK_ON_REMOVE_FROM_BATTLEGROUND
    }) { }

    void OnPlayerJoinBG(Player* player) override
    {
        if (!WowPsParty::IsEnabled() || !player || !player->GetSession()) return;
        if (sPlayerbotsMgr.GetPlayerbotAI(player)) return;   // a bot joined; only react to the human leader

        // Must be a managed party (has companions following) queuing Warsong Gulch.
        std::vector<ObjectGuid> party;
        WowPsParty::GetPartyGuidsFor(player->GetGUID(), party);
        if (party.size() < 2) return;
        if (!player->InBattlegroundQueueForBattlegroundQueueType(WsgQueueType())) return;
        // Bracket gate: this build fills the 70-79 WSG bracket.
        if (player->GetLevel() < BG_FILL_MIN_LEVEL || player->GetLevel() > BG_FILL_MAX_LEVEL) return;

        uint32 const leaderLow = player->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_activeLeaders.count(leaderLow)) return;   // already filling for this leader
            g_activeLeaders.insert(leaderLow);
        }
        SpawnFillBots(player);
    }

    void OnPlayerRemoveFromBattleground(Player* player, Battleground* /*bg*/) override
    {
        if (!player) return;
        RetireFillBot(player->GetGUID().GetCounter(), player);
    }
};

class PartyBgFillWorldScript : public WorldScript
{
public:
    PartyBgFillWorldScript() : WorldScript("PartyBgFillWorldScript", { WORLDHOOK_ON_UPDATE }) { }

    void OnUpdate(uint32 diff) override
    {
        _accum += diff;
        if (_accum < 1000) return;
        _accum = 0;

        // Snapshot BOTH sets under one lock. The leader's-own-alts drive loop
        // below must run even when there are zero fill bots (SpawnFillBots can
        // abort with none found for the enemy bracket, yet the leader is still in
        // g_activeLeaders and queued) — otherwise the human enters WSG alone.
        std::vector<std::pair<uint32, uint32>> snapshot;
        std::vector<uint32> leaders;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_fillBots.empty() && g_activeLeaders.empty()) return;
            snapshot.assign(g_fillBots.begin(), g_fillBots.end());
            leaders.assign(g_activeLeaders.begin(), g_activeLeaders.end());
        }

        for (auto const& [botLow, leaderLow] : snapshot)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(botLow));
            if (!bot)
                continue;   // still spawning (async), or already gone — handled next tick / on remove

            // If the leader bailed out of the queue before it popped, the fill bots
            // should leave too (they'd otherwise sit queued forever).
            Player* leader = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(leaderLow));
            bool const leaderGone = !leader || !leader->IsInWorld()
                || (!leader->InBattleground() && !leader->InBattlegroundQueueForBattlegroundQueueType(WsgQueueType()));
            if (leaderGone && !bot->InBattleground())
            {
                RetireFillBot(botLow, bot);   // logout dequeues it from the BG queue
                continue;
            }

            if (bot->InBattleground())
                continue;   // in the match; its own AI plays it; cleanup fires on remove

            if (!bot->InBattlegroundQueue())
                QueueFillBotForWsg(bot);     // freshly logged in -> queue it
            else
                AcceptFillBotInvite(bot);    // queued -> accept the pop when invited
        }

        // The LEADER's own party-of-5 bots must enter the BG too. Phase-1
        // (TickAcceptBgInvite, per-bot AI tick) proved unreliable in the ~40s invite
        // window for managed bots — the human entered WSG alone (1v5) while the alts
        // stayed in the world. Drive them from this reliable world-thread tick:
        // queue any the group-queue missed (Alliance, same team as the human), and
        // accept the pop. NB: party bots are the user's ALTS — never logged out here
        // (only fill bots are, via RetireFillBot), so they're not added to g_fillBots.
        for (uint32 leaderLow : leaders)
        {
            Player* leader = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(leaderLow));
            if (!leader || (!leader->InBattleground()
                            && !leader->InBattlegroundQueueForBattlegroundQueueType(WsgQueueType())))
                continue;
            std::vector<ObjectGuid> party;
            WowPsParty::GetPartyGuidsFor(leader->GetGUID(), party);
            for (ObjectGuid const& g : party)
            {
                if (g == leader->GetGUID()) continue;
                Player* pb = ObjectAccessor::FindConnectedPlayer(g);
                if (!pb || !sPlayerbotsMgr.GetPlayerbotAI(pb)) continue;   // managed alts only
                if (pb->InBattleground()) continue;                        // already in
                if (!pb->InBattlegroundQueue())
                {
                    LOG_INFO("module", "[WowPsParty BGFill] party bot {} wasn't in the BG queue — queuing it", pb->GetName());
                    QueueFillBotForWsg(pb);     // Alliance -> joins the human's team
                }
                else
                    AcceptFillBotInvite(pb);    // accept the pop so it ports in with the human
            }
        }
    }

private:
    uint32 _accum = 0;
};

void AddPartyBgFillScripts()
{
    new PartyBgFillPlayerScript();
    new PartyBgFillWorldScript();
}
