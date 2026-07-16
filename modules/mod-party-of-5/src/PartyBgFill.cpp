/*
 * WowPs Party-of-5 — Battleground full-fill.
 *
 * A managed party (a human + heroes/henchmen) can't pop a BG on its own and,
 * even if it did, would face an empty/half-empty enemy. When such a human queues
 * ANY normal battleground, fill BOTH teams up to the BG's MaxPlayersPerTeam with
 * parked RNDBOT-pool chars re-leveled to the human's bracket:
 *   - the human's team: enough SAME-faction bots to top up (human + heroes + fills
 *     = MaxPerTeam),
 *   - the enemy team: MaxPerTeam OPPOSITE-faction bots.
 * so the match aims to pop as a full N-v-N (WSG 10v10, AB 15v15, AV 40v40, EotS/SotA
 * 15v15, IoC 40v40, …). Fill bots play via mod-playerbots' BattleGroundTactics; they (and
 * the human's heroes) are driven to queue + accept the pop from the world tick,
 * since gated party bots never click "Enter Battle". When the match/queue ends the
 * fill bots are logged out (the human's own heroes are NOT — they're real alts).
 * NB: a full N-v-N is best-effort, not guaranteed — out-of-bracket fills re-level one per
 * world tick (below), and CheckNormalMatch pops as soon as both sides reach MinPlayers, so
 * a big draw can start partially filled and the rest backfill as they queue. A sub-cap pop
 * (e.g. 14v20) is staggered fill, not a regression; the human is always in it.
 *
 * How the human is NEVER locked out of their own match (the bug the earlier
 * enemy-only revert was avoiding): EVERY participant — human, heroes and all fills —
 * lands in the NORMAL queue, so CheckNormalMatch forms ONE match FIFO and the human
 * (who queued first, before the async fill logins) is always selected; FillPlayersToBG
 * then tops both sides up to MaxPerTeam from the remaining normal-queue fills. The one
 * BG whose MinPlayersPerTeam is <= a full 5-party — WSG — would otherwise flag the
 * human's group as a PREMADE (isPremade = members >= MinPlayersPerTeam) and strand it in
 * the premade queue while the solo fills self-matched in the normal queue; a migration
 * raises WSG min to 6 so any party of <=5 is always normal (its stock min is below 6:
 * the AzerothCore seed is 5, this server's live value is 2 — both trip a full party).
 * Every other BG's min is already >5. See data/sql/db-world/updates/
 * 2026_06_19_00_party_bgfill_wsg_minplayers.sql. With no premade anywhere, there is no
 * same-faction self-match: ally fills can only ever backfill the human's own match.
 *
 * Design notes (see memory wowps-battleground-ondemand-fill):
 *  - Capacity + bracket are read from the BG TEMPLATE at queue time
 *    (GetBattlegroundTemplate + GetBattlegroundBracketByLevel) so this works for
 *    every BG and level bracket with no hardcoding.
 *  - Fill bots are INDEPENDENT (master 0 -> isRndbot bypass): they queue solo and
 *    backfill the human's match; none join the human's group.
 *  - A small flat pool covers every bracket: in-bracket parked chars are drawn first
 *    (no re-level); any shortfall is backfilled from out-of-bracket chars re-leveled
 *    to the bracket via PlayerbotFactory on the world tick — at most one re-level per
 *    tick so a 40v40 spin-up never stalls the world thread.
 *  - OnPlayerJoinBG fires at QUEUE time (BattleGroundHandler), per group member, so
 *    we react when the human queues — not after the (empty) match already formed.
 *  - Random BG (BATTLEGROUND_RB) IS supported: it has no real map at queue time, but
 *    its own template capacity (10v10, level 80) governs the RB queue, so we fill the
 *    RB queue itself to 10v10. It pops a randomly-chosen real BG, then TopUpBgInPrep
 *    reads that live instance's true capacity and backfills it (e.g. an AV to 40v40)
 *    during the pre-gate countdown. Only ARENAS are skipped here (separate rated flow).
 */

#include "PartyMgr.h"
#include "PartyFollow.h"

#include "ArenaTeam.h"        // rated-arena enemy fill: bot arena teams
#include "ArenaTeamMgr.h"     // sArenaTeamMgr->GetArenaTeamById
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "GroupMgr.h"         // sGroupMgr->AddGroup for the bot arena team's group
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
#include "World.h"           // sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL) — never re-level a fill bot past the cap
#include "WorldPacket.h"
#include "WorldSession.h"

