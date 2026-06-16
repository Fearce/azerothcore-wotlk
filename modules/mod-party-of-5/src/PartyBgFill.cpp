/*
 * WowPs Party-of-5 — Battleground full-fill.
 *
 * A managed party (a human + heroes/henchmen) can't pop a BG on its own and,
 * even if it did, would face an empty/half-empty enemy. When such a human queues
 * ANY normal battleground, fill BOTH teams up to the BG's MaxPlayersPerTeam with
 * parked RNDBOT-pool chars at the human's bracket:
 *   - the human's team: enough SAME-faction bots to top up (human + heroes + fills
 *     = MaxPerTeam),
 *   - the enemy team: MaxPerTeam OPPOSITE-faction bots.
 * so the match pops as a full N-v-N (WSG 10v10, AB 15v15, AV 40v40, EotS/SotA 15v15,
 * IoC 40v40, …). Fill bots play via mod-playerbots' BattleGroundTactics; they (and
 * the human's heroes) are driven to queue + accept the pop from the world tick,
 * since gated party bots never click "Enter Battle". When the match/queue ends the
 * fill bots are logged out (the human's own heroes are NOT — they're real alts).
 *
 * Design notes (see memory wowps-battleground-ondemand-fill):
 *  - Capacity + bracket are read from the BG TEMPLATE at queue time
 *    (GetBattlegroundTemplate + GetBattlegroundBracketByLevel) so this works for
 *    every BG and level bracket with no hardcoding.
 *  - Fill bots are INDEPENDENT (master 0 -> isRndbot bypass): an opposite-faction
 *    bot can't join the human's group, and same-faction fills don't need to.
 *  - OnPlayerJoinBG fires at QUEUE time (BattleGroundHandler), per group member, so
 *    we react when the human queues — not after the (empty) match already formed.
 *  - Random BG (BATTLEGROUND_RB) and arenas are skipped: the real BG isn't known
 *    until pop, so it can't be pre-filled.
 */

