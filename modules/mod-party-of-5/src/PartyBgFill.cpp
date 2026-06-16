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
    struct FillEntry { uint32 leaderLow; uint32 bgTypeId; uint32 spawnMs; bool entered; };

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
            { std::lock_guard<std::mutex> lk(g_mutex); g_fillBots[g] = { leaderLow, bgTypeId, getMSTime(), false }; }
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
        if (!bot || !bot->IsInWorld() || !bot->GetSession()) return;
        // Strip the Deserter debuff (spell 26013) first — CanJoinToBattleground
        // refuses the queue while it's up. The heroes accumulated it from prior
        // attempts where they couldn't enter (no-show Deserter), a vicious cycle
        // that left them re-queuing every tick and never getting in. These are the
        // player's own managed bots filling for them, not real players gaming the
        // system, so clearing it is correct.
        if (bot->HasAura(26013)) bot->RemoveAura(26013);
        WorldPacket* p = new WorldPacket(CMSG_BATTLEMASTER_JOIN, 20);
        *p << bot->GetGUID() << uint32(bgTypeId) << uint32(0) /*instanceId: first available*/ << uint8(0) /*joinAsGroup*/;
        bot->GetSession()->QueuePacket(p);   // patched handler lets a bot queue without a battlemaster
        LOG_INFO("module", "[WowPsParty BGFill] {} queued for bg {}", bot->GetName(), bgTypeId);
    }

    // Accept a pending BG invite for a bot, whatever BG it's for (mirrors the human
    // clicking "Enter Battle"). General — used for both fill bots and the heroes.
    void AcceptBgInvite(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !bot->GetSession()) return;
        if (bot->InBattleground() || bot->IsBeingTeleported()) return;   // already going in
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
            // DEFER the port: QUEUE the CMSG_BATTLEFIELD_PORT so the core processes it
            // in the bot's normal session update — do NOT call HandleBattleFieldPortOpcode
            // synchronously here. This runs inside the world OnUpdate tick, and porting a
            // player across maps mid-world-update (×many bots + the heroes at once on a
            // full-fill BG join) crashed the worldserver (Kevin: immediate crash joining
            // WSG). Mirrors QueueFillBot's deferred join, which is stable.
            WorldPacket* p = new WorldPacket(CMSG_BATTLEFIELD_PORT, 20);
            *p << uint8(arenaType) << uint8(0) << uint32(bgTypeId) << uint16(0x1F90) << uint8(1);
            bot->GetSession()->QueuePacket(p);
            LOG_INFO("module", "[WowPsParty BGFill] {} queued PORT-accept for bg {}", bot->GetName(), uint32(bgTypeId));
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

    // Can this BG type be FILLED with bots — a normal, specific battleground? NOT
    // Random BG (real BG unknown until pop) and NOT an arena.
    bool IsFillableBg(uint32 bgTypeId)
    {
        if (bgTypeId == BATTLEGROUND_RB) return false;
        Battleground* tpl = sBattlegroundMgr->GetBattlegroundTemplate(BattlegroundTypeId(bgTypeId));
        return tpl && !tpl->isArena() && tpl->GetMaxPlayersPerTeam() > 0;
    }

    // The (non-arena) battleground the human just queued — INCLUDING Random BG. We
    // TRACK the leader for ANY such queue so the world tick drives the human's heroes
    // to accept the pop — that doesn't need the specific BG, and the heroes were
    // getting a no-show Deserter debuff on Random-BG queues because the old code only
    // tracked fillable BGs. Spawning enemy fills is gated separately on IsFillableBg.
    // Returns 0 if the human isn't in a normal BG queue (e.g. arena only).
    uint32 PickQueuedBg(Player* human)
    {
        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const qt = human->GetBattlegroundQueueTypeId(i);
            if (qt == BATTLEGROUND_QUEUE_NONE) continue;
            if (BattlegroundMgr::BGArenaType(qt) != 0) continue;   // skip arenas (rated/separate flow)
            return uint32(BattlegroundMgr::BGTemplateId(qt));      // a normal BG or Random BG
        }
        return 0;
    }
}