#include "AiObjectContext.h"    // "bg role" roll for fill bots (BGTactics job split)
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"  // sPlayerbotAIConfig.randomBotJoinBG (the "bg" strategy gate)
#include "PlayerbotFactory.h"   // re-level a pool fill bot to the BG bracket on spawn
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h" // sRandomPlayerbotMgr — server-side spawn for WG (no human anchor)
#include "Battlefield.h"        // Wintergrasp: IsWarTime / war enrolment / participant sets
#include "BattlefieldMgr.h"     // sBattlefieldMgr->GetBattlefieldToZoneId(WG)
#include "BattlefieldWG.h"      // CanInteractWithRelic (final-gate breach -> defenders to core)
#include "MotionMaster.h"       // WG bot AI movement (MovePoint / MoveChase)
#include "GameObject.h"         // WG siege: destructible keep-wall damage (ModifyHealth)
#include "Creature.h"           // WG siege vehicles (steer / react state / despawn)
#include "Map.h"                // WG waypoint ground-snap (Map::GetHeight)

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace
{
    struct FillEntry
    {
        uint32 leaderLow;
        uint32 bgTypeId;
        uint32 spawnMs;
        bool   entered;     // made it into the BG (retire from the world tick on exit)
        bool   ally;        // same faction as the human (true) or enemy fill (false)
        uint8  bmin;        // bracket min level — re-level target floor
        uint8  bmax;        // bracket max level — re-level target (top of bracket)
        bool   releveled;   // re-level done, or unnecessary (in-bracket pool char)
        uint32 outSinceMs = 0;  // when it first read !InBattleground after entering (0 = in)
        uint32 account = 0; // the pool account this char is on — only ONE char per account
                            // can be online, so fills must claim DISTINCT accounts
    };

    // Grace before retiring a fill bot that read !InBattleground after entering. A real
    // match-end stays out; a transient clear of bgInstanceID (an intra-BG teleport / a tick
    // where the flag flickers) comes back within this window — without it an active bot got
    // logged out mid-match ("a bot just left in the middle of a battleground").
    constexpr uint32 BG_OUT_GRACE_MS = 8000;

    // A fill bot that never finishes its async login is retired after this so it
    // can't pin the leader's session (g_activeLeaders) forever.
    constexpr uint32 FILL_SPAWN_GRACE_MS = 60000;

    // Don't bot-fill a BG the INSTANT a managed party queues — hold off this long first so
    // REAL players have a window to queue and we get genuine multiplayer matches instead of
    // an immediate bot pop (Kevin: an instant pop forces grouping-up before queueing). If the
    // BG pops on its own within the window (enough real players), we never fill (TopUpBgInPrep
    // tops up the deficit during prep instead).
    constexpr uint32 BG_FILL_DELAY_MS = 20000;

    // fillBotGuidLow -> {leaderGuidLow, bgTypeId}
    std::unordered_map<uint32, FillEntry> g_fillBots;
    // leaderGuidLow -> bgTypeId currently being filled (one fill session per leader)
    std::unordered_map<uint32, uint32>    g_activeLeaders;
    // leaderGuidLow -> getMSTime() at queue: fill is PENDING (delayed). The world tick fires
    // StartFill once BG_FILL_DELAY_MS has elapsed and the BG hasn't popped on its own.
    std::unordered_map<uint32, uint32>    g_fillPendingMs;
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

    // CSV of the fill-bot guids already spawned for ANY leader, so a second concurrent
    // fill (or the in-bracket vs out-of-bracket halves of the same draw) can't pick the
    // same parked char twice. "0" is a never-a-guid placeholder so the NOT IN is valid
    // even when nothing is active yet.
    std::string ActiveFillCsv()
    {
        std::string csv = "0";
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto const& kv : g_fillBots) { csv += ','; csv += std::to_string(kv.first); }
        return csv;
    }

    // CSV of ACCOUNTS already claimed by an active fill (any leader, both factions).
    // Only one char per account can be online at once, and a RNDBOT account holds BOTH
    // factions, so a new draw MUST avoid these accounts or its logins silently fail —
    // the cause of the lopsided "40 ally / 23 enemy" AV: the ally fills (drawn first)
    // claimed accounts the enemy Horde draw then collided with. "0" is a never-an-account
    // placeholder so NOT IN stays valid when nothing is active yet.
    std::string ActiveFillAccountCsv()
    {
        std::string csv = "0";
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto const& kv : g_fillBots) { csv += ','; csv += std::to_string(kv.second.account); }
        return csv;
    }

    // Spawn up to `count` parked rndbot chars of the given races as independent bots
    // (master 0) and register them as fill bots for this leader's BG. Draws a
    // CLASS-BALANCED spread — round-robin across classes, one char per account — so a
    // 40v40 isn't a wall of one class. In-bracket chars are preferred WITHIN each class
    // (no re-level); a class that's short in-bracket pulls out-of-bracket chars flagged
    // for a re-level to the bracket on the world tick, so a small flat pool serves every
    // bracket. Returns how many were spawned (pool may be short — logged, never silently
    // capped).
    uint32 SpawnFillTeam(PlayerbotMgr* mgr, uint32 leaderLow, uint32 bgTypeId,
                         std::string const& acctCsv, char const* races,
                         uint8 bmin, uint8 bmax, uint32 count, bool ally, char const* sideLabel)
    {
        if (count == 0) return 0;

        // Claim DISTINCT accounts. Only one char per account can be online at once, and a
        // RNDBOT account holds BOTH factions, so two picks on the same account (or an enemy
        // pick on an account the ally fill already used) collide and the second login
        // silently fails — the lopsided "40 ally / 23 enemy" AV. `exclAccts` = accounts
        // any active fill already holds; `usedAccts` = accounts taken within THIS draw.
        // `GROUP BY account` yields one char per account; the NOT IN excludes claimed ones.
        std::string const exclAccts = ActiveFillAccountCsv();
        std::vector<std::tuple<uint32, uint32, bool>> picks;   // {guid, account, needsRelevel}

        // Pull ALL eligible parked chars (any level) and pick a CLASS-BALANCED spread —
        // one char per account, evenly across classes. The old query took MIN(guid) per
        // account, which ALWAYS chose the warrior (created first => lowest guid), and it
        // preferred already-in-bracket chars (mostly warriors get leveled), so a 40v40
        // came out a wall of warriors even though the pool itself is class-even. We now
        // balance by class and prefer in-bracket chars only WITHIN a class — re-leveling
        // out-of-bracket ones (RelevelFillBot full re-roll) to fill classes short at this
        // bracket. The "account NOT IN (online)" still enforces one online char/account.
        {
            struct Cand { uint32 guid; uint32 acct; };
            std::unordered_map<uint8, std::vector<Cand>> inb, oob;   // per class; ORDER BY RAND() order
            std::vector<uint8> classes;
            if (QueryResult q = CharacterDatabase.Query(
                    "SELECT `guid`, `account`, `class`, `level` FROM `characters` WHERE `account` IN ({}) "
                    "AND `account` NOT IN ({}) AND `account` NOT IN (SELECT `account` FROM `characters` WHERE `online` = 1) "
                    "AND `online` = 0 AND `race` IN ({}) ORDER BY RAND()",
                    acctCsv, exclAccts, races))
            {
                do {
                    Field* f = q->Fetch();
                    uint32 const g   = f[0].Get<uint32>();
                    uint32 const a   = f[1].Get<uint32>();
                    uint8  const cls = f[2].Get<uint8>();
                    uint8  const lvl = f[3].Get<uint8>();
                    bool const inBand = (lvl >= bmin && lvl <= bmax);
                    auto& bucket = inBand ? inb : oob;
                    if (inb.find(cls) == inb.end() && oob.find(cls) == oob.end())
                        classes.push_back(cls);
                    bucket[cls].push_back({ g, a });
                } while (q->NextRow());
            }

            // Round-robin across classes, one pick per class per round, in-bracket
            // preferred, one char per account, until we hit `count` or run dry.
            std::unordered_map<uint32, bool> usedAccts;
            std::unordered_map<uint8, size_t> iIdx, oIdx;
            auto pickFrom = [&](uint8 cls, std::unordered_map<uint8, std::vector<Cand>>& m,
                                std::unordered_map<uint8, size_t>& idx, bool needRelevel) -> bool
            {
                auto it = m.find(cls);
                if (it == m.end()) return false;
                std::vector<Cand>& v = it->second;
                size_t& k = idx[cls];
                while (k < v.size())
                {
                    Cand const c = v[k++];
                    if (usedAccts.count(c.acct)) continue;
                    usedAccts[c.acct] = true;
                    picks.emplace_back(c.guid, c.acct, needRelevel);
                    return true;
                }
                return false;
            };
            bool progress = true;
            while (picks.size() < count && progress)
            {
                progress = false;
                for (uint8 cls : classes)
                {
                    if (picks.size() >= count) break;
                    if (pickFrom(cls, inb, iIdx, false) || pickFrom(cls, oob, oIdx, true))
                        progress = true;
                }
            }
        }

        if (picks.empty())
        {
            LOG_INFO("module", "[WowPsParty BGFill] no parked {} rndbot pool available — {} side short by {}",
                     sideLabel, sideLabel, count);
            return 0;
        }

        uint32 spawned = 0;
        for (auto const& [g, acct, needsRelevel] : picks)
        {
            ObjectGuid const botGuid = ObjectGuid::Create<HighGuid::Player>(g);
            mgr->AddPlayerBot(botGuid, 0);   // master 0 -> isRndbot bypass, no group/follow
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_fillBots[g] = { leaderLow, bgTypeId, getMSTime(), false, ally,
                                  bmin, bmax, !needsRelevel, 0, acct };
            }
            ++spawned;
        }

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
        // Clamp the bracket to the server cap. A bracket whose maxLevel exceeds the cap
        // (or a stale 85 pool char) would otherwise be selected as "in-bracket" and used
        // AS-IS — putting a level-85 bot into a WotLK (cap-80) battleground. With bmax
        // clamped, any >cap char falls into the out-of-bracket query and is re-leveled down.
        uint8 const cap  = uint8(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        uint8 const bmin = std::min<uint8>(uint8(br->minLevel), cap);
        uint8 const bmax = std::min<uint8>(uint8(br->maxLevel), cap);

        std::string const acctCsv = RndbotAccountCsv();
        if (acctCsv.empty()) { LOG_INFO("module", "[WowPsParty BGFill] no rndbot pool — abort"); return; }

        // Fill BOTH teams. (The earlier enemy-only revert was pre-WSG-min-fix: back then
        // same-faction ally fills could give the matchmaker a whole bot-vs-bot match in a
        // premade WSG and enter without the human, stranding the player.) That can't happen
        // now: every participant is in the NORMAL queue, the human's group queued FIRST
        // (synchronously at click time, before these async fill logins), so CheckNormalMatch's
        // FIFO scan always selects it; and no fillable BG flags a <=5 party as a PREMADE —
        // WSG's min is raised to 6 by migration 2026_06_19_00_party_bgfill_wsg_minplayers.sql
        // (its stock min trips a full party: AC seed 5, this server 2) and every other BG's
        // min is already >5. With no premade anywhere there is no same-faction self-match:
        // an ally fill can only ever backfill the human's own match.
        std::vector<ObjectGuid> party;
        WowPsParty::GetPartyGuidsFor(human->GetGUID(), party);
        // human + heroes. A solo player has no party directives (empty vector), so floor at
        // 1: the human always holds one slot on their own side, else the ally side spawns
        // one fill too many (11 on a 10-cap team).
        uint32 const ownSide   = std::max<uint32>(uint32(party.size()), 1);  // human + heroes
        uint32 const allyNeed  = maxPerTeam > ownSide ? maxPerTeam - ownSide : 0;
        uint32 const enemyNeed = maxPerTeam;

        bool const allianceLeader = human->GetTeamId() == TEAM_ALLIANCE;
        uint32 const leaderLow = human->GetGUID().GetCounter();

        uint32 const allies  = SpawnFillTeam(mgr, leaderLow, bgTypeId, acctCsv,
                                             RaceCsv(allianceLeader), bmin, bmax, allyNeed, true, "ally");
        uint32 const enemies = SpawnFillTeam(mgr, leaderLow, bgTypeId, acctCsv,
                                             RaceCsv(!allianceLeader), bmin, bmax, enemyNeed, false, "enemy");

        LOG_INFO("module",
            "[WowPsParty BGFill] {} queued bg {} (bracket {}-{}): ally {}+{} fills, enemy +{} fills -> {}v{}",
            human->GetName(), bgTypeId, bmin, bmax, ownSide, allies, enemies, ownSide + allies, enemies);
        ChatHandler(human->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Forming a full match: +{} allies, +{} opponents…", allies, enemies);
    }

    // Prep-phase deficit TOP-UP. The spawn-at-queue fill (StartFill) can lose the
    // login/relevel/queue race, so a match locks in under-filled (Mill: "joined a 6v6
    // that should've been a 10v10"). This is the safety net: while a tracked leader is in
    // a BG that is either counting down (STATUS_WAIT_JOIN) OR already live
    // (STATUS_IN_PROGRESS — WotLK allows joining a BG in progress with room, the real-server
    // backfill behaviour), keep BOTH teams topped up to MaxPlayersPerTeam by spawning fills
    // for the REAL deficit read off the LIVE instance.
    // Deficit-based and self-throttling — newly-spawned fills are counted as in-flight, so
    // it converges to a full match and stops spawning. Runs every world tick; the cheap
    // both-teams-full check below short-circuits the steady state (a correctly-filled match
    // counting down) BEFORE any DB query, so the DB hits (RndbotAccountCsv + SpawnFillTeam)
    // only run while a deficit actually exists. Called from OnUpdate for each tracked leader.
    void TopUpBgInPrep(Player* leader)
    {
        if (!leader || !leader->IsInWorld() || !leader->InBattleground()) return;
        Battleground* bg = leader->GetBattleground();
        if (!bg || bg->isArena()) return;
        // Backfill both during the pre-gate countdown (STATUS_WAIT_JOIN) AND once the match
        // is LIVE (STATUS_IN_PROGRESS). A fill that lost the login/relevel/queue/port race
        // during prep leaves the match locked in short with no recovery — the 15v10 Arathi
        // Kevin saw (a Random-BG pop where +6 Horde top-ups were spawned at present=9 but
        // only ~1 entered before the gates opened). WotLK allows joining a BG in progress
        // with room, so keep topping the real deficit up mid-match too; the both-teams-full
        // short-circuit below stops it the instant the match is balanced, and it also keeps
        // a match full as real players leave. Only WAIT_LEAVE (match over) is excluded.
        if (bg->GetStatus() != STATUS_WAIT_JOIN && bg->GetStatus() != STATUS_IN_PROGRESS) return;
        uint32 const maxPerTeam = bg->GetMaxPlayersPerTeam();
        if (maxPerTeam == 0) return;
        // Both teams already full -> nothing to do; skip the DB queries below. This is the
        // common steady state for the rest of the prep countdown once the fill succeeded.
        if (bg->GetPlayersCountByTeam(TEAM_ALLIANCE) >= maxPerTeam
            && bg->GetPlayersCountByTeam(TEAM_HORDE) >= maxPerTeam) return;

        PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(leader);
        if (!mgr) return;
        Battleground* tpl = sBattlegroundMgr->GetBattlegroundTemplate(bg->GetBgTypeID());
        if (!tpl) return;
        PvPDifficultyEntry const* br = GetBattlegroundBracketByLevel(tpl->GetMapId(), leader->GetLevel());
        if (!br) return;
        uint8 const cap  = uint8(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        uint8 const bmin = std::min<uint8>(uint8(br->minLevel), cap);
        uint8 const bmax = std::min<uint8>(uint8(br->maxLevel), cap);

        std::string const acctCsv = RndbotAccountCsv();
        if (acctCsv.empty()) return;

        uint32 const leaderLow  = leader->GetGUID().GetCounter();
        uint32 const bgTypeId   = uint32(bg->GetBgTypeID());   // resolved real BG (Random BG -> real type)
        TeamId const leaderTeam = leader->GetTeamId();

        // Heroes (managed party bots) not yet in the BG will take the leader-team slots —
        // count them so the top-up doesn't over-spawn ally fills on top of them.
        uint32 pendingHeroes = 0;
        {
            std::vector<ObjectGuid> party;
            WowPsParty::GetPartyGuidsFor(leader->GetGUID(), party);
            for (ObjectGuid const& g : party)
            {
                if (g == leader->GetGUID()) continue;
                Player* pb = ObjectAccessor::FindConnectedPlayer(g);
                if (pb && sPlayerbotsMgr.GetPlayerbotAI(pb) && !pb->InBattleground()) ++pendingHeroes;
            }
        }

        for (uint8 ti = 0; ti < 2; ++ti)
        {
            TeamId const t = TeamId(ti);                       // TEAM_ALLIANCE=0, TEAM_HORDE=1
            bool   const ally = (t == leaderTeam);
            uint32 const present = bg->GetPlayersCountByTeam(t);

            // My fills already heading to this team (spawned, not yet in) — counted so we
            // don't re-spawn what's already in flight.
            uint32 inFlight = 0;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                for (auto const& kv : g_fillBots)
                {
                    if (kv.second.leaderLow != leaderLow || kv.second.entered) continue;
                    TeamId const feTeam = kv.second.ally
                        ? leaderTeam
                        : (leaderTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE);
                    if (feTeam == t) ++inFlight;
                }
            }

            uint32 accounted = present + inFlight;
            if (ally) accounted += pendingHeroes;              // heroes en route fill this team
            if (accounted >= maxPerTeam) continue;

            uint32 const need = maxPerTeam - accounted;
            uint32 const spawned = SpawnFillTeam(mgr, leaderLow, bgTypeId, acctCsv,
                                                 RaceCsv(t == TEAM_ALLIANCE), bmin, bmax, need, ally,
                                                 ally ? "ally-topup" : "enemy-topup");
            if (spawned)
                LOG_INFO("module",
                    "[WowPsParty BGFill] prep top-up bg {} team {}: present={} inFlight={} pendingHeroes={} -> +{} fills",
                    bgTypeId, uint32(t), present, inFlight, ally ? pendingHeroes : 0u, spawned);
        }
    }

    // Re-level an out-of-bracket pool fill bot to the top of the BG bracket so a small
    // flat pool serves every bracket. PlayerbotFactory::Randomize(false) is the same full
    // re-roll mod-playerbots uses for its random bots (level + gear + talents + spells);
    // it is HEAVY, so the world tick runs at most ONE per tick (see OnUpdate).
    void RelevelFillBot(Player* bot, uint8 level)
    {
        if (!bot || !bot->IsInWorld() || !sPlayerbotsMgr.GetPlayerbotAI(bot)) return;
        // Never re-level a fill bot past the server cap. WotLK max is 80; a stale pool
        // char left at 85 from an old higher-cap config (the "level 85 bots in AV/WG"
        // bug) would otherwise be re-rolled right back to 85 by a bracket whose maxLevel
        // exceeds the cap. Clamp at the single shared relevel point so every caller is safe.
        uint8 const cap = uint8(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        if (level > cap) level = cap;
        PlayerbotFactory factory(bot, level);
        factory.Randomize(false);
        LOG_INFO("module", "[WowPsParty BGFill] re-leveled fill bot {} to {} for the bracket", bot->GetName(), uint32(level));
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

            uint8 const arenaType = BattlegroundMgr::BGArenaType(qt);
            // Resolve the REAL bgTypeId of the invited INSTANCE. For a BG, BGTemplateId(qt)
            // already equals it. But for an ARENA the queue's template is BATTLEGROUND_AA
            // while the live instance is a SPECIFIC arena map (Nagrand / Ruins of Lordaeron /
            // …). HandleBattleFieldPortOpcode looks the instance up by (instanceId, bgTypeId),
            // so sending AA made GetBattleground return null (diagnostic: bgFound=false) and
            // the port was REJECTED — the heroes were invited but never entered (the endless
            // 1v5). The arena QUEUE id is arena-type-based (BGQueueTypeId ignores bgTypeId for
            // arenas), so the specific type still resolves the right queue AND finds the
            // instance. Search ALL types (NONE) for the instance to get its real type.
            Battleground* const invBg =
                sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, BATTLEGROUND_TYPE_NONE);
            BattlegroundTypeId const bgTypeId = invBg ? invBg->GetBgTypeID() : BattlegroundMgr::BGTemplateId(qt);
            if (arenaType != 0)
                LOG_INFO("module",
                         "[WowPsParty BGFill] arena-port {} -> inst={} realBg={} (queued qt={}) combat={} charm={} vehicle={} frozen={}",
                         bot->GetName(), ginfo.IsInvitedToBGInstanceGUID, uint32(bgTypeId), uint32(qt),
                         bot->IsInCombat(), !bot->GetCharmGUID().IsEmpty(), !!bot->GetVehicle(), bot->HasAura(9454));
            // HandleBattleFieldPortOpcode REJECTS the accept — at LOG_DEBUG, invisibly —
            // while the player is charmed, in combat, or GM-frozen (aura 9454). A bot
            // parked inside an ACTIVE Wintergrasp battle got auto-enrolled in the war on
            // login, was re-flagged into combat every tick, and burned its whole 60s
            // invite window on rejected ports: the enemy "5v5" team entered 3- or
            // 4-strong. These are all managed bots, so strip every blocker before the
            // accept (logged raw above, so residual causes stay visible).
            if (bot->GetVehicle()) bot->ExitVehicle();
            if (bot->GetCharmGUID()) bot->RemoveCharmAuras();
            if (bot->HasAura(9454)) bot->RemoveAura(9454);
            if (bot->IsInCombat()) bot->CombatStop(true);
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
        // NEVER log a fill bot out while it's TRANSITIONAL (mid-teleport / not fully
        // in world / mid-cast) — e.g. just leaving a BG. LogoutPlayer then tears down
        // its items + removes it from the world while a map-worker thread is still
        // touching that map, racing Item::GetOwner -> FindPlayer (the recurring crash
        // the capture pinned to a fill-bot retire during a dungeon). Leave it TRACKED
        // and retry next world tick once it's stable.
        if (bot && (bot->IsBeingTeleported() || !bot->IsInWorld()
                    || bot->IsNonMeleeSpellCast(false, false, true)))
            return;
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
            // master-0 (rndbot) fill bots register on sRandomPlayerbotMgr's holder, NOT
            // the leader's PlayerbotMgr (master account 0 == rndbot) — exactly like the
            // arena fill (RetireArenaFill) and WG fill, which both log out THERE. Routing
            // through the LEADER's mgr (the old code) was a NO-OP for these bots: they were
            // erased from g_fillBots but never actually logged out, so the whole fill team
            // stayed in-world after the match, pinning every rndbot account (one online
            // char/account) — after one 40v40, 97/100 accounts were stuck and the next BG
            // filled "+0 allies, +0 opponents". LogoutPlayerBot still queues the teardown
            // onto the world thread + guards double-logout, so the BG-join crash concern is
            // unchanged. If the leader's gone it's irrelevant — the bot is sRandomPlayerbotMgr's.
            if (bot->GetGroup()) bot->RemoveFromGroup();
            sRandomPlayerbotMgr.LogoutPlayerBot(bot->GetGUID());
        }
    }

    // Can this BG type be FILLED with bots? Any non-arena battleground, INCLUDING
    // Random BG. RB doesn't pick its real map until the queue pops, but it has its OWN
    // template capacity (battleground_template ID 32: 10v10, level 80) and the fills go
    // into the RB QUEUE — not a specific BG — so we top BOTH RB-queue sides to RB's
    // MaxPlayersPerTeam, it pops a real BG at 10v10, then TopUpBgInPrep reads the LIVE
    // instance's (real BG's) capacity and backfills it to its true N-v-N during the
    // pre-gate countdown. RB's MaxPlayersPerTeam (10) is <= every rollable BG's cap
    // (WSG's 10 is the smallest), so the initial pop is never over-invited.
    bool IsFillableBg(uint32 bgTypeId)
    {
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

    // ===== Rated ARENA enemy fill (Phase 1) ================================
    // A rated arena queue can only pop against an OPPOSING arena team. When a managed
    // party's leader queues rated, pick a full, opposite-faction bot arena team whose
    // rating is nearest the human's (so it pops promptly AND the result is a real rated
    // win/loss — ratings stay spread), log its members in, group them under the captain,
    // queue the team rated, and drive it to accept the pop. Bot teams already exist in
    // the DB (arena_team); Phase 2 will top them up to 15/bracket + add an hourly
    // bot-vs-bot loop to spread ratings. Mirrors HandleBattlemasterJoinArena's rated path.
    struct ArenaFillSession
    {
        uint32 leaderLow;
        uint8  arenaType;            // 2 / 3 / 5
        uint32 botTeamId;
        std::vector<uint32> members; // bot member guidLows (captain first)
        uint32 spawnMs;
        uint32 humanMmr = 1500;      // queue the bot team at THIS matchmaker rating so it
                                     // pairs with the human within Arena.MaxRatingDifference
                                     // (else a >150 gap waits out the 10-min discard timer)
        bool   queued  = false;
        bool   entered = false;      // a bot made it into the arena instance
        uint32 outSinceMs = 0;       // first tick all bots read !InBattleground after entering
    };
    std::unordered_map<uint32, ArenaFillSession> g_arenaFills;   // leaderLow -> session

    // Dalaran fountain — sanctuary staging for a queued bot arena team. Pool chars log
    // in wherever they last stood; several were parked inside Wintergrasp, where an
    // active war means non-stop combat and a combat-flagged player can never accept the
    // arena port (see AcceptBgInvite). A sanctuary makes combat impossible while the
    // team waits out the pop, and post-match the arena exit returns them here (their
    // entry point), so a staged team can't re-poison itself.
    constexpr uint32 ARENA_STAGING_MAP = 571;
    constexpr float  ARENA_STAGING_X = 5807.98f;
    constexpr float  ARENA_STAGING_Y = 588.49f;
    constexpr float  ARENA_STAGING_Z = 660.94f;
    constexpr float  ARENA_STAGING_O = 1.67f;

    void RetireArenaFill(uint32 leaderLow)
    {
        ArenaFillSession s;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_arenaFills.find(leaderLow);
            if (it == g_arenaFills.end()) return;
            s = it->second;
            g_arenaFills.erase(it);
        }
        for (uint32 g : s.members)
        {
            ObjectGuid const bg = ObjectGuid::Create<HighGuid::Player>(g);
            Player* b = ObjectAccessor::FindConnectedPlayer(bg);
            if (!b || !b->GetSession()) continue;
            if (b->IsBeingTeleported() || !b->IsInWorld()) continue;   // never log out mid-port
            if (b->GetGroup()) b->RemoveFromGroup();
            // master-0 (rndbot) bots register on sRandomPlayerbotMgr's holder, NOT the
            // leader's, so log them out THERE — the WG fill uses the same call.
            sRandomPlayerbotMgr.LogoutPlayerBot(bg);
        }
    }

    // Form the bot team into a group and queue it rated. Mirrors the rated branch of
    // WorldSession::HandleBattlemasterJoinArena (AddGroup + per-member AddBattlegroundQueueId
    // + ScheduleQueueUpdate). Called from the world tick once every member is online.
    void QueueBotArenaTeam(uint32 leaderLow)
    {
        ArenaFillSession s;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_arenaFills.find(leaderLow);
            if (it == g_arenaFills.end() || it->second.queued) return;
            s = it->second;
        }
        ArenaTeam* at = sArenaTeamMgr->GetArenaTeamById(s.botTeamId);
        if (!at) { RetireArenaFill(leaderLow); return; }
        Player* captain = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(s.members.front()));
        if (!captain) { RetireArenaFill(leaderLow); return; }

        // BattlegroundQueue::AddGroup ASSERTs no member is already queued — a stale-queued
        // bot (mid-retire, or pulled into another fill) would crash the worldserver. Bail
        // softly instead; the session retires and the human can re-queue.
        for (uint32 g : s.members)
        {
            Player* m = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g));
            if (m && m->InBattlegroundQueue()) { RetireArenaFill(leaderLow); return; }
        }

        if (captain->GetGroup()) captain->RemoveFromGroup();
        Group* grp = new Group();
        if (!grp->Create(captain)) { delete grp; RetireArenaFill(leaderLow); return; }
        sGroupMgr->AddGroup(grp);
        for (size_t i = 1; i < s.members.size(); ++i)
        {
            Player* m = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(s.members[i]));
            if (!m) continue;
            if (m->GetGroup()) m->RemoveFromGroup();
            grp->AddMember(m);
        }
        // NEVER field a short team: a failed AddMember (or a member evaporating between
        // the all-online check and here) would queue a 4-man "5v5" and the core happily
        // starts the rated match under-manned. Retire instead — the human just re-queues.
        if (grp->GetMembersCount() < uint32(s.members.size()))
        {
            LOG_INFO("module",
                "[WowPsParty ArenaFill] bot team {} grouped only {}/{} members — retiring, not queueing short",
                s.botTeamId, grp->GetMembersCount(), uint32(s.members.size()));
            grp->Disband(true);
            RetireArenaFill(leaderLow);
            return;
        }

        Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_AA);
        if (!bgt) { RetireArenaFill(leaderLow); return; }
        PvPDifficultyEntry const* bracket = GetBattlegroundBracketByLevel(bgt->GetMapId(), captain->GetLevel());
        if (!bracket) { RetireArenaFill(leaderLow); return; }
        uint8 const arenatype = s.arenaType;
        BattlegroundQueueTypeId const qt = BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_AA, arenatype);
        BattlegroundQueue& q = sBattlegroundMgr->GetBattlegroundQueue(qt);
        uint32 rating = at->GetRating();
        if (rating == 0) rating = 1;
        // Queue at the HUMAN's matchmaker rating (not the bot team's own) so the pair is
        // inside Arena.MaxRatingDifference and pops NOW. The bot team's persistent rating
        // (above) is untouched, so the post-match result still updates real ratings.
        uint32 const mmr = s.humanMmr;
        bgt->SetRated(true);
        q.AddGroup(captain, grp, BATTLEGROUND_AA, bracket, arenatype, /*isRated=*/true, /*isPremade=*/false,
                   rating, mmr, at->GetId(), at->GetPreviousOpponents());
        uint32 staged = 0;
        for (GroupReference* itr = grp->GetFirstMember(); itr; itr = itr->next())
            if (Player* mm = itr->GetSource())
            {
                mm->AddBattlegroundQueueId(qt);
                // Stage in the Dalaran sanctuary while the pop lands (see the constants
                // above for why); small offset so the team doesn't stack on one spot.
                mm->TeleportTo(ARENA_STAGING_MAP, ARENA_STAGING_X + 2.0f * staged,
                               ARENA_STAGING_Y, ARENA_STAGING_Z, ARENA_STAGING_O);
                ++staged;
            }
        sBattlegroundMgr->ScheduleQueueUpdate(mmr, arenatype, qt, BATTLEGROUND_AA, bracket->GetBracketId());

        { std::lock_guard<std::mutex> lk(g_mutex);
          auto it = g_arenaFills.find(leaderLow);
          if (it != g_arenaFills.end()) it->second.queued = true; }
        LOG_INFO("module",
            "[WowPsParty ArenaFill] bot team {} queued rated {}v{} (rating {}, mmr {}) to face leader {}",
            at->GetId(), uint32(arenatype), uint32(arenatype), rating, mmr, leaderLow);
    }

    void StartArenaFill(Player* human, uint8 arenaType)
    {
        if (!human || !human->GetSession()) return;
        uint32 const leaderLow = human->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto const it = g_arenaFills.find(leaderLow);
            // Skip ONLY a fill still being fielded for the CURRENT queue (created moments
            // ago, hasn't entered a match) — guards a double-fire of the join hook. Any
            // OTHER leftover must be cleared below so this fresh queue gets its own enemy
            // team: re-queuing right after a WIN landed here while the spent session's
            // teardown grace (BG_OUT_GRACE_MS) hadn't fired yet, so the old bare
            // "one session per leader" guard returned and NO enemy team was ever fielded —
            // "the queue never pops until I leave the queue and requeue".
            if (it != g_arenaFills.end() && !it->second.entered
                && getMSTime() - it->second.spawnMs < 3000)
                return;
        }
        // Clear a spent (already played its match) or stale prior session so it can't
        // block this queue. No-op when there's nothing to retire.
        RetireArenaFill(leaderLow);

        uint8 const slot = ArenaTeam::GetSlotByType(arenaType);
        uint32 const humanTeamId = human->GetArenaTeamId(slot);
        if (!humanTeamId) return;   // not a rated team queue
        ArenaTeam* hat = sArenaTeamMgr->GetArenaTeamById(humanTeamId);
        uint32 humanRating = hat ? hat->GetRating() : 0;
        if (humanRating == 0) humanRating = 1500;   // fresh team — match mid-ladder
        // The human's matchmaker rating drives the pairing window (Arena.MaxRatingDifference).
        // We queue the bot team at THIS so it pops immediately regardless of the team's
        // own (possibly far) rating.
        uint32 humanMmr = (hat && human->GetGroup()) ? hat->GetAverageMMR(human->GetGroup()) : 0;
        if (humanMmr == 0) humanMmr = humanRating;

        bool const allianceHuman = human->GetTeamId() == TEAM_ALLIANCE;
        char const* const oppRaces = RaceCsv(!allianceHuman);   // bot CAPTAIN must be opposite faction (queue bucket)

        // FULL, opposite-faction bot teams with NO member currently online (a busy
        // member can't log in for the match — one char per account), ordered by
        // rating proximity. We pull the nearest BAND (not just the single closest)
        // and pick a RANDOM one for matchup VARIANCE — otherwise the human faces the
        // exact same nearest-rated team every queue. The whole band is a fair match
        // (the bot team is queued at the human's MMR regardless of its own rating).
        QueryResult q = CharacterDatabase.Query(
            "SELECT at.arenaTeamId, at.captainGuid FROM arena_team at "
            "JOIN characters cap ON cap.guid = at.captainGuid "
            "WHERE at.type = {} AND cap.race IN ({}) AND at.arenaTeamId <> {} "
            "AND (SELECT COUNT(*) FROM arena_team_member m WHERE m.arenaTeamId = at.arenaTeamId) = {} "
            "AND NOT EXISTS (SELECT 1 FROM arena_team_member m JOIN characters mc ON mc.guid = m.guid "
            "WHERE m.arenaTeamId = at.arenaTeamId AND mc.online = 1) "
            "ORDER BY ABS(CAST(at.rating AS SIGNED) - {}) ASC LIMIT 12",
            uint32(arenaType), oppRaces, humanTeamId, uint32(arenaType), humanRating);
        if (!q)
        {
            ChatHandler(human->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Rated arena: no free bot team to match right now — try again in a moment.");
            LOG_INFO("module", "[WowPsParty ArenaFill] {} rated {}v{}: no available opposite-faction bot team",
                     human->GetName(), uint32(arenaType), uint32(arenaType));
            return;
        }
        std::vector<std::pair<uint32, uint32>> teamCands;
        do { Field* tf = q->Fetch(); teamCands.emplace_back(tf[0].Get<uint32>(), tf[1].Get<uint32>()); }
        while (q->NextRow());
        auto const& chosen = teamCands[urand(0, uint32(teamCands.size()) - 1)];
        uint32 const teamId     = chosen.first;
        uint32 const captainLow = chosen.second;

        std::vector<uint32> members;
        members.push_back(captainLow);
        if (QueryResult mq = CharacterDatabase.Query(
                "SELECT guid FROM arena_team_member WHERE arenaTeamId = {} AND guid <> {}", teamId, captainLow))
            do { members.push_back(mq->Fetch()[0].Get<uint32>()); } while (mq->NextRow());

        PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(human);
        if (!mgr) return;
        for (uint32 g : members)
            mgr->AddPlayerBot(ObjectGuid::Create<HighGuid::Player>(g), 0);   // master 0 -> rndbot bypass

        { std::lock_guard<std::mutex> lk(g_mutex);
          ArenaFillSession sess;
          sess.leaderLow = leaderLow; sess.arenaType = arenaType; sess.botTeamId = teamId;
          sess.members = members; sess.spawnMs = getMSTime(); sess.humanMmr = humanMmr;
          g_arenaFills[leaderLow] = sess; }
        LOG_INFO("module",
            "[WowPsParty ArenaFill] {} queued rated {}v{} — spawning bot team {} ({} members) to match (humanRating {})",
            human->GetName(), uint32(arenaType), uint32(arenaType), teamId, uint32(members.size()), humanRating);
    }

    // Self-heal managed party bots (henchmen + enrolled alts) orphaned in a FINISHED
    // arena/BG. Unlike the rndbot enemy team — which runs mod-playerbots' BG AI and
    // leaves itself on TIME_TO_AUTOREMOVE — the human's own party bots run only the
    // WowPsParty AI and have NO bg-leave behaviour. They depend entirely on the core's
    // hard auto-close to teleport them out, a cross-map far-teleport their bot session
    // must ack. When that race wedges (commonly: the leader re-queues during the prior
    // match's WAIT_LEAVE window, so a new arena port collides with the old auto-close),
    // the bot is stranded in the dead instance — shows offline, frozen, fixable only by
    // a relog. Leaving promptly the instant the match ends (STATUS_WAIT_LEAVE) closes
    // that window before it can open AND recovers any already-stranded bot. Runs every
    // tick independent of any ArenaFill session (that session retires when the ENEMY
    // team clears, which can be before our henchmen are out). Skips mid-port bots and
    // never touches an IN_PROGRESS match, so a live arena is never disturbed.
    void RecoverOrphanedPartyBots()
    {
        std::vector<ObjectGuid> followers;
        WowPsParty::GetAllFollowers(followers);
        for (ObjectGuid const& g : followers)
        {
            Player* pb = ObjectAccessor::FindConnectedPlayer(g);
            if (!pb || !pb->IsInWorld() || pb->IsBeingTeleported()) continue;
            if (!sPlayerbotsMgr.GetPlayerbotAI(pb)) continue;   // managed bot only
            Battleground* bg = pb->GetBattleground();
            if (!bg || bg->GetStatus() != STATUS_WAIT_LEAVE) continue;
            LOG_INFO("module",
                "[WowPsParty ArenaFill] match over — leaving {} out of finished {} (status WAIT_LEAVE)",
                pb->GetName(), bg->isArena() ? "arena" : "battleground");
            pb->LeaveBattleground(bg);
        }
    }

    // World-tick driver: queue the bot team once its members are online, accept the pop,
    // and retire (log out) the bots once the match ends. Reuses AcceptBgInvite (it ports
    // via a deferred CMSG_BATTLEFIELD_PORT, the same crash-safe path the BG fill uses).
    void DriveArenaFills()
    {
        std::vector<uint32> leaders;
        { std::lock_guard<std::mutex> lk(g_mutex);
          if (g_arenaFills.empty()) return;
          for (auto const& kv : g_arenaFills) leaders.push_back(kv.first); }

        for (uint32 leaderLow : leaders)
        {
            ArenaFillSession s;
            { std::lock_guard<std::mutex> lk(g_mutex);
              auto it = g_arenaFills.find(leaderLow); if (it == g_arenaFills.end()) continue; s = it->second; }

            std::vector<Player*> bots;
            for (uint32 g : s.members)
                if (Player* b = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g)))
                    bots.push_back(b);
            Player* leader = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(leaderLow));

            // Drive the human's OWN heroes/henchmen to ACCEPT the arena pop — they're gated
            // bots that never click "Enter Battle", so without this only the human ports in
            // (the reported 1v5). They run the WowPsParty AI inside the arena, so they fight
            // normally once in; they just need the invite accepted. Runs every tick the
            // session is live, so it catches the invite whenever the pop lands.
            if (leader)
            {
                std::vector<ObjectGuid> party;
                WowPsParty::GetPartyGuidsFor(leader->GetGUID(), party);
                int found = 0, inGrp = 0, inQ = 0, invited = 0, inBg = 0;
                Group* const lgrp = leader->GetGroup();
                for (ObjectGuid const& g : party)
                {
                    if (g == leader->GetGUID()) continue;
                    Player* pb = ObjectAccessor::FindConnectedPlayer(g);
                    if (!pb || !sPlayerbotsMgr.GetPlayerbotAI(pb)) continue;   // managed heroes/henchmen only
                    ++found;
                    if (lgrp && lgrp->IsMember(g)) ++inGrp;
                    if (pb->InBattleground()) { ++inBg; continue; }
                    if (pb->InBattlegroundQueue()) ++inQ;
                    // Does this hero actually have a PENDING INVITE (vs just sitting in queue)?
                    // This is THE missing datum: invited=0 => the match never invited them
                    // (group/queue-info problem); invited>0 but inBg stays 0 => the PORT is
                    // being rejected (group churn / port-handler), not an invite problem.
                    for (uint8 qi = 0; qi < PLAYER_MAX_BATTLEGROUND_QUEUES; ++qi)
                    {
                        BattlegroundQueueTypeId const qt = pb->GetBattlegroundQueueTypeId(qi);
                        if (qt == BATTLEGROUND_QUEUE_NONE) continue;
                        GroupQueueInfo gi;
                        if (sBattlegroundMgr->GetBattlegroundQueue(qt).GetPlayerGroupInfoData(pb->GetGUID(), &gi)
                            && gi.IsInvitedToBGInstanceGUID && gi.RemoveInviteTime)
                        { ++invited; break; }
                    }
                    if (pb->IsBeingTeleported()) continue;
                    AcceptBgInvite(pb);
                }
                // DIAGNOSTIC (throttled): why is it 1v5? grp membership + queue + INVITE status.
                static thread_local std::unordered_map<uint32, uint32> heroLogMs;
                uint32 const nowL = getMSTime();
                uint32& hl = heroLogMs[leaderLow];
                if (nowL - hl > 3000)
                {
                    hl = nowL;
                    LOG_INFO("module",
                        "[WowPsParty ArenaFill] hero-drive leader={} grpSize={} isRaid={} isBGGrp={} heroesFound={} inLeaderGrp={} inArenaQueue={} invited={} inBg={}",
                        leaderLow, lgrp ? uint32(lgrp->GetMembersCount()) : 0u,
                        lgrp && lgrp->isRaidGroup(), lgrp && lgrp->isBGGroup(),
                        found, inGrp, inQ, invited, inBg);
                }
            }

            if (!s.queued)
            {
                if (bots.size() < s.members.size())   // still logging in
                {
                    if (getMSTime() - s.spawnMs > FILL_SPAWN_GRACE_MS)
                    {
                        LOG_INFO("module", "[WowPsParty ArenaFill] bot team {} never fully logged in — retiring", s.botTeamId);
                        RetireArenaFill(leaderLow);
                    }
                    continue;
                }
                // Leader left the queue before we could field the enemy → stand down.
                if (!leader || (!leader->InBattleground() && !leader->InBattlegroundQueue()))
                { RetireArenaFill(leaderLow); continue; }
                QueueBotArenaTeam(leaderLow);
                continue;
            }

            // Queued: accept the pop / track the match.
            bool anyIn = false;
            for (Player* b : bots)
            {
                if (b->InBattleground())
                {
                    anyIn = true;
                    // master-0 bots aren't IsRandomBot(), so AiFactory never gave them a
                    // PvP strategy — without this they stand AFK and the human wins by
                    // default. The "bg" strategy has an isArena() branch, so it drives
                    // arena combat too. Add once (HasStrategy-guarded; re-add resets pathing).
                    if (sPlayerbotAIConfig.randomBotJoinBG)
                        if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(b))
                            if (!ai->HasStrategy("bg", BOT_STATE_NON_COMBAT))
                                ai->ChangeStrategy("+bg", BOT_STATE_NON_COMBAT);
                    continue;
                }
                if (!b->IsBeingTeleported()) AcceptBgInvite(b);
            }
            if (anyIn)
            {
                if (!s.entered)
                { std::lock_guard<std::mutex> lk(g_mutex);
                  auto it = g_arenaFills.find(leaderLow); if (it != g_arenaFills.end()) { it->second.entered = true; it->second.outSinceMs = 0; } }
                continue;
            }
            if (s.entered)
            {
                // Entered, now nobody's in → match ended (grace against a flicker).
                uint32 const now = getMSTime();
                if (!s.outSinceMs)
                { std::lock_guard<std::mutex> lk(g_mutex);
                  auto it = g_arenaFills.find(leaderLow); if (it != g_arenaFills.end()) it->second.outSinceMs = now; continue; }
                if (now - s.outSinceMs < BG_OUT_GRACE_MS) continue;
                RetireArenaFill(leaderLow);
                continue;
            }
            // Queued but never entered: pop timed out / leader bailed → retire after a while.
            if ((!leader || (!leader->InBattleground() && !leader->InBattlegroundQueue()))
                && getMSTime() - s.spawnMs > FILL_SPAWN_GRACE_MS)
                RetireArenaFill(leaderLow);
        }
    }
}

