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
 *  - Random BG (BATTLEGROUND_RB) and arenas are skipped: the real BG isn't known
 *    until pop, so it can't be pre-filled.
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

#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"  // sPlayerbotAIConfig.randomBotJoinBG (the "bg" strategy gate)
#include "PlayerbotFactory.h"   // re-level a pool fill bot to the BG bracket on spawn
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h" // sRandomPlayerbotMgr — server-side spawn for WG (no human anchor)
#include "Battlefield.h"        // Wintergrasp: IsWarTime / war enrolment / participant sets
#include "BattlefieldMgr.h"     // sBattlefieldMgr->GetBattlefieldToZoneId(WG)
#include "MotionMaster.h"       // WG bot AI movement (MovePoint / MoveChase)
#include "GameObject.h"         // WG siege: destructible keep-wall damage (ModifyHealth)
#include "Cell.h"               // grid search for the keep walls
#include "CellImpl.h"
#include "GridNotifiers.h"      // GameObjectListSearcher
#include "GridNotifiersImpl.h"

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
    // (master 0) and register them as fill bots for this leader's BG. Draws IN-BRACKET
    // chars first (no re-level needed); any shortfall is backfilled from out-of-bracket
    // chars (closest level first) flagged for a re-level to the bracket on the world
    // tick — so a small flat pool serves every bracket. Returns how many were spawned
    // (pool may be short — logged, never silently capped).
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
        std::vector<uint32> usedAccts;
        auto accExclude = [&]() {
            std::string s = exclAccts;
            for (uint32 a : usedAccts) { s += ','; s += std::to_string(a); }
            return s;
        };
        auto take = [&](QueryResult& r, bool needsRelevel) {
            do {
                Field* f = r->Fetch();
                uint32 const guid = f[0].Get<uint32>();
                uint32 const acct = f[1].Get<uint32>();
                usedAccts.push_back(acct);
                picks.emplace_back(guid, acct, needsRelevel);
            } while (r->NextRow() && picks.size() < count);
        };

        // 1) In-bracket parked chars — usable as-is, no re-level. One per distinct account.
        //    The inner "account NOT IN (online chars)" also skips accounts busy with an
        //    online char from ANOTHER path (a hired henchman, a prior fill) — those are
        //    rndbot accounts too, and one online char per account is the hard limit.
        if (QueryResult q = CharacterDatabase.Query(
                "SELECT MIN(`guid`), `account` FROM `characters` WHERE `account` IN ({}) "
                "AND `account` NOT IN ({}) AND `account` NOT IN (SELECT `account` FROM `characters` WHERE `online` = 1) "
                "AND `online` = 0 AND `race` IN ({}) "
                "AND `level` BETWEEN {} AND {} GROUP BY `account` ORDER BY RAND() LIMIT {}",
                acctCsv, accExclude(), races, uint32(bmin), uint32(bmax), count))
            take(q, false);

        // 2) Out-of-bracket backfill, re-leveled on spawn — from accounts not already taken.
        //    (No "closest level" ordering: RelevelFillBot does a FULL re-roll to the bracket,
        //    so the starting level is irrelevant; distinct accounts are what matter.)
        if (picks.size() < count)
        {
            uint32 const remaining = count - uint32(picks.size());
            if (QueryResult q2 = CharacterDatabase.Query(
                    "SELECT MIN(`guid`), `account` FROM `characters` WHERE `account` IN ({}) "
                    "AND `account` NOT IN ({}) AND `account` NOT IN (SELECT `account` FROM `characters` WHERE `online` = 1) "
                    "AND `online` = 0 AND `race` IN ({}) "
                    "AND (`level` < {} OR `level` > {}) GROUP BY `account` ORDER BY RAND() LIMIT {}",
                    acctCsv, accExclude(), races, uint32(bmin), uint32(bmax), remaining))
                take(q2, true);
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
        uint32 const ownSide   = uint32(party.size());                       // human + heroes
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
    // a BG that is STILL in its pre-gate countdown (STATUS_WAIT_JOIN — the ~90s before the
    // doors open, where joins are guaranteed accepted, exactly the real-server "join an
    // in-progress BG with room" behaviour Mill described), keep BOTH teams topped up to
    // MaxPlayersPerTeam by spawning fills for the REAL deficit read off the LIVE instance.
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
        if (bg->GetStatus() != STATUS_WAIT_JOIN) return;   // only before the gates open
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
                LOG_INFO("module", "[WowPsParty BGFill] arena-port {} -> inst={} realBg={} (queued qt={})",
                         bot->GetName(), ginfo.IsInvitedToBGInstanceGUID, uint32(bgTypeId), uint32(qt));
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
            // Use the playerbots-native logout, NOT a raw WorldSession::LogoutPlayer.
            // LogoutPlayerBot queues the group/teardown cleanup onto the WORLD thread
            // and guards double-logout, so it doesn't tear the bot's items down from
            // here (the world-tick) while a map-worker thread races Item::GetOwner ->
            // FindPlayer — the recurring crash when ~15 fill bots retired at once on a
            // BG join. The fill bot was spawned on the leader's holder
            // (mgr->AddPlayerBot(guid,0)), so route through it.
            Player* const leader = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(leaderLow));
            PlayerbotMgr* const mgr = leader ? sPlayerbotsMgr.GetPlayerbotMgr(leader) : nullptr;
            if (mgr)
                mgr->LogoutPlayerBot(bot->GetGUID());
            else
                bot->GetSession()->LogoutPlayer(true);   // leader gone — raw fallback
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
        for (GroupReference* itr = grp->GetFirstMember(); itr; itr = itr->next())
            if (Player* mm = itr->GetSource())
                mm->AddBattlegroundQueueId(qt);
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

        // Rated-arena enemy fill runs on the same 1s cadence (its own state map, so it
        // must run BEFORE the BG-fill early-return below).
        DriveArenaFills();

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
                            ai->ChangeStrategy("+bg", BOT_STATE_NON_COMBAT);
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
                bool hasFills = false;
                for (auto const& kv : g_fillBots) if (kv.second.leaderLow == leaderLow) { hasFills = true; break; }
                if (!hasFills) g_activeLeaders.erase(leaderLow);
                continue;
            }
            // Keep the match topped up to max while it's still in its pre-gate countdown
            // (no-op unless the leader is in a BG in STATUS_WAIT_JOIN with a real deficit).
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
// server-side, enrolled into the war) and DRIVES a simple advance-and-engage AI so it's a
// lively ~30v30 instead of a dead zone. Everything is gated behind IsWarTime(): nothing
// runs outside an active battle, and all fills are torn down when it ends.
//   NEEDS LIVE TESTING in a real WG window (battlefield/vehicle paths can't be unit-
//   tested). Siege-vehicle crewing is a deliberate follow-up — bots fight on foot here.
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
// The attackers maintain up to WG_MAX_VEHICLES siege creatures that drive to the keep
// walls and batter them down (Mill). Each is spawned attacker-faction (so defenders treat
// it as a target) and, when one's available, an idle attacking fill bot is seated as the
// driver for flavour; the vehicle's motion + wall damage are driven server-side regardless.
constexpr uint32 WG_MAX_VEHICLES      = 5;
constexpr float  WG_VEH_WALL_RANGE    = 16.0f;    // within this of a wall -> batter it
constexpr uint32 WG_VEH_WALL_DMG_DEN  = 25;       // damage/tick = wall maxHealth / this (~50s/wall solo)
struct WgVehicle { ObjectGuid driver; uint32 spawnMs; };
std::unordered_map<ObjectGuid, WgVehicle> g_wgVehicles;   // vehicle GUID -> state