class PartyBgFillPlayerScript : public PlayerScript
{
public:
    PartyBgFillPlayerScript() : PlayerScript("PartyBgFillPlayerScript", {
        PLAYERHOOK_ON_PLAYER_JOIN_BG
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

        uint32 const bgTypeId = PickQueuedBg(player);
        if (!bgTypeId) return;

        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_activeLeaders.count(player->GetGUID().GetCounter())) return;
            g_activeLeaders[player->GetGUID().GetCounter()] = bgTypeId;
        }
        // Always drive the heroes to accept (the world tick handles that via
        // g_activeLeaders). Only spawn enemy fills for a specific, fillable BG —
        // Random BG resolves its real BG at pop, so it can't be pre-filled.
        if (IsFillableBg(bgTypeId))
            StartFill(player, bgTypeId);
        else
            LOG_INFO("module",
                "[WowPsParty BGFill] {} queued bg {} (random/unfillable) — driving heroes to accept the pop, no enemy fill",
                player->GetName(), bgTypeId);
    }
    // NB: deliberately NO OnPlayerRemoveFromBattleground. Retiring (logging out) a
    // fill bot synchronously inside the BG-removal hook — for every fill bot when a
    // 10v10/40v40 ends — is a re-entrant teardown crash risk. The world tick retires
    // them safely instead (it watches each fill bot leave its match via `entered`).
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

            if (bot->InBattleground())
            {
                // In the match; its AI plays it. Remember it made it in, so when the
                // match ENDS (below) we retire it from HERE — never from the BG-removal
                // hook, where a synchronous LogoutPlayer mid-teardown (×10-40 bots on a
                // BG end) is a crash risk.
                if (!fe.entered)
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    auto it = g_fillBots.find(botLow);
                    if (it != g_fillBots.end()) it->second.entered = true;
                }
                continue;
            }
            // Out of the BG. If it had ENTERED, its match is over -> retire safely here.
            if (fe.entered) { RetireFillBot(botLow, bot); continue; }

            // Pre-pop: if the leader bailed the queue, the fill bots leave too.
            Player* leader = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(fe.leaderLow));
            if (!leaderStillIn(leader, fe.bgTypeId)) { RetireFillBot(botLow, bot); continue; }
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

// Catches the case OnPlayerJoinBG (queue time) misses: the human queued before its
// heroes were in the party (e.g. queued solo, or the BG reformed the group on entry
// and the party-of-5 system re-grouped the heroes only AFTER the human was already
// inside). On ENTRY we track the human regardless of current party size — scoped to
// party-of-5 users (companions enabled) — so the world tick backfills the heroes into
// THIS bg the moment they appear in the party. Without this they sat in Dalaran,
// never queued, and the human played the BG short-handed (the 6v10 Kevin saw).
class PartyBgFillBgScript : public AllBattlegroundScript
{
public:
    PartyBgFillBgScript() : AllBattlegroundScript("PartyBgFillBgScript", {
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_ADD_PLAYER
    }) { }

    void OnBattlegroundAddPlayer(Battleground* bg, Player* player) override
    {
        if (!WowPsParty::IsEnabled() || !bg || !player || !player->GetSession()) return;
        if (sPlayerbotsMgr.GetPlayerbotAI(player)) return;   // a bot entered; only the human leader
        if (bg->isArena()) return;
        // Track even with no heroes grouped YET — they may group after entry; the
        // world tick brings them in once they're in the party. Scoped to managed
        // (companions-enabled) accounts so we don't track unrelated real players.
        if (!WowPsParty::GetAccountSettings(player->GetSession()->GetAccountId()).spawnCompanions)
            return;
        std::lock_guard<std::mutex> lk(g_mutex);
        g_activeLeaders[player->GetGUID().GetCounter()] = uint32(bg->GetBgTypeID());
    }
};

void AddPartyBgFillScripts()
{
    new PartyBgFillPlayerScript();
    new PartyBgFillWorldScript();
    new PartyBgFillBgScript();
}