class PartyBgFillPlayerScript : public PlayerScript
{
public:
    PartyBgFillPlayerScript() : PlayerScript("PartyBgFillPlayerScript", {
        PLAYERHOOK_ON_PLAYER_JOIN_BG,
        PLAYERHOOK_ON_PLAYER_JOIN_ARENA
    }) { }

    // Rated arena: a queue can only pop against an opposing arena team, so when a
    // managed party's leader queues rated, field an opposite-faction bot team (see
    // StartArenaFill). The hook fires per group member — react only to the human
    // team LEADER queuing a RATED bracket (i.e. they hold an arena team for the slot).
    void OnPlayerJoinArena(Player* player) override
    {
        if (!WowPsParty::IsEnabled() || !player || !player->GetSession()) return;
        if (sPlayerbotsMgr.GetPlayerbotAI(player)) return;   // a bot queued; only the human leader

        uint8 arenaType = 0;
        BattlegroundQueueTypeId arenaQt = BATTLEGROUND_QUEUE_NONE;
        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const qt = player->GetBattlegroundQueueTypeId(i);
            if (qt == BATTLEGROUND_QUEUE_NONE) continue;
            if (uint8 const at = BattlegroundMgr::BGArenaType(qt)) { arenaType = at; arenaQt = qt; break; }
        }
        if (!arenaType) return;                                            // not an arena queue
        if (Group* g = player->GetGroup())
            if (g->GetLeaderGUID() != player->GetGUID()) return;           // only the team leader
        std::vector<ObjectGuid> party;
        WowPsParty::GetPartyGuidsFor(player->GetGUID(), party);
        if (party.size() < 2) return;                                      // managed party only
        // Only fill for a RATED queue — the hook ALSO fires for skirmish (which needs no
        // team and shouldn't force a rated bot match). The authoritative flag lives on the
        // queue entry the core just created (read it back, as AcceptBgInvite does).
        {
            BattlegroundQueue& q = sBattlegroundMgr->GetBattlegroundQueue(arenaQt);
            GroupQueueInfo gi;
            if (!q.GetPlayerGroupInfoData(player->GetGUID(), &gi) || !gi.IsRated) return;
        }
        StartArenaFill(player, arenaType);
    }

    void OnPlayerJoinBG(Player* player) override
    {
        if (!WowPsParty::IsEnabled() || !player || !player->GetSession()) return;
        if (sPlayerbotsMgr.GetPlayerbotAI(player)) return;   // a bot joined; only react to the human leader

        // Fill for ANY human who queues a BG — solo included, and NOT gated on
        // spawnCompanions: Kevin plays in SOLO MODE (companions OFF) and still wants full
        // bot-filled matches, since BGs never pop naturally on this low-pop server. (Bots
        // were already excluded above; the delayed fire in the world tick lets any real
        // players queue first before StartFill tops both teams to a full N-v-N.)
        uint32 const bgTypeId = PickQueuedBg(player);
        if (!bgTypeId) return;

        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_activeLeaders.count(player->GetGUID().GetCounter())) return;
            g_activeLeaders[player->GetGUID().GetCounter()] = bgTypeId;
            // DELAY the fill: real players get BG_FILL_DELAY_MS to queue first. The world
            // tick fires StartFill once it elapses (and the BG hasn't popped on its own).
            if (IsFillableBg(bgTypeId))
                g_fillPendingMs[player->GetGUID().GetCounter()] = getMSTime();
        }
        if (IsFillableBg(bgTypeId))
            LOG_INFO("module",
                "[WowPsParty BGFill] {} queued bg {} — holding the fill {}s so real players can queue first",
                player->GetName(), bgTypeId, BG_FILL_DELAY_MS / 1000);
        else
            LOG_INFO("module",
                "[WowPsParty BGFill] {} queued bg {} (unfillable) — driving heroes to accept the pop, no enemy fill",
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

        // Rated-arena enemy fill runs on the same 1s cadence (its own state map, so it
        // must run BEFORE the BG-fill early-return below).
        DriveArenaFills();

        // Get our own party bots out of any FINISHED match. Independent of the fill
        // sessions above (they retire when the enemy team clears, our bots may not be
        // out yet) and runs even when no fill is active, so it self-heals strays too.
        RecoverOrphanedPartyBots();

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

        // 1) Drive each fill bot: re-level (if needed) -> queue -> accept -> retire. Only
        //    one heavy re-level runs per tick so a 40v40 spin-up never stalls the world.
        bool releveledThisTick = false;
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
                // Make the fill bot actually PLAY the BG. It was added via
                // AddPlayerBot(guid,0) and never registered in RandomPlayerbotMgr's
                // currentBots, so IsRandomBot() is false and AiFactory NEVER gave it the
                // "bg" strategy — it just stands in its start area / graveyard and only
                // fights what reaches it (Kevin's "BG bots are AFK, never walk anywhere").
                // Add the bg strategy ourselves — the same "+bg" mod-playerbots' own
                // first-bot-to-join workaround uses. Guard on HasStrategy: addStrategy
                // does remove-then-readd, so calling it every tick would re-init the
                // strategy and reset its objective pathing each second — only add it when
                // it's MISSING, which also recovers it if a death/res reset the bot.
                if (sPlayerbotAIConfig.randomBotJoinBG)
                    if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot))
                        if (!ai->HasStrategy("bg", BOT_STATE_NON_COMBAT))
                        {
                            ai->ChangeStrategy("+bg", BOT_STATE_NON_COMBAT);
                            // BGTactics splits jobs by "bg role" (0-9). Bots that accept the
                            // pop themselves roll it in BGStatusAction, but fill bots accept
                            // via AcceptBgInvite and would all stay role 0 — same objective,
                            // no flank split — without this roll.
                            ai->GetAiObjectContext()->GetValue<uint32>("bg role")->Set(urand(0, 9));
                        }
                // Remember it made it in, so when the match ENDS (below) we retire it from
                // HERE — never from the BG-removal hook, where a synchronous LogoutPlayer
                // mid-teardown (×10-40 bots on a BG end) is a crash risk.
                if (!fe.entered || fe.outSinceMs)
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    auto it = g_fillBots.find(botLow);
                    if (it != g_fillBots.end()) { it->second.entered = true; it->second.outSinceMs = 0; }
                }
                continue;
            }
            // Out of the BG after entering. Could be the match ending OR a transient clear
            // of InBattleground (an intra-BG teleport, a tick where the flag flickers) —
            // retiring on the transient case logs an ACTIVE bot out mid-match. Require it to
            // stay out for a grace; a real match-end persists, a glitch comes back (which
            // resets outSinceMs in the InBattleground branch above).
            if (fe.entered)
            {
                uint32 const now = getMSTime();
                if (!fe.outSinceMs)
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    auto it = g_fillBots.find(botLow);
                    if (it != g_fillBots.end()) it->second.outSinceMs = now;
                    continue;   // start the grace; re-check next tick
                }
                if (now - fe.outSinceMs < BG_OUT_GRACE_MS) continue;   // still settling — wait
                RetireFillBot(botLow, bot);
                continue;
            }

            // Pre-pop: if the leader bailed the queue, the fill bots leave too.
            Player* leader = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(fe.leaderLow));
            if (!leaderStillIn(leader, fe.bgTypeId)) { RetireFillBot(botLow, bot); continue; }

            // Out-of-bracket pool bot: re-level it to the bracket BEFORE it queues (a bot
            // queues into its CURRENT level's bracket, so queuing first would land it in the
            // wrong one). The re-roll is heavy -> at most one per tick; the rest wait their
            // turn. In-bracket bots are flagged releveled at spawn and skip straight to queue.
            if (!fe.releveled)
            {
                if (releveledThisTick) continue;
                releveledThisTick = true;
                RelevelFillBot(bot, fe.bmax);
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_fillBots.find(botLow);
                if (it != g_fillBots.end()) it->second.releveled = true;
                continue;
            }

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
                g_fillPendingMs.erase(leaderLow);   // left the queue before the hold elapsed
                bool hasFills = false;
                for (auto const& kv : g_fillBots) if (kv.second.leaderLow == leaderLow) { hasFills = true; break; }
                if (!hasFills) g_activeLeaders.erase(leaderLow);
                continue;
            }
            // Delayed initial fill: once the hold window elapses AND the BG hasn't popped on
            // its own (leader still queued, not yet in a BG), bot-fill both teams. If real
            // players popped it first, skip — TopUpBgInPrep tops up any deficit during prep.
            {
                bool fireFill = false;
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    auto it = g_fillPendingMs.find(leaderLow);
                    if (it != g_fillPendingMs.end())
                    {
                        if (leader->InBattleground())                              // popped on its own
                            g_fillPendingMs.erase(it);
                        else if (getMSTime() - it->second >= BG_FILL_DELAY_MS)
                        { fireFill = true; g_fillPendingMs.erase(it); }
                    }
                }
                if (fireFill)   // StartFill takes g_mutex itself — call it UNLOCKED
                {
                    LOG_INFO("module", "[WowPsParty BGFill] {} fill hold elapsed — filling bg {} now",
                             leader->GetName(), bgTypeId);
                    StartFill(leader, bgTypeId);
                }
            }
            // Keep the match topped up to max during the pre-gate countdown AND while it's
            // live (no-op unless the leader is in a BG in STATUS_WAIT_JOIN or STATUS_IN_PROGRESS
            // with a real deficit — the mid-match backfill that fixes a locked-in 15v10).
            TopUpBgInPrep(leader);
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
// ============================================================================
// Wintergrasp population. WG is an outdoor Battlefield (zone 4197), not a BG instance,
// and mod-playerbots has NO WG AI — so this both POPULATES it (rndbot-pool chars spawned
// server-side, enrolled into the war) and DRIVES an objective AI so it plays like real
// WG instead of a dead zone: attacker siege vehicles spawn at the attacker-held
// workshops and convoy to the fortress gate, foot bots capture workshops / raid or
// hold the southern towers / garrison the keep, movement is routed through the front
// gate (never "through" a standing wall), and the battle progresses gate -> relic
// door -> relic click. Everything is gated behind IsWarTime(): nothing runs outside
// an active battle, and all fills are torn down when it ends.
// ============================================================================
constexpr uint32 WG_ZONE_ID         = 4197;
constexpr uint32 WG_MAP_ID          = 571;
constexpr uint32 WG_TARGET_PER_SIDE = 30;
constexpr uint32 WG_SPAWN_PER_TICK  = 3;     // ramp gradually; never spike the world thread
constexpr uint8  WG_RELEVEL_TO      = 80;
constexpr float  WG_ENGAGE_RANGE    = 45.0f;