#include "PartyMgr.h"
#include "PartyFollow.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Chat.h"
#include "DBCStores.h"        // GetBattlegroundBracketByLevel
#include "DBCStructure.h"     // PvPDifficultyEntry
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Timer.h"           // getMSTime — grace-retire fill bots that never loaded
#include "WorldPacket.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct FillEntry { uint32 leaderLow; uint32 bgTypeId; uint32 spawnMs; };

    // A fill bot that never finishes its async login is retired after this so it
    // can't pin the leader's session (g_activeLeaders) forever.
    constexpr uint32 FILL_SPAWN_GRACE_MS = 60000;

    // fillBotGuidLow -> {leaderGuidLow, bgTypeId}
    std::unordered_map<uint32, FillEntry> g_fillBots;
    // leaderGuidLow -> bgTypeId currently being filled (one fill session per leader)
    std::unordered_map<uint32, uint32>    g_activeLeaders;
    std::mutex                            g_mutex;

    BattlegroundQueueTypeId QueueTypeFor(uint32 bgTypeId)
    {
        return BattlegroundMgr::BGQueueTypeId(BattlegroundTypeId(bgTypeId), 0);
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

    // Races on a given side, for picking faction-correct fill bots.
    char const* RaceCsv(bool alliance)
    {
        return alliance ? "1,3,4,7,11"   // Human, Dwarf, Night Elf, Gnome, Draenei
                        : "2,5,6,8,10";  // Orc, Undead, Tauren, Troll, Blood Elf
    }

    // Spawn up to `count` parked rndbot chars of the given races within [bmin,bmax]
    // as independent bots (master 0) and register them as fill bots for this leader's
    // BG. Returns how many were actually spawned (pool may be short — logged).
    uint32 SpawnFillTeam(PlayerbotMgr* mgr, uint32 leaderLow, uint32 bgTypeId,
                         std::string const& acctCsv, char const* races,
                         uint8 bmin, uint8 bmax, uint32 count, char const* sideLabel)
    {
        if (count == 0) return 0;
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid` FROM `characters` WHERE `account` IN ({}) AND `online` = 0 "
            "AND `race` IN ({}) AND `level` BETWEEN {} AND {} ORDER BY RAND() LIMIT {}",
            acctCsv, races, uint32(bmin), uint32(bmax), count);
        if (!q)
        {
            LOG_INFO("module", "[WowPsParty BGFill] no parked {} rndbot ({}-{}) available — {} side short by {}",
                     sideLabel, bmin, bmax, sideLabel, count);
            return 0;
        }
        uint32 spawned = 0;
        do {
            uint32 const g = q->Fetch()[0].Get<uint32>();
            ObjectGuid const botGuid = ObjectGuid::Create<HighGuid::Player>(g);
            mgr->AddPlayerBot(botGuid, 0);   // master 0 -> isRndbot bypass, no group/follow
            { std::lock_guard<std::mutex> lk(g_mutex); g_fillBots[g] = { leaderLow, bgTypeId, getMSTime() }; }
            ++spawned;
        } while (q->NextRow());

        if (spawned < count)
            LOG_INFO("module", "[WowPsParty BGFill] {} side: pool gave {}/{} — filling short (no silent cap)",
                     sideLabel, spawned, count);
        return spawned;
    }

    // Begin a full-fill for `human`'s freshly-queued `bgTypeId`.
    void StartFill(Player* human, uint32 bgTypeId)
    {
        PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(human);
        if (!mgr) { LOG_INFO("module", "[WowPsParty BGFill] no PlayerbotMgr for {} — abort", human->GetName()); return; }

        Battleground* tpl = sBattlegroundMgr->GetBattlegroundTemplate(BattlegroundTypeId(bgTypeId));
        if (!tpl) { LOG_INFO("module", "[WowPsParty BGFill] no template for bg {} — abort", bgTypeId); return; }
        uint32 const maxPerTeam = tpl->GetMaxPlayersPerTeam();
        if (maxPerTeam == 0) return;

        PvPDifficultyEntry const* br = GetBattlegroundBracketByLevel(tpl->GetMapId(), human->GetLevel());
        if (!br) { LOG_INFO("module", "[WowPsParty BGFill] no bracket for level {} on map {} — abort", human->GetLevel(), tpl->GetMapId()); return; }
        uint8 const bmin = uint8(br->minLevel);
        uint8 const bmax = uint8(br->maxLevel);

        std::string const acctCsv = RndbotAccountCsv();
        if (acctCsv.empty()) { LOG_INFO("module", "[WowPsParty BGFill] no rndbot pool — abort"); return; }

        // The human's team already contributes the human + heroes (all same faction).
        std::vector<ObjectGuid> party;
        WowPsParty::GetPartyGuidsFor(human->GetGUID(), party);
        uint32 const ownSide  = uint32(party.size());                       // human + heroes
        uint32 const allyNeed = maxPerTeam > ownSide ? maxPerTeam - ownSide : 0;
        uint32 const enemyNeed = maxPerTeam;

        bool const allianceLeader = human->GetTeamId() == TEAM_ALLIANCE;
        uint32 const leaderLow = human->GetGUID().GetCounter();

        uint32 const allies  = SpawnFillTeam(mgr, leaderLow, bgTypeId, acctCsv,
                                             RaceCsv(allianceLeader),  bmin, bmax, allyNeed,  "ally");
        uint32 const enemies = SpawnFillTeam(mgr, leaderLow, bgTypeId, acctCsv,
                                             RaceCsv(!allianceLeader), bmin, bmax, enemyNeed, "enemy");

        LOG_INFO("module",
            "[WowPsParty BGFill] {} queued bg {} ({}v{} bracket {}-{}): own={} +{} ally fills, +{} enemy fills",
            human->GetName(), bgTypeId, maxPerTeam, maxPerTeam, bmin, bmax, ownSide, allies, enemies);
        ChatHandler(human->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Filling your battleground: +{} allies, +{} opponents…", allies, enemies);
    }

    void QueueFillBot(Player* bot, uint32 bgTypeId)
    {
        WorldPacket* p = new WorldPacket(CMSG_BATTLEMASTER_JOIN, 20);
        *p << bot->GetGUID() << uint32(bgTypeId) << uint32(0) /*instanceId: first available*/ << uint8(0) /*joinAsGroup*/;
        bot->GetSession()->QueuePacket(p);   // patched handler lets a bot queue without a battlemaster
        LOG_INFO("module", "[WowPsParty BGFill] {} queued for bg {}", bot->GetName(), bgTypeId);
    }

    // Accept a pending BG invite for a bot, whatever BG it's for (mirrors the human
    // clicking "Enter Battle"). General — used for both fill bots and the heroes.
    void AcceptBgInvite(Player* bot)
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
            LOG_INFO("module", "[WowPsParty BGFill] {} accepted bg {} invite", bot->GetName(), uint32(bgTypeId));
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
            leaderLow = it->second.leaderLow;
            g_fillBots.erase(it);
            for (auto const& kv : g_fillBots) if (kv.second.leaderLow == leaderLow) { stillHasSiblings = true; break; }
            if (!stillHasSiblings) g_activeLeaders.erase(leaderLow);
        }
        if (bot && bot->GetSession())
        {
            LOG_INFO("module", "[WowPsParty BGFill] retiring fill bot {} (match/queue over)", bot->GetName());
            bot->GetSession()->LogoutPlayer(true);
        }
    }

    // The first fillable BG the human is queued for that we aren't already filling:
    // a normal (non-arena, non-random) battleground. Returns 0 (BATTLEGROUND_TYPE_NONE)
    // if none.
    uint32 PickFillableBg(Player* human)
    {
        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const qt = human->GetBattlegroundQueueTypeId(i);
            if (qt == BATTLEGROUND_QUEUE_NONE) continue;
            BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(qt);
            if (bgTypeId == BATTLEGROUND_RB) continue;            // random — BG unknown until pop
            Battleground* tpl = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
            if (!tpl || tpl->isArena() || tpl->GetMaxPlayersPerTeam() == 0) continue;
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_activeLeaders.count(human->GetGUID().GetCounter())) return 0;  // already filling one
            return uint32(bgTypeId);
        }
        return 0;
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

        // Only fill for a MANAGED party (a human running with heroes/henchmen) — not
        // every incidental real player who queues.
        std::vector<ObjectGuid> party;
        WowPsParty::GetPartyGuidsFor(player->GetGUID(), party);
        if (party.size() < 2) return;

        uint32 const bgTypeId = PickFillableBg(player);
        if (!bgTypeId) return;

        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_activeLeaders.count(player->GetGUID().GetCounter())) return;
            g_activeLeaders[player->GetGUID().GetCounter()] = bgTypeId;
        }
        StartFill(player, bgTypeId);
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

        std::vector<std::pair<uint32, FillEntry>> fills;
        std::vector<std::pair<uint32, uint32>>    leaders;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_fillBots.empty() && g_activeLeaders.empty()) return;
            fills.assign(g_fillBots.begin(), g_fillBots.end());
            leaders.assign(g_activeLeaders.begin(), g_activeLeaders.end());
        }

        auto leaderStillIn = [](Player* leader, uint32 bgTypeId) -> bool
        {
            return leader && leader->IsInWorld()
                && (leader->InBattleground()
                    || leader->InBattlegroundQueueForBattlegroundQueueType(QueueTypeFor(bgTypeId)));
        };

        // 1) Drive each fill bot: queue -> accept the pop -> retire when it's over.
        for (auto const& [botLow, fe] : fills)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(botLow));
            if (!bot)
            {
                // Still spawning (async) is normal for a second or two. But a bot whose
                // AddPlayerBot silently failed would never appear — retire it after a
                // grace so it can't pin the leader's fill session (g_activeLeaders)
                // until restart. RetireFillBot handles the null-bot case.
                if (getMSTime() - fe.spawnMs > FILL_SPAWN_GRACE_MS)
                {
                    LOG_INFO("module", "[WowPsParty BGFill] fill bot {} never loaded — retiring stale entry", botLow);
                    RetireFillBot(botLow, nullptr);
                }
                continue;
            }

            Player* leader = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(fe.leaderLow));
            if (!leaderStillIn(leader, fe.bgTypeId) && !bot->InBattleground())
            {
                RetireFillBot(botLow, bot);   // leader bailed the queue -> logout dequeues the bot
                continue;
            }
            if (bot->InBattleground()) continue;                 // in the match; its AI plays it
            if (!bot->InBattlegroundQueue()) QueueFillBot(bot, fe.bgTypeId);
            else                             AcceptBgInvite(bot);
        }

        // 2) Drive the LEADER's own heroes (managed party bots) into the BG too — they
        //    never click "Enter Battle" on their own, which is why the human entered
        //    alone. Queue any the group-queue missed, and accept the pop.
        for (auto const& [leaderLow, bgTypeId] : leaders)
        {
            Player* leader = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(leaderLow));
            if (!leaderStillIn(leader, bgTypeId))
            {
                // Leader gone and no fill bots left to retire -> drop the stale session.
                std::lock_guard<std::mutex> lk(g_mutex);
                bool hasFills = false;
                for (auto const& kv : g_fillBots) if (kv.second.leaderLow == leaderLow) { hasFills = true; break; }
                if (!hasFills) g_activeLeaders.erase(leaderLow);
                continue;
            }
            std::vector<ObjectGuid> party;
            WowPsParty::GetPartyGuidsFor(leader->GetGUID(), party);
            for (ObjectGuid const& g : party)
            {
                if (g == leader->GetGUID()) continue;
                Player* pb = ObjectAccessor::FindConnectedPlayer(g);
                if (!pb || !sPlayerbotsMgr.GetPlayerbotAI(pb)) continue;   // managed heroes only
                if (pb->InBattleground()) continue;                        // already in
                if (!pb->InBattlegroundQueue())
                {
                    // Normally the heroes are group-queued with the human and reach
                    // this loop only to ACCEPT. A hero NOT in the queue means a solo
                    // (non-group) human join — solo-queue it to get it in. On a busy
                    // server a solo queue can land in a different instance of the same
                    // bracket; log it so that split case is diagnosable.
                    LOG_INFO("module", "[WowPsParty BGFill] hero {} wasn't queued (solo join?) — solo-queuing for bg {}", pb->GetName(), bgTypeId);
                    QueueFillBot(pb, bgTypeId);                             // same team as the human
                }
                else
                    AcceptBgInvite(pb);                                     // accept the pop
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