// Random attacker siege creatures. Siege Engine is faction-specific; demolisher/catapult
// are shared. (Entries from BattlefieldWG.h.)
uint32 const WG_VEH_SIEGE_ALLY  = 28312;
uint32 const WG_VEH_SIEGE_HORDE = 32627;
uint32 const WG_VEH_DEMOLISHER  = 28094;
uint32 const WG_VEH_CATAPULT    = 27881;

// Finds intact destructible buildings (the keep walls/towers) near a point.
struct WgWallCheck
{
    float x, y, z, range;
    WgWallCheck(float _x, float _y, float _z, float _r) : x(_x), y(_y), z(_z), range(_r) {}
    bool operator()(GameObject* go) const
    {
        if (!go || go->GetGoType() != GAMEOBJECT_TYPE_DESTRUCTIBLE_BUILDING) return false;
        if (go->GetDestructibleState() == GO_DESTRUCTIBLE_DESTROYED) return false;
        return go->IsWithinDist3d(x, y, z, range);
    }
};

// Contest points to fan the fills across (real WG locations so pathing stays on the
// navmesh): the keep is the central objective both sides converge on, the NE workshop a
// secondary so they don't all stack on one spot.
struct WgPt { float x, y, z; };
WgPt const WG_KEEP     { 5345.0f, 2842.0f, 410.0f };
WgPt const WG_WORKSHOP { 5104.0f, 2300.0f, 368.0f };

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
        if (team == wg->GetDefenderTeam()) bot->TeleportTo(WG_MAP_ID, 5345.0f, 2842.0f, 410.0f, 3.14f);
        else if (team == TEAM_HORDE)       bot->TeleportTo(WG_MAP_ID, 5025.857f, 3674.629f, 362.737f, 4.135f);
        else                               bot->TeleportTo(WG_MAP_ID, 5101.284f, 2186.564f, 365.549f, 3.812f);
    }

    // Advance-and-engage: in combat -> the class combat AI fights; otherwise grab the
    // nearest enemy war participant, or push toward an objective so the sides converge.
    static void DriveAI(Player* bot, Battlefield* wg)
    {
        if (!bot->IsAlive()) return;                          // dead -> battlefield res handles it
        if (bot->IsInCombat() && bot->GetVictim()) return;    // fighting -> combat AI owns it
        if (bot->IsNonMeleeSpellCast(false, false, true)) return;

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
                else if (!bot->isMoving())
                    bot->GetMotionMaster()->MovePoint(0, veh->GetPositionX(), veh->GetPositionY(), veh->GetPositionZ());
                return;
            }
        }

        uint32 const guidLow = bot->GetGUID().GetCounter();
        WgPt const& obj = (guidLow % 3 == 0) ? WG_WORKSHOP : WG_KEEP;
        float const tx = obj.x + float(int(guidLow % 7) - 3) * 10.0f;   // fan out so they don't stack
        float const ty = obj.y + float(int(guidLow / 7 % 7) - 3) * 10.0f;
        if (!bot->isMoving() && bot->GetExactDist2d(tx, ty) > 8.0f)
            bot->GetMotionMaster()->MovePoint(0, tx, ty, obj.z);
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
                    if (IsInWar(wg, hero)) continue;                                // already enrolled
                    if (hero->GetZoneId() != WG_ZONE_ID) { TeleportToStaging(hero, wg); continue; }
                    wg->InvitePlayerToWar(hero);
                    wg->PlayerAcceptInviteToWar(hero);
                }
            }
        }
    }

    // Drive every tracked attacker vehicle into the nearest intact keep wall and batter it.
    // Cleans up vehicles that died / despawned / no longer have a war (returns the live count).
    static uint32 DriveVehicles(Battlefield* wg)
    {
        std::vector<ObjectGuid> vguids;
        { std::lock_guard<std::mutex> lk(g_wgMutex); for (auto const& kv : g_wgVehicles) vguids.push_back(kv.first); }
        if (vguids.empty()) return 0;
        // Anchor for creature lookups: any in-map war participant (creatures resolve via its map).
        WorldObject* anchor = nullptr;
        for (uint8 ti = 0; ti < 2 && !anchor; ++ti)
            for (ObjectGuid const& g : wg->GetPlayersInWarSet(TeamId(ti)))
                if (Player* p = ObjectAccessor::FindConnectedPlayer(g)) { anchor = p; break; }
        if (!anchor) return uint32(vguids.size());   // can't resolve right now; assume still live
        uint32 live = 0;
        for (ObjectGuid const& vg : vguids)
        {
            Creature* veh = ObjectAccessor::GetCreature(*anchor, vg);
            if (!veh || !veh->IsAlive())
            {
                if (veh) veh->DespawnOrUnsummon();
                { std::lock_guard<std::mutex> lk(g_wgMutex); g_wgVehicles.erase(vg); }
                continue;
            }
            ++live;
            // Nearest intact wall to the vehicle (scan around it; the keep walls are clustered).
            GameObject* wall = nullptr; float bestD = 1e9f;
            std::list<GameObject*> walls;
            WgWallCheck check(veh->GetPositionX(), veh->GetPositionY(), veh->GetPositionZ(), 300.0f);
            Acore::GameObjectListSearcher<WgWallCheck> searcher(veh, walls, check);
            Cell::VisitObjects(veh, searcher, 300.0f);
            for (GameObject* w : walls)
            {
                float const d = veh->GetDistance(w);
                if (d < bestD) { bestD = d; wall = w; }
            }
            if (!wall) continue;   // walls all down (battle nearly won) — nothing to batter
            if (bestD > WG_VEH_WALL_RANGE)
            {
                if (!veh->isMoving())
                    veh->GetMotionMaster()->MovePoint(0, wall->GetPositionX(), wall->GetPositionY(), wall->GetPositionZ());
            }
            else
            {
                uint32 const maxHp = wall->GetGOValue()->Building.MaxHealth;
                int32 const dmg = std::max<int32>(1, int32(maxHp / WG_VEH_WALL_DMG_DEN));
                wall->ModifyHealth(-dmg, veh);   // siege damage; WG building hooks react -> battle progresses
            }
        }
        return live;
    }

    // Keep up to WG_MAX_VEHICLES attacker siege vehicles in the field. Spawn near the
    // attacker staging and, if an idle attacking fill bot is free, seat it as the driver.
    static void TopUpVehicles(Battlefield* wg, uint32 live)
    {
        if (live >= WG_MAX_VEHICLES || !wg->IsWarTime()) return;

        TeamId const atk = wg->GetAttackerTeam();
        Position const stage = (atk == TEAM_HORDE) ? Position(5025.857f, 3674.629f, 362.737f, 4.135f)
                                                   : Position(5101.284f, 2186.564f, 365.549f, 3.812f);
        uint32 const entries[4] = { atk == TEAM_ALLIANCE ? WG_VEH_SIEGE_ALLY : WG_VEH_SIEGE_HORDE,
                                    WG_VEH_DEMOLISHER, WG_VEH_CATAPULT, WG_VEH_DEMOLISHER };
        static uint32 vehRoll = 0;
        uint32 const pick = entries[(vehRoll++) % 4];   // vary the entry across spawns
        Creature* veh = wg->SpawnCreature(pick, stage, atk);
        if (!veh) return;

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
        { std::lock_guard<std::mutex> lk(g_wgMutex); g_wgVehicles[veh->GetGUID()] = WgVehicle{ driver, getMSTime() }; }
        LOG_INFO("module", "[WowPsParty WGFill] spawned attacker vehicle entry={} (live now {})", pick, live + 1);
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
        {
            std::lock_guard<std::mutex> lk(g_wgMutex);
            for (auto const& kv : g_wgVehicles) vguids.push_back(kv.first);
            g_wgVehicles.clear();
        }
        if (vguids.empty()) return;
        WorldObject* anchor = nullptr;
        {
            std::vector<uint32> bl;
            { std::lock_guard<std::mutex> lk(g_wgMutex); for (auto const& kv : g_wgBots) bl.push_back(kv.first); }
            for (uint32 b : bl)
                if (Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(b)))
                { anchor = p; break; }
        }
        if (!anchor) return;
        for (ObjectGuid const& vg : vguids)
            if (Creature* v = ObjectAccessor::GetCreature(*anchor, vg))
                v->DespawnOrUnsummon();
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
    new PartyWgFillWorldScript();   // populate + drive Wintergrasp while the battle is active
}