struct WgBot { uint32 team; bool releveled; bool enrolled; uint32 spawnMs; uint32 account; };
std::unordered_map<uint32, WgBot> g_wgBots;   // guidLow -> state
std::mutex                        g_wgMutex;

// ---- Attacker siege vehicles -------------------------------------------------
// Real-WG siege flow (Kevin): vehicles SPAWN AT the siege workshops the attacker
// controls, drive the road (over the canyon bridges from the southern workshops) to
// the fortress front, and batter the assault corridor's obstacles in order — the
// front gate, then the relic door. The old spawn anchored on the nearest wall GO at
// the wall's Z, which put vehicles on top of walls / next to the core. Each vehicle
// is spawned attacker-faction and, when one's available, an idle attacking fill bot
// is seated as the driver for flavour; motion + damage are driven server-side.
constexpr uint32 WG_MAX_VEHICLES      = 5;
constexpr float  WG_VEH_WALL_RANGE    = 16.0f;    // within this of the target -> batter it
constexpr uint32 WG_VEH_WALL_DMG_DEN  = 25;       // damage/tick = target maxHealth / this (~50s solo)
constexpr float  WG_LEG_REACH         = 15.0f;    // a route leg counts as reached within this
constexpr uint32 WG_LEG_TIMEOUT_MS    = 90000;    // stuck on a leg this long -> skip to the next
constexpr uint32 WG_VEH_SPAWN_CD_MS   = 15000;    // min gap between vehicle spawns — a farmed
                                                  // convoy respawns as a trickle, not a 2s-tick
                                                  // pile-up stacking on the workshop pad
struct WgPt { float x, y, z; };
struct WgVehicle
{
    ObjectGuid driver;
    uint32 spawnMs;
    std::vector<WgPt> legs;   // convoy route: workshop -> road/bridge -> gate approach
    size_t leg = 0;
    uint32 legSinceMs = 0;
    float steerX = 0.0f;      // last point SteerVehicle issued — a changed corridor
    float steerY = 0.0f;      // target re-steers immediately instead of after arrival
};
std::unordered_map<ObjectGuid, WgVehicle> g_wgVehicles;   // vehicle GUID -> state

// Random attacker siege creatures. Siege Engine is faction-specific; demolisher/catapult
// are shared. (Entries from BattlefieldWG.h.)
uint32 const WG_VEH_SIEGE_ALLY  = 28312;
uint32 const WG_VEH_SIEGE_HORDE = 32627;
uint32 const WG_VEH_DEMOLISHER  = 28094;
uint32 const WG_VEH_CATAPULT    = 27881;

// Real WG objective coordinates (live-DB factory-banner + core tower positions) so pathing
// stays on the navmesh. Bots are fanned ACROSS these by faction + guid-hash so the sides
// SPREAD and contest the whole map instead of all bunching at the keep.
WgPt const WG_KEEP     { 5345.0f, 2842.0f, 410.0f };   // keep centre (defender staging)
WgPt const WG_RELIC    { 5440.0f, 2840.0f, 430.0f };   // Titan relic / core (final objective)
// The 4 outer Siege-Workshop capture banners; index == core WintergraspWorkshopIds
// (BATTLEFIELD_WG_WORKSHOP_NE/NW/SE/SW — verified against the live gameobject table).
WgPt const WG_WS[4] = {
    { 4949.3f, 2432.6f, 320.2f },   // NE (GO 190475)
    { 4948.5f, 3342.3f, 376.9f },   // NW (GO 190487)
    { 4398.1f, 2356.5f, 376.2f },   // SE (GO 194959) — attacker-held at battle start
    { 4390.8f, 3304.1f, 372.4f },   // SW (GO 194962) — attacker-held at battle start
};
// The three southern attack towers (base positions from the core spawn table).
WgPt const WG_TOWER_SHADOWSIGHT { 4557.2f, 3623.9f, 395.9f };   // GO 190356 (west)
WgPt const WG_TOWER_WINTERSEDGE { 4398.2f, 2822.5f, 405.6f };   // GO 190357 (centre)
WgPt const WG_TOWER_FLAMEWATCH  { 4459.1f, 1944.3f, 435.0f };   // GO 190358 (east)

// The fortress assault corridor, south -> north along y~2841. The keep walls are
// destructible GAMEOBJECTS, which the navmesh knows nothing about — a straight
// MovePoint "walks through" them. The fortress is therefore modelled as STAGES whose
// only legit crossings are the corridor openings: outdoors -> [front gate, breachable]
// -> front courtyard -> [inner-ring arch GO 191805, always open] -> keep ->
// [vault door, breachable] -> relic vault. All routed movement steps stage by stage.
uint32 const WG_GO_FRONT_GATE = 190375;   // "Wintergrasp Fortress Gate" (outer ring, faces south)
uint32 const WG_GO_LAST_DOOR  = 191810;   // the relic vault door (breach -> relic clickable)
WgPt const WG_GATE_APPROACH { 5090.0f, 2841.0f, 407.0f };   // siege line just outside the gate
WgPt const WG_GATE          { 5163.0f, 2841.2f, 410.2f };
WgPt const WG_COURTYARD     { 5230.0f, 2841.0f, 409.3f };
WgPt const WG_INNER_ARCH    { 5279.1f, 2840.8f, 409.8f };   // open archway through the inner ring (GO 191805, "wall with passage")
WgPt const WG_INNER_COURT   { 5340.0f, 2841.0f, 409.8f };   // keep floor between the arch and the vault ramp
WgPt const WG_VAULT_DOOR    { 5397.1f, 2841.5f, 425.9f };   // the relic vault door GO (top of the ramp)
WgPt const WG_CORE_GATE_GUARD { 5382.0f, 2841.0f, 424.0f }; // defender stand on the ramp in FRONT of the vault door

// Which fortress stage a position is in: 0 = outdoors, 1 = front courtyard,
// 2 = keep, 3 = relic vault. The wall boxes come off the core's building spawn
// table (BattlefieldWG.h): front section walls at x~5163/5280 spanning y 2747..2934;
// keep section out to x~5510 spanning y 2630..3050; the vault door line at x~5397.
int WgStage(float x, float y)
{
    if (x < 5163.0f) return 0;
    if (x < 5280.0f) return (y >= 2747.0f && y <= 2934.0f) ? 1 : 0;
    if (x <= 5510.0f && y >= 2630.0f && y <= 3050.0f) return x < 5397.0f ? 2 : 3;
    return 0;
}

// Road waypoints from the workshops to the fortress front. The southern workshops sit
// across the central canyon, so their routes cross the bridges.
WgPt const WG_BRIDGE_CENTER { 4526.5f, 2810.2f, 391.2f };
WgPt const WG_BRIDGE_WEST   { 4573.0f, 3475.5f, 363.0f };
WgPt const WG_ROAD_CENTER   { 4870.0f, 2820.0f, 380.0f };
WgPt const WG_ROAD_WEST     { 4950.0f, 3340.0f, 377.0f };
WgPt const WG_MID_EAST      { 5060.0f, 2650.0f, 380.0f };
WgPt const WG_MID_WEST      { 5060.0f, 3080.0f, 380.0f };

std::vector<WgPt> WgVehicleRoute(uint8 workshopId)
{
    switch (workshopId)
    {
        case BATTLEFIELD_WG_WORKSHOP_NE: return { WG_MID_EAST, WG_GATE_APPROACH };
        case BATTLEFIELD_WG_WORKSHOP_NW: return { WG_MID_WEST, WG_GATE_APPROACH };
        case BATTLEFIELD_WG_WORKSHOP_SE: return { WG_BRIDGE_CENTER, WG_ROAD_CENTER, WG_GATE_APPROACH };
        default:                         return { WG_BRIDGE_WEST, WG_ROAD_WEST, WG_MID_WEST, WG_GATE_APPROACH };
    }
}

// Ground-snap a waypoint z so spawns/legs sit on the terrain regardless of how rough
// the hint is (the hint is only trusted when no ground resolves).
float WgSnapZ(Map* map, float x, float y, float zHint)
{
    if (!map) return zHint;
    float const z = map->GetHeight(x, y, zHint + 60.0f, true, 120.0f);
    return z > INVALID_HEIGHT ? z : zHint;
}

// Destructible-building state straight off the battlefield's own tracking — a grid
// scan only sees ~530y around the searcher, this works from anywhere on the map.
bool WgBuildingDestroyed(BattlefieldWG* bf, uint32 entry)
{
    for (BfWGGameObjectBuilding* b : bf->GetBuildingsInZone())
        if (b && b->m_Build.GetEntry() == entry)
            return b->m_State == BATTLEFIELD_WG_OBJECTSTATE_ALLIANCE_DESTROY
                || b->m_State == BATTLEFIELD_WG_OBJECTSTATE_HORDE_DESTROY
                || b->m_State == BATTLEFIELD_WG_OBJECTSTATE_NEUTRAL_DESTROY;
    return false;
}

GameObject* WgBuildingGo(BattlefieldWG* bf, uint32 entry)
{
    for (BfWGGameObjectBuilding* b : bf->GetBuildingsInZone())
        if (b && b->m_Build.GetEntry() == entry)
            return bf->GetGameObject(b->m_Build);
    return nullptr;
}

// Outer workshops (NE/NW/SE/SW) currently NOT held by `team`, as capture targets.
// Sorted by id — WorkshopsList is a set of pointers, so raw iteration order is
// address order and would reshuffle objectives every restart.
std::vector<uint8> WgWorkshopsNotHeldBy(BattlefieldWG* bf, TeamId team, bool northOnly)
{
    std::vector<uint8> out;
    for (WGWorkshop* ws : bf->GetWorkshopsList())
    {
        if (!ws || ws->workshopId >= BATTLEFIELD_WG_WORKSHOP_KEEP_WEST) continue;
        if (northOnly && ws->workshopId != BATTLEFIELD_WG_WORKSHOP_NE
                      && ws->workshopId != BATTLEFIELD_WG_WORKSHOP_NW) continue;
        if (ws->teamControl != team) out.push_back(ws->workshopId);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Every outer workshop the attacker holds, for vehicle spawns (sorted for a stable
// round-robin; empty = the defenders flipped every workshop, so no siege can be
// fielded: real WG rules). Spawning across ALL held workshops instead of only the
// nearest one keeps a replacement convoy from piling onto one spawn pad.
std::vector<uint8> WgAttackerWorkshops(BattlefieldWG* bf, TeamId atk)
{
    std::vector<uint8> held;
    for (WGWorkshop* ws : bf->GetWorkshopsList())
        if (ws && ws->workshopId < BATTLEFIELD_WG_WORKSHOP_KEEP_WEST && ws->teamControl == atk)
            held.push_back(ws->workshopId);
    std::sort(held.begin(), held.end());
    return held;
}

// Per-bot objective, aware of the LIVE battle state (workshop control, gate breach,
// relic door). ATTACKERS besiege from OUTSIDE until the front gate falls — capture
// workshops for vehicle slots, hold their towers, mass at the siege line — then pour
// up the assault corridor. DEFENDERS garrison the fortress, field recapture/tower
// details, and collapse onto the relic room the moment its door is breached. (Foot
// bots fight whatever enemy they pass on the way — DriveAI engages before this.)
WgPt WgObjectiveFor(Player* bot, Battlefield* wg)
{
    BattlefieldWG* bfwg = static_cast<BattlefieldWG*>(wg);
    bool const defender = (bot->GetTeamId() == wg->GetDefenderTeam());
    uint32 const h = bot->GetGUID().GetCounter();

    // Vault door breached -> endgame: attackers rush the relic; defenders make their
    // stand at the CORE GATE (the vault-ramp doorway) — blocking the way in, not
    // AFK-stacked on top of the relic itself.
    if (bfwg->CanInteractWithRelic())
        return defender ? WG_CORE_GATE_GUARD : WG_RELIC;

    bool const gateDown = WgBuildingDestroyed(bfwg, WG_GO_FRONT_GATE);

    if (defender)
    {
        // ~30% field details while the walls hold: retake a lost northern workshop
        // (deny the attacker the close vehicle spawns), else raid the southern towers
        // (destroying all three shortens the battle — the real defender play).
        if (h % 10 < 3 && !gateDown)
        {
            std::vector<uint8> const lost = WgWorkshopsNotHeldBy(bfwg, bot->GetTeamId(), /*northOnly=*/true);
            if (!lost.empty())
                return WG_WS[lost[h % lost.size()]];
            switch (h % 3)
            {
                case 0:  return WG_TOWER_SHADOWSIGHT;
                case 1:  return WG_TOWER_WINTERSEDGE;
                default: return WG_TOWER_FLAMEWATCH;
            }
        }
        if (gateDown)
            return (h & 1) ? WG_COURTYARD : WG_INNER_ARCH;   // plug the breach + hold the inner choke
        static WgPt const GARRISON[3] = { { 5192.0f, 2841.0f, 409.3f },    // inside the gate
                                          { 5255.0f, 2954.0f, 409.0f },    // west courtyard
                                          { 5255.0f, 2728.0f, 409.0f } };  // east courtyard
        return GARRISON[h % 3];
    }

    // Attackers.
    if (gateDown)
        return (h % 3 == 0) ? WG_COURTYARD : WG_INNER_COURT;   // pour up the corridor toward the vault
    if (h % 10 < 2)   // small detail defends the towers the attackers own
    {
        switch (h % 3)
        {
            case 0:  return WG_TOWER_SHADOWSIGHT;
            case 1:  return WG_TOWER_WINTERSEDGE;
            default: return WG_TOWER_FLAMEWATCH;
        }
    }
    std::vector<uint8> const caps = WgWorkshopsNotHeldBy(bfwg, bot->GetTeamId(), /*northOnly=*/false);
    if (!caps.empty() && (h & 1))
        return WG_WS[caps[h % caps.size()]];   // flip workshops -> more vehicle slots
    return WG_GATE_APPROACH;                   // escort the siege at the gate front
}

// Move `bot` toward (tx,ty,tz) without ghosting through standing fortress walls:
// stage crossings go through the gate / inner arch / vault door in corridor order.
// A blocked crossing (intact gate or vault door) holds attackers at the obstacle's
// approach; defenders hop an INTACT front gate via a keep-portal-style teleport
// (exactly how defenders really leave/re-enter the WG fortress). stopDist tightens
// the arrive threshold for targets that must be reached exactly (the relic click).
void WgMoveRouted(Player* bot, Battlefield* wg, float tx, float ty, float tz, float stopDist = 10.0f)
{
    int const bs = WgStage(bot->GetPositionX(), bot->GetPositionY());
    int const ts = WgStage(tx, ty);
    if (bs != ts)
    {
        BattlefieldWG* bfwg = static_cast<BattlefieldWG*>(wg);
        bool const deeper = ts > bs;
        int const boundary = deeper ? bs : bs - 1;   // 0 = gate, 1 = arch, 2 = vault door
        float const spread = float(int(bot->GetGUID().GetCounter() % 5) - 2) * 3.0f;
        if (boundary == 0)
        {
            if (!WgBuildingDestroyed(bfwg, WG_GO_FRONT_GATE))
            {
                if (bot->GetTeamId() == wg->GetDefenderTeam())
                {
                    if (bot->IsInCombat() || bot->IsBeingTeleported()) return;   // no combat portal-hopping
                    WgPt const& dst = deeper ? WG_COURTYARD : WG_GATE_APPROACH;
                    bot->TeleportTo(WG_MAP_ID, dst.x, dst.y,
                                    WgSnapZ(bot->GetMap(), dst.x, dst.y, dst.z), deeper ? 0.0f : 3.14f);
                    return;
                }
                if (!deeper)
                {
                    // Attacker INSIDE with the gate still standing is a wall-ghost anomaly
                    // (pre-fix stragglers, a bad res spot) — put it back on the siege line.
                    if (!bot->IsBeingTeleported())
                        bot->TeleportTo(WG_MAP_ID, WG_GATE_APPROACH.x, WG_GATE_APPROACH.y, WG_GATE_APPROACH.z, 3.14f);
                    return;
                }
                // Attacker vs an intact gate: hold the siege line and let the vehicles work.
                tx = WG_GATE_APPROACH.x; ty = WG_GATE_APPROACH.y + spread; tz = WG_GATE_APPROACH.z;
            }
            else if (bot->GetExactDist2d(WG_GATE.x, WG_GATE.y) > 25.0f)
            { tx = WG_GATE.x; ty = WG_GATE.y + spread; tz = WG_GATE.z; }
        }
        else if (boundary == 1)
        {
            if (bot->GetExactDist2d(WG_INNER_ARCH.x, WG_INNER_ARCH.y) > 15.0f)
            { tx = WG_INNER_ARCH.x; ty = WG_INNER_ARCH.y + spread * 0.5f; tz = WG_INNER_ARCH.z; }
        }
        else
        {
            if (!WgBuildingDestroyed(bfwg, WG_GO_LAST_DOOR))
            { tx = WG_CORE_GATE_GUARD.x; ty = WG_CORE_GATE_GUARD.y + spread; tz = WG_CORE_GATE_GUARD.z; }
            else if (bot->GetExactDist2d(WG_VAULT_DOOR.x, WG_VAULT_DOOR.y) > 12.0f)
            { tx = WG_VAULT_DOOR.x; ty = WG_VAULT_DOOR.y + spread * 0.5f; tz = WG_VAULT_DOOR.z; }
        }
    }
    if (!bot->isMoving() && bot->GetExactDist2d(tx, ty) > stopDist)
        bot->GetMotionMaster()->MovePoint(0, tx, ty, tz);
}

class PartyWgFillWorldScript : public WorldScript
{
public:
    PartyWgFillWorldScript() : WorldScript("PartyWgFillWorldScript") {}

    void OnUpdate(uint32 diff) override
    {
        _accum += diff;
        if (_accum < 2000) return;   // 2s cadence
        _accum = 0;
        if (!WowPsParty::IsEnabled()) return;

        Battlefield* wg = sBattlefieldMgr->GetBattlefieldToZoneId(WG_ZONE_ID);
        if (!wg || !wg->IsWarTime())
        {
            DespawnVehicles();                // war over -> despawn siege vehicles + clear tracking
            DespawnSome(WG_SPAWN_PER_TICK);   // no active battle -> tear fills down gradually
            return;
        }

        std::vector<std::pair<uint32, WgBot>> fills;
        { std::lock_guard<std::mutex> lk(g_wgMutex); fills.assign(g_wgBots.begin(), g_wgBots.end()); }

        uint32 haveA = 0, haveH = 0;
        bool releveledThisTick = false;
        for (auto const& [botLow, wb] : fills)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(botLow));
            if (!bot)
            {
                if (getMSTime() - wb.spawnMs > 60000) Forget(botLow);   // never loaded -> drop
                continue;
            }
            // A bot that never gets enrolled within a window (sub-level, stuck out of zone,
            // a battlefield that keeps no-op'ing the invite) just wastes a slot AND counts
            // toward `have`, suppressing TopUp. Drop it so a fresh one replaces it.
            if (!wb.enrolled && getMSTime() - wb.spawnMs > 90000)
            {
                sRandomPlayerbotMgr.LogoutPlayerBot(bot->GetGUID());
                Forget(botLow);
                continue;
            }
            if (bot->GetTeamId() == TEAM_ALLIANCE) ++haveA; else ++haveH;

            if (!wb.releveled)
            {
                // "In bracket" = within [75, cap]. A bot ABOVE the cap (a stale level-85
                // pool char from an old higher-cap config) must be re-leveled DOWN — the
                // old ">= 75" fast-path kept it as-is, which is exactly how level-85 bots
                // leaked into Wintergrasp (and got re-drawn into BG fills).
                uint8 const cap = uint8(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
                if (bot->GetLevel() >= 75 && bot->GetLevel() <= cap) { Mark(botLow, true, wb.enrolled); continue; }
                if (releveledThisTick) continue;                                          // one heavy re-roll/tick
                releveledThisTick = true;
                RelevelFillBot(bot, WG_RELEVEL_TO);
                Mark(botLow, true, wb.enrolled);
                continue;
            }
            if (bot->GetZoneId() != WG_ZONE_ID) { TeleportToStaging(bot, wg); continue; }
            if (!wb.enrolled)
            {
                // Only mark enrolled once the battlefield ACTUALLY lists the bot as a war
                // participant. InvitePlayerToWar silently no-ops on sub-level / in-BG / mid-
                // teleport, so blindly marking it would leave a ghost that DriveAI steers
                // around but the battle never counts. Retry until it takes (bounded by the
                // 90s max-lifetime drop above).
                if (!IsInWar(wg, bot))
                {
                    wg->InvitePlayerToWar(bot);
                    wg->PlayerAcceptInviteToWar(bot);
                }
                if (IsInWar(wg, bot)) Mark(botLow, true, true);
                continue;
            }
            // Defence-in-depth vs the battlefield-raid master adoption (see the isBFGroup
            // guard in PlayerbotAI::UpdateAIGroupMaster): a WG fill must never anchor to
            // a human — strip any master/follow that slipped through another path.
            if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot))
            {
                if (ai->GetMaster()) ai->SetMaster(nullptr);
                if (ai->HasStrategy("follow", BOT_STATE_NON_COMBAT))
                    ai->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
            }
            DriveAI(bot, wg);
        }

        TopUp(wg, TEAM_ALLIANCE, haveA);
        TopUp(wg, TEAM_HORDE,    haveH);

        EnrolHeroes(wg);                              // bring the human's heroes into the war (the Icecrown fix)
        uint32 const liveVeh = DriveVehicles(wg);     // drive attacker siege vehicles into the walls + batter them
        TopUpVehicles(wg, liveVeh);                   // keep up to WG_MAX_VEHICLES attacker vehicles in the field
    }

private:
    uint32 _accum = 0;

    static void Mark(uint32 botLow, bool releveled, bool enrolled)
    {
        std::lock_guard<std::mutex> lk(g_wgMutex);
        auto it = g_wgBots.find(botLow);
        if (it != g_wgBots.end()) { it->second.releveled = releveled; it->second.enrolled = enrolled; }
    }
    static void Forget(uint32 botLow)
    {
        std::lock_guard<std::mutex> lk(g_wgMutex);
        g_wgBots.erase(botLow);
    }

    // Is the bot actually counted as a war participant (vs merely invited/teleported in)?
    static bool IsInWar(Battlefield* wg, Player* bot)
    {
        GuidUnorderedSet const& set = wg->GetPlayersInWarSet(bot->GetTeamId());
        return set.find(bot->GetGUID()) != set.end();
    }

    // Drop the bot at its team's staging point so the war-enrol (next tick) takes over.
    static void TeleportToStaging(Player* bot, Battlefield* wg)
    {
        TeamId const team = bot->GetTeamId();
        if (team == wg->GetDefenderTeam()) bot->TeleportTo(WG_MAP_ID, WG_KEEP.x, WG_KEEP.y, WG_KEEP.z, 3.14f);
        else if (team == TEAM_HORDE)       bot->TeleportTo(WG_MAP_ID, 5025.857f, 3674.629f, 362.737f, 4.135f);
        else                               bot->TeleportTo(WG_MAP_ID, 5101.284f, 2186.564f, 365.549f, 3.812f);
    }

    // Advance-and-engage: in combat -> the class combat AI fights; otherwise grab the
    // nearest enemy war participant, or push toward an objective so the sides converge.
    static void DriveAI(Player* bot, Battlefield* wg)
    {
        if (!bot->IsAlive()) return;                          // dead -> battlefield res handles it
        if (bot->GetVehicle()) return;                        // seated in a siege vehicle — the convoy drives it
        // WG is force-PvP for real players (zone 4197 carries AREA_FLAG_WINTERGRASP ->
        // hostile-area flag on zone update), but teleported-in bots miss/lose that update
        // and fought unflagged until they attacked (Kevin). Enforce it every drive tick.
        if (!bot->IsPvP()) bot->UpdatePvP(true, true);
        if (bot->IsInCombat() && bot->GetVictim()) return;    // fighting -> combat AI owns it
        if (bot->IsNonMeleeSpellCast(false, false, true)) return;

        // Attacker endgame FIRST — before any enemy-chasing: claim the relic, which is
        // how a WG assault actually ends. Ranked above the engage scan so a won siege
        // converges on the relic instead of being diverted by lingering defenders every
        // tick until the battle times out. Approach it EXACTLY (no objective jitter,
        // tight stop) and click from interact range: the old 8y check off a jittered
        // 10y-stop walk left bots permanently a few yards out of click range (Kevin:
        // "attackers don't click the core"). ProcessEvent is invoked directly so ending
        // the battle never depends on the relic GO's goober-event wiring.
        if (bot->GetTeamId() != wg->GetDefenderTeam())
            if (BattlefieldWG* bfwg = static_cast<BattlefieldWG*>(wg); bfwg->CanInteractWithRelic())
                if (GameObject* relic = bfwg->GetRelic())
                {
                    if (bot->IsWithinDistInMap(relic, 10.0f))
                    {
                        LOG_INFO("module", "[WowPsParty WGFill] attacker {} claims the Titan relic", bot->GetName());
                        relic->Use(bot);
                        bfwg->ProcessEvent(relic, 0);   // no-ops if Use() already ended the war
                        return;
                    }
                    WgMoveRouted(bot, wg, relic->GetPositionX(), relic->GetPositionY(),
                                 relic->GetPositionZ(), /*stopDist=*/4.0f);
                    return;
                }

        TeamId const enemyTeam = bot->GetTeamId() == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
        Unit* enemy = nullptr; float bestD = WG_ENGAGE_RANGE;
        for (ObjectGuid const& g : wg->GetPlayersInWarSet(enemyTeam))
        {
            Player* e = ObjectAccessor::FindConnectedPlayer(g);
            if (!e || !e->IsAlive() || e->GetMapId() != bot->GetMapId()) continue;
            if (!bot->IsValidAttackTarget(e)) continue;
            float const d = bot->GetDistance(e);
            if (d < bestD) { bestD = d; enemy = e; }
        }
        if (enemy)
        {
            // Never chase THROUGH a standing wall: LoS respects destructible-GO state,
            // so no sight line means a wall is in the way — walk the routed corridor
            // toward the enemy instead of ghosting a straight MoveChase at them.
            if (!bot->IsWithinLOSInMap(enemy))
            {
                WgMoveRouted(bot, wg, enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ());
                return;
            }
            if (bot->GetVictim() != enemy)
            {
                bot->Attack(enemy, true);
                bot->GetMotionMaster()->MoveChase(enemy);   // (re)chase the new/closer target
            }
            else if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
                bot->GetMotionMaster()->MoveChase(enemy);
            return;
        }

        // No enemy PLAYER in range. A DEFENDER heads for the nearest attacking SIEGE
        // VEHICLE — that's where the real fight is (vehicles batter the walls), exactly how
        // people play WG (Mill). Attack it if in range, else march to intercept. Attackers
        // (and a defender with no vehicle to chase) fall back to the keep objective.
        if (bot->GetTeamId() == wg->GetDefenderTeam())
        {
            std::vector<ObjectGuid> vguids;
            { std::lock_guard<std::mutex> lk(g_wgMutex); for (auto const& kv : g_wgVehicles) vguids.push_back(kv.first); }
            Creature* veh = nullptr; float vd = 300.0f;
            for (ObjectGuid const& vg : vguids)
            {
                Creature* v = ObjectAccessor::GetCreature(*bot, vg);
                if (!v || !v->IsAlive() || v->GetMapId() != bot->GetMapId()) continue;
                float const d = bot->GetDistance(v);
                if (d < vd) { vd = d; veh = v; }
            }
            if (veh)
            {
                if (vd <= WG_ENGAGE_RANGE && bot->IsValidAttackTarget(veh))
                {
                    if (bot->GetVictim() != veh) { bot->Attack(veh, true); bot->GetMotionMaster()->MoveChase(veh); }
                    else if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
                        bot->GetMotionMaster()->MoveChase(veh);
                }
                else
                    WgMoveRouted(bot, wg, veh->GetPositionX(), veh->GetPositionY(), veh->GetPositionZ());
                return;
            }
        }

        // No enemy/vehicle in range -> head to this bot's assigned OBJECTIVE so the sides
        // spread across the map (workshops / towers / breach) instead of all bunching. A
        // small per-bot jitter keeps a shared objective from stacking pixel-perfect, and
        // WgMoveRouted keeps the walk honest around the fortress walls.
        uint32 const guidLow = bot->GetGUID().GetCounter();
        WgPt const obj = WgObjectiveFor(bot, wg);
        float const tx = obj.x + float(int(guidLow % 7) - 3) * 4.0f;
        float const ty = obj.y + float(int(guidLow / 7 % 7) - 3) * 4.0f;
        WgMoveRouted(bot, wg, tx, ty, obj.z);
    }

    // ---- Attacker siege vehicles + hero war-enrolment --------------------------------

    // The human's party HEROES (managed bots) queued WG with them but never accepted the
    // pop, so they sat in Icecrown while the human fought alone (Mill). Enrol any managed
    // hero whose human leader is IN the war: invite + accept + teleport to staging, exactly
    // like the rndbot fills. Bounded by the battlefield's own war-vacancy checks.
    static void EnrolHeroes(Battlefield* wg)
    {
        for (uint8 ti = 0; ti < 2; ++ti)
        {
            TeamId const team = TeamId(ti);
            std::vector<ObjectGuid> participants(wg->GetPlayersInWarSet(team).begin(),
                                                 wg->GetPlayersInWarSet(team).end());
            for (ObjectGuid const& pg : participants)
            {
                Player* leader = ObjectAccessor::FindConnectedPlayer(pg);
                if (!leader || sPlayerbotsMgr.GetPlayerbotAI(leader)) continue;   // humans only
                std::vector<ObjectGuid> party;
                WowPsParty::GetPartyGuidsFor(leader->GetGUID(), party);
                for (ObjectGuid const& hg : party)
                {
                    if (hg == leader->GetGUID()) continue;
                    Player* hero = ObjectAccessor::FindConnectedPlayer(hg);
                    if (!hero || !sPlayerbotsMgr.GetPlayerbotAI(hero)) continue;   // managed heroes only
                    if (hero->GetTeamId() != leader->GetTeamId()) continue;
                    if (IsInWar(wg, hero))
                    {
                        // Enrolled heroes get the same PvP enforcement as the fills —
                        // they miss/lose the zone's force-flag update just like them.
                        if (!hero->IsPvP()) hero->UpdatePvP(true, true);
                        continue;
                    }
                    if (hero->GetZoneId() != WG_ZONE_ID) { TeleportToStaging(hero, wg); continue; }
                    wg->InvitePlayerToWar(hero);
                    wg->PlayerAcceptInviteToWar(hero);
                }
            }
        }
    }

    // Drive every tracked attacker vehicle along its convoy route (workshop -> road/
    // bridge -> gate approach), then batter the assault corridor's obstacles in order:
    // the front gate, then the relic door — the two breaches that actually progress a
    // WG battle. Cleans up vehicles that died / despawned (returns the live count).
    static uint32 DriveVehicles(Battlefield* wg)
    {
        // Snapshot + re-find-by-key-under-lock relies on OnUpdate being the only writer
        // (single world thread, 2s cadence) — don't call this from anywhere else.
        std::vector<std::pair<ObjectGuid, WgVehicle>> snap;
        { std::lock_guard<std::mutex> lk(g_wgMutex); snap.assign(g_wgVehicles.begin(), g_wgVehicles.end()); }
        if (snap.empty()) return 0;
        // Anchor for creature lookups: any in-map war participant (creatures resolve via its map).
        WorldObject* anchor = nullptr;
        for (uint8 ti = 0; ti < 2 && !anchor; ++ti)
            for (ObjectGuid const& g : wg->GetPlayersInWarSet(TeamId(ti)))
                if (Player* p = ObjectAccessor::FindConnectedPlayer(g)) { anchor = p; break; }
        if (!anchor) return uint32(snap.size());   // can't resolve right now; assume still live
        BattlefieldWG* bfwg = static_cast<BattlefieldWG*>(wg);
        uint32 live = 0;
        for (auto const& [vg, st] : snap)
        {
            Creature* veh = ObjectAccessor::GetCreature(*anchor, vg);
            if (!veh || !veh->IsAlive())
            {
                if (veh)
                {
                    LOG_INFO("module",
                        "[WowPsParty WGFill] vehicle {} destroyed at ({:.0f},{:.0f}) leg {}/{}",
                        veh->GetEntry(), veh->GetPositionX(), veh->GetPositionY(), st.leg, st.legs.size());
                    veh->DespawnOrUnsummon();
                }
                else
                    LOG_INFO("module", "[WowPsParty WGFill] vehicle {} vanished (leg {}/{})",
                             vg.GetCounter(), st.leg, st.legs.size());
                { std::lock_guard<std::mutex> lk(g_wgMutex); g_wgVehicles.erase(vg); }
                continue;
            }
            ++live;

            // Convoy legs first. A leg that can't be reached in WG_LEG_TIMEOUT_MS is
            // skipped so one bad path never wedges the whole siege behind it.
            if (st.leg < st.legs.size())
            {
                WgPt const& t = st.legs[st.leg];
                bool const timedOut = getMSTime() - st.legSinceMs > WG_LEG_TIMEOUT_MS;
                if (veh->GetExactDist2d(t.x, t.y) < WG_LEG_REACH || timedOut)
                {
                    if (timedOut)
                        LOG_INFO("module",
                            "[WowPsParty WGFill] vehicle {} stuck on leg {} at ({:.0f},{:.0f}) — skipping",
                            veh->GetEntry(), st.leg, veh->GetPositionX(), veh->GetPositionY());
                    // Home follows the convoy: an evade / combat-drop reset must never
                    // send the vehicle crawling all the way back to its spawn workshop
                    // (SpawnCreature homes it there — the "stacked at the workshop" loop).
                    veh->SetHomePosition(veh->GetPositionX(), veh->GetPositionY(),
                                         veh->GetPositionZ(), veh->GetOrientation());
                    std::lock_guard<std::mutex> lk(g_wgMutex);
                    auto it = g_wgVehicles.find(vg);
                    if (it != g_wgVehicles.end()) { ++it->second.leg; it->second.legSinceMs = getMSTime(); }
                }
                else
                    SteerVehicle(vg, veh, t.x, t.y, WgSnapZ(veh->GetMap(), t.x, t.y, t.z));
                continue;
            }

            // At the front: batter the assault corridor in REAL siege order, advancing
            // stage by stage — the gate first, then THROUGH the gate and inner arch on
            // the open centre line, then shell the vault door from the keep floor.
            // ModifyHealth needs no LoS, so every swing is position-gated: the vault
            // door is only ever hit from INSIDE the keep, past the arch — never through
            // standing walls (the "relic door died while every inner wall stood" bug).
            float const yOff = float(int(vg.GetCounter() % 5) - 2) * 4.0f;
            GameObject* gate = WgBuildingGo(bfwg, WG_GO_FRONT_GATE);
            GameObject* door = WgBuildingGo(bfwg, WG_GO_LAST_DOOR);
            if (gate && !WgBuildingDestroyed(bfwg, WG_GO_FRONT_GATE))
            {
                if (veh->GetDistance(gate) > WG_VEH_WALL_RANGE)
                    SteerVehicle(vg, veh, WG_GATE.x - 16.0f, WG_GATE.y + yOff, WG_GATE.z);
                else
                    Batter(veh, gate);
            }
            else if (door && !WgBuildingDestroyed(bfwg, WG_GO_LAST_DOOR))
            {
                // Corridor bands by x: <5222 head for the courtyard; <5284 line up on
                // the arch; >=5330 and within 45y shell the door; the 5284..5330 gap
                // advances to the keep floor via the else — every x moves or swings.
                float const vx = veh->GetPositionX();
                if (vx < WG_COURTYARD.x - 8.0f)
                    SteerVehicle(vg, veh, WG_COURTYARD.x, WG_COURTYARD.y + yOff * 0.5f, WG_COURTYARD.z);
                else if (vx < WG_INNER_ARCH.x + 5.0f)
                    SteerVehicle(vg, veh, WG_INNER_ARCH.x + 10.0f, WG_INNER_ARCH.y + yOff * 0.25f, WG_INNER_ARCH.z);
                else if (vx >= WG_INNER_COURT.x - 10.0f && veh->GetDistance(door) <= 45.0f)
                    Batter(veh, door);   // shell the vault door from the keep floor, demolisher-style
                else
                    SteerVehicle(vg, veh, WG_INNER_COURT.x + 15.0f, WG_INNER_COURT.y + yOff * 0.5f, WG_INNER_COURT.z);
            }
            // Corridor fully open (relic stage): hold position as an occupying siege. No
            // flavour-battering of random walls — stray demolition is exactly what made
            // the battle state incomprehensible before.
        }
        return live;
    }

    // Re-steer unless the vehicle is already executing OUR point move TO THIS target:
    // an evade/home or charm-driven motion reset would otherwise leave it parked (or
    // crawling back to its spawn) until the next leg change, and a corridor target
    // that advanced mid-move (courtyard -> arch -> vault front) would lag a full
    // arrival behind without the changed-destination check.
    static void SteerVehicle(ObjectGuid vg, Creature* veh, float x, float y, float z)
    {
        bool const onPointMove = veh->isMoving()
            && veh->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
        {
            std::lock_guard<std::mutex> lk(g_wgMutex);
            auto it = g_wgVehicles.find(vg);
            if (it == g_wgVehicles.end())
            {
                if (onPointMove) return;
            }
            else
            {
                if (onPointMove && std::abs(it->second.steerX - x) < 4.0f
                                && std::abs(it->second.steerY - y) < 4.0f)
                    return;   // already driving to (roughly) this point
                it->second.steerX = x; it->second.steerY = y;
            }
        }
        veh->GetMotionMaster()->MovePoint(0, x, y, z);
    }

    static void Batter(Creature* veh, GameObject* target)
    {
        uint32 const maxHp = target->GetGOValue()->Building.MaxHealth;
        int32 const dmg = std::max<int32>(1, int32(maxHp / WG_VEH_WALL_DMG_DEN));
        target->ModifyHealth(-dmg, veh);   // siege damage; WG building hooks react -> battle progresses
    }

    // Keep up to WG_MAX_VEHICLES attacker siege vehicles in the field. Spawn near the
    // attacker staging and, if an idle attacking fill bot is free, seat it as the driver.
    static void TopUpVehicles(Battlefield* wg, uint32 live)
    {
        if (!wg->IsWarTime()) return;
        TeamId const atk = wg->GetAttackerTeam();
        // Fill toward ~50% of the attacker's ACTUAL siege-slot capacity (granted by the
        // workshops it controls), floor WG_MAX_VEHICLES so there's always a siege push.
        uint32 const maxSlots = wg->GetData(atk == TEAM_ALLIANCE
            ? BATTLEFIELD_WG_DATA_MAX_VEHICLE_A : BATTLEFIELD_WG_DATA_MAX_VEHICLE_H);
        uint32 const target = std::max<uint32>(WG_MAX_VEHICLES, (maxSlots + 1) / 2);
        if (live >= target) return;

        // Trickle replacements: a convoy the defenders are farming must not respawn a
        // vehicle every 2s tick onto the same pad — that's the "vehicles stack on top
        // of each other" pile-up. One spawn per cooldown window, tops.
        static uint32 lastSpawnMs = 0;
        if (lastSpawnMs && getMSTime() - lastSpawnMs < WG_VEH_SPAWN_CD_MS) return;

        // Spawn the siege AT a workshop the attacker CONTROLS — that's where WG vehicles
        // actually come from — then let DriveVehicles run the convoy up the road to the
        // gate. Round-robin across ALL held workshops so replacements spread out instead
        // of piling onto one pad. No attacker-held workshop -> no siege, the real WG rule.
        BattlefieldWG* bfwg = static_cast<BattlefieldWG*>(wg);
        std::vector<uint8> const held = WgAttackerWorkshops(bfwg, atk);
        if (held.empty()) return;

        WorldObject* anchor = nullptr;
        for (uint8 ti = 0; ti < 2 && !anchor; ++ti)
            for (ObjectGuid const& g : wg->GetPlayersInWarSet(TeamId(ti)))
                if (Player* p = ObjectAccessor::FindConnectedPlayer(g)) { anchor = p; break; }
        if (!anchor) return;   // nobody loaded to resolve the map yet — retry next tick

        static uint32 vehRoll = 0;
        uint8 const wsId = held[vehRoll % held.size()];
        WgPt const& ws = WG_WS[wsId];
        std::vector<WgPt> const legs = WgVehicleRoute(wsId);
        float const sx = ws.x + float(int(vehRoll % 5) - 2) * 6.0f;
        float const sy = ws.y + float(int(vehRoll / 5 % 5) - 2) * 6.0f;
        float const sz = WgSnapZ(anchor->GetMap(), sx, sy, ws.z);
        Position const spawnPos(sx, sy, sz, std::atan2(legs.front().y - sy, legs.front().x - sx));

        uint32 const entries[4] = { atk == TEAM_ALLIANCE ? WG_VEH_SIEGE_ALLY : WG_VEH_SIEGE_HORDE,
                                    WG_VEH_DEMOLISHER, WG_VEH_CATAPULT, WG_VEH_DEMOLISHER };
        uint32 const pick = entries[(vehRoll++) % 4];   // vary the entry across spawns
        Creature* veh = wg->SpawnCreature(pick, spawnPos, atk);
        if (!veh) return;
        lastSpawnMs = getMSTime();
        // Never fight back / evade off the convoy: the drive loop owns this creature.
        veh->SetReactState(REACT_PASSIVE);

        // Seat an idle attacking fill bot as the driver (flavour — the vehicle drives itself
        // server-side regardless). Pick one not already in a vehicle / fighting.
        ObjectGuid driver;
        {
            std::vector<uint32> cand;
            { std::lock_guard<std::mutex> lk(g_wgMutex);
              for (auto const& kv : g_wgBots) if (kv.second.team == uint32(atk) && kv.second.enrolled) cand.push_back(kv.first); }
            for (uint32 bl : cand)
            {
                Player* b = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(bl));
                if (!b || !b->IsAlive() || b->GetVehicle() || b->IsInCombat()) continue;
                b->EnterVehicle(veh, 0);
                driver = b->GetGUID();
                break;
            }
        }
        { std::lock_guard<std::mutex> lk(g_wgMutex);
          g_wgVehicles[veh->GetGUID()] = WgVehicle{ driver, getMSTime(), legs, 0, getMSTime() }; }
        LOG_INFO("module",
            "[WowPsParty WGFill] spawned attacker vehicle entry={} at workshop {} (live now {})",
            pick, uint32(wsId), live + 1);
    }

    // Spawn rndbot-pool chars (race-correct, offline, prefer already-high-level to skip
    // the heavy re-roll) up to the per-side target, a few per tick, within war vacancy.
    static void TopUp(Battlefield* wg, TeamId team, uint32 have)
    {
        if (have >= WG_TARGET_PER_SIDE || !wg->HasWarVacancy(team)) return;
        uint32 const need = std::min(WG_TARGET_PER_SIDE - have, WG_SPAWN_PER_TICK);
        std::string const acctCsv = RndbotAccountCsv();
        if (acctCsv.empty()) return;
        // Claim DISTINCT, FREE accounts only. One char per account can be online, and a
        // RNDBOT account holds both factions, so picking an account already busy with an
        // online char (the other team's fill, a henchman) or one an in-flight WG fill has
        // claimed makes the login silently fail and the side under-fills. Exclude accounts
        // any in-flight WG fill holds, then let SQL skip accounts with an online char and
        // collapse to one char per account.
        std::string acctExclude = "0";
        { std::lock_guard<std::mutex> lk(g_wgMutex);
          for (auto const& kv : g_wgBots) { acctExclude += ','; acctExclude += std::to_string(kv.second.account); } }
        std::string const sql =
            "SELECT MIN(`guid`), `account` FROM `characters` WHERE `account` IN (" + acctCsv + ") "
            "AND `account` NOT IN (" + acctExclude + ") "
            "AND `account` NOT IN (SELECT `account` FROM `characters` WHERE `online` = 1) "
            "AND `race` IN (" + RaceCsv(team == TEAM_ALLIANCE) + ") AND `online` = 0 "
            "GROUP BY `account` ORDER BY RAND() LIMIT " + std::to_string(need);
        QueryResult q = CharacterDatabase.Query(sql);
        if (!q) return;
        uint32 spawned = 0;
        do {
            if (spawned >= need) break;
            Field* f = q->Fetch();
            uint32 const guidLow = f[0].Get<uint32>();
            uint32 const acct    = f[1].Get<uint32>();
            { std::lock_guard<std::mutex> lk(g_wgMutex); if (g_wgBots.count(guidLow)) continue; }
            sRandomPlayerbotMgr.AddPlayerBot(ObjectGuid::Create<HighGuid::Player>(guidLow), 0);
            { std::lock_guard<std::mutex> lk(g_wgMutex);
              g_wgBots[guidLow] = WgBot{ uint32(team), false, false, getMSTime(), acct }; }
            ++spawned;
        } while (q->NextRow());
        if (spawned)
            LOG_INFO("module", "[WowPsParty WGFill] {} side: spawned {} (have {} -> target {})",
                     team == TEAM_ALLIANCE ? "Alliance" : "Horde", spawned, have, WG_TARGET_PER_SIDE);
    }

    // War over: despawn every tracked attacker siege vehicle and clear the tracking map so
    // neither the creatures nor the dict leak across battles (DriveVehicles — the only other
    // place that erases them — doesn't run once IsWarTime() is false). The map is cleared
    // unconditionally (no tracking leak even if no anchor is loaded to resolve the creatures);
    // a still-loaded fill bot serves as the anchor to actually despawn the creatures, and
    // fills linger for several post-war ticks (DespawnSome is gradual) so one is available.
    static void DespawnVehicles()
    {
        std::vector<ObjectGuid> vguids;
        { std::lock_guard<std::mutex> lk(g_wgMutex); for (auto const& kv : g_wgVehicles) vguids.push_back(kv.first); }
        if (vguids.empty()) return;
        WorldObject* anchor = nullptr;
        std::vector<uint32> bl;
        { std::lock_guard<std::mutex> lk(g_wgMutex); for (auto const& kv : g_wgBots) bl.push_back(kv.first); }
        for (uint32 b : bl)
            if (Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(b)))
            { anchor = p; break; }
        if (!anchor)
        {
            // No loaded fill bot left to resolve the creatures. Keep the tracking and retry
            // while bots are still winding down; once none remain the battlefield's own
            // teardown reaps the creatures — drop the tracking so it can't leak across battles.
            if (bl.empty()) { std::lock_guard<std::mutex> lk(g_wgMutex); g_wgVehicles.clear(); }
            return;
        }
        for (ObjectGuid const& vg : vguids)
            if (Creature* v = ObjectAccessor::GetCreature(*anchor, vg))
                v->DespawnOrUnsummon();
        std::lock_guard<std::mutex> lk(g_wgMutex);
        g_wgVehicles.clear();
    }

    // Log out up to n fills (gradual, so a battle-end teardown of ~60 bots never spikes).
    static void DespawnSome(uint32 n)
    {
        std::vector<std::pair<uint32, uint32>> snap;   // guidLow, spawnMs
        {
            std::lock_guard<std::mutex> lk(g_wgMutex);
            if (g_wgBots.empty()) return;
            for (auto const& kv : g_wgBots)
            { snap.emplace_back(kv.first, kv.second.spawnMs); if (snap.size() >= n) break; }
        }
        uint32 cleared = 0;
        for (auto const& [botLow, spawnMs] : snap)
        {
            ObjectGuid const g = ObjectGuid::Create<HighGuid::Player>(botLow);
            // Idempotent: logs out an in-world bot, no-ops one not yet owned. Forget it ONLY
            // once it's actually gone — and gate on the load grace so a bot that finished
            // its async login AFTER the war ended still gets logged out before we drop it,
            // instead of being orphaned in the world (forget-then-leak).
            sRandomPlayerbotMgr.LogoutPlayerBot(g);
            if (!ObjectAccessor::FindConnectedPlayer(g) && getMSTime() - spawnMs > 60000)
            { Forget(botLow); ++cleared; }
        }
        if (cleared)
            LOG_INFO("module", "[WowPsParty WGFill] despawned {} fill bot(s)", cleared);
    }
};

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
        // Track even with no heroes grouped YET — they may group after entry; the world tick
        // brings them in once they're in the party. Tracks any human (solo mode included) to
        // match the queue-time hook — deliberately NOT gated on spawnCompanions, so a
        // companions-OFF solo player still gets the entry-time top-up safety net.
        std::lock_guard<std::mutex> lk(g_mutex);
        g_activeLeaders[player->GetGUID().GetCounter()] = uint32(bg->GetBgTypeID());
    }
};

void AddPartyBgFillScripts()
{
    new PartyBgFillPlayerScript();
    new PartyBgFillWorldScript();
    new PartyBgFillBgScript();
    new PartyWgFillWorldScript();   // populate + drive Wintergrasp while the battle is active
}
