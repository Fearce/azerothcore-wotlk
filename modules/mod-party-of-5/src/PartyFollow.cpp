/*
 * WowPs Party-of-5 — Dedicated follow system implementation
 *
 * See PartyFollow.h for rationale.
 *
 * Implementation notes:
 *   - Registry is a flat vector<Directive>. We expect ~5 entries per
 *     account, total max maybe 50 in a real session. Linear scan is fine.
 *   - Tick accumulator pattern: we don't need an exact 1Hz; "approximately
 *     once per second" is the contract. WorldHook::OnUpdate fires every
 *     world tick (~50ms-ish in AC); we accumulate diff and fire when over
 *     threshold.
 *   - MoveFollow is idempotent when the bot is already following the same
 *     target with same dist/angle. AC's MoveSplineInit caches the spline
 *     state and won't generate redundant packets unless the destination
 *     actually changed. So re-asserting every second is cheap.
 *   - Skip conditions per follower:
 *       * not in world  -> skip (transient logout/teleport)
 *       * different map  -> skip (cross-map MoveFollow is broken)
 *       * in combat      -> skip (combat AI runs MoveChase; don't fight it)
 *       * has charm      -> skip (mid-possess controller; charm is fragile)
 *       * is charmed     -> skip (controlled-by-someone; can't drive)
 *       * leader missing -> skip the entry; don't auto-clear (leader may
 *                          just be momentarily offline mid-loading-screen)
 */
#include "PartyFollow.h"
#include "PartyMgr.h"
#include "PartyPath.h"
#include "PartyRotation.h"   // BotIsKiting

#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LFGMgr.h"
#include "Log.h"
#include "MotionMaster.h"
#include "StringFormat.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"   // SpellInfo::Effects[] for the leader's mount-type check

// Gathering (mining / herbalism) for follower bots.
#include "Cell.h"
#include "CellImpl.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "LootMgr.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "WorldPacket.h"
#include "Opcodes.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <vector>

namespace WowPsParty
{
    namespace
    {
        // Predicate for the grid searcher: any spawned GameObject within range.
        // The mining/herb skill-lock filtering happens in the caller; this just
        // collects nearby spawned GOs.
        struct NearbySpawnedGOCheck
        {
            NearbySpawnedGOCheck(WorldObject const* src, float range)
                : _src(src), _range(range) {}
            bool operator()(GameObject* go) const
            { return go && go->isSpawned() && _src->IsWithinDist(go, _range, false); }
            WorldObject const* _src;
            float _range;
        };

        struct Directive
        {
            uint32      account;       // for SetActiveFollowers clearing
            ObjectGuid  followerGuid;
            ObjectGuid  leaderGuid;
            uint8       slot   = 0;    // 0..4 within the owning account's party
            std::string role   = "dps"; // tank / healer / dps
            bool        henchman = false; // hired bot (default combat AI), not an enrolled alt
        };

        // Which slot in the directive list belongs to the "leading tank" for
        // dungeon formations. Computed in SetActiveFollowers; -1 means none
        // (no tank enrolled, or active player IS the tank).
        struct AccountFormation
        {
            int tankSlot = -1;  // slot whose member is the dungeon-front tank
        };
        static std::unordered_map<uint32, AccountFormation> g_formations;

        // followerGuidLow -> absolute getMSTime() at which the hold expires.
        // Used by AssistTarget / PartyFollow to skip motion changes while a
        // bot is intentionally stationary (drinking, holding for healer, etc).
        static std::unordered_map<uint32, uint32> g_holdUntilMs;

        static std::mutex                g_mutex;
        static std::vector<Directive>    g_directives;
        static std::atomic<bool>         g_tickerInstalled{false};
        static constexpr uint32          TICK_INTERVAL_MS = 1000;

        // Per-account quick erase by account id.
        void EraseByAccount_NoLock(uint32 account)
        {
            g_directives.erase(
                std::remove_if(g_directives.begin(), g_directives.end(),
                    [account](Directive const& d) { return d.account == account; }),
                g_directives.end());
        }
    } // namespace

    void SetActiveFollowers(uint32 account, ObjectGuid leaderGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Erase only enrolled-alt directives — KEEP hired henchmen. They live
        // outside account_party, so a full wipe would drop them from our
        // management (UpdateAI stops treating them as party bots → they revert
        // to default-AI "glued to master" without follow/leash). Kicking or
        // re-enrolling an alt re-runs this, so henchmen must survive it.
        g_directives.erase(
            std::remove_if(g_directives.begin(), g_directives.end(),
                [account](Directive const& d)
                { return d.account == account && !d.henchman; }),
            g_directives.end());
        // Re-point surviving henchmen at the current leader.
        for (auto& d : g_directives)
            if (d.account == account && d.henchman) d.leaderGuid = leaderGuid;
        g_formations.erase(account);

        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid`, `slot`, COALESCE(`role`, 'dps') FROM `account_party` "
            "WHERE `account` = {}", account);
        if (!q) return;
        uint32 added = 0;
        int firstTankSlot = -1;
        do
        {
            Field* f = q->Fetch();
            uint32 const memberGuidLow = f[0].Get<uint32>();
            uint8 const  memberSlot    = f[1].Get<uint8>();
            std::string  memberRole    = f[2].Get<std::string>();
            ObjectGuid const memberGuid = ObjectGuid::Create<HighGuid::Player>(memberGuidLow);
            if (memberGuid == leaderGuid) continue;

            Directive d;
            d.account = account;
            d.followerGuid = memberGuid;
            d.leaderGuid = leaderGuid;
            d.slot = memberSlot;
            d.role = std::move(memberRole);
            if (d.role == "tank" && firstTankSlot < 0)
                firstTankSlot = int(memberSlot);
            g_directives.push_back(std::move(d));
            ++added;

            // Disable mod-playerbots' own follow strategy on this bot so it
            // doesn't fight our ticker. Their FollowAction reads stale
            // GroupLeaderValue cache and was pulling bots back to the
            // original session player every AI tick -- visible as bots
            // oscillating between old leader and new leader. Combat strategies
            // (MoveChase etc.) are unaffected.
            Player* p = ObjectAccessor::FindConnectedPlayer(memberGuid);
            if (p)
            {
                if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(p))
                {
                    ai->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
                }
            }
        } while (q->NextRow());

        g_formations[account].tankSlot = firstTankSlot;

        LOG_INFO("module",
            "[WowPsParty Follow] SetActiveFollowers account={} leader_guid={} "
            "followers_installed={} dungeon_tank_slot={}",
            account, leaderGuid.GetCounter(), added, firstTankSlot);
    }

    // Append a single follow directive for a hired henchman, WITHOUT clearing
    // the account's other directives (SetActiveFollowers rebuilds the whole
    // set, which would wipe henchmen). The henchman gets our follow ticker,
    // leash and tank-lead, but the UpdateAI gate lets default mod-playerbots
    // AI run its combat rotation (see WowPsParty_IsHenchman_Trampoline).
    void AddHenchmanDirective(uint32 account, ObjectGuid henchGuid,
                              ObjectGuid leaderGuid, std::string const& role)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto const& d : g_directives)
            if (d.followerGuid == henchGuid) return;   // already tracked
        Directive d;
        d.account      = account;
        d.followerGuid = henchGuid;
        d.leaderGuid   = leaderGuid;
        d.slot         = 255;          // not an account_party slot
        d.role         = role.empty() ? "dps" : role;
        d.henchman     = true;
        g_directives.push_back(std::move(d));
        LOG_INFO("module",
            "[WowPsParty Follow] AddHenchmanDirective account={} hench_guid={} "
            "leader_guid={} role={}", account, henchGuid.GetCounter(),
            leaderGuid.GetCounter(), role);
    }

    // Update a tracked follower's role in place (e.g. a henchman whose spec was
    // re-rolled by the level-match factory after spawn — its directive role
    // drives targeting mode and lead-tank selection, so it must follow the new
    // spec, not the pre-spawn guess).
    void SetHenchmanRole(ObjectGuid followerGuid, std::string const& role)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& d : g_directives)
            if (d.followerGuid == followerGuid)
            {
                d.role = role.empty() ? "dps" : role;
                return;
            }
    }

    // Drop a single follower's directive (henchman dismiss / logout).
    void RemoveFollower(ObjectGuid followerGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_directives.erase(
            std::remove_if(g_directives.begin(), g_directives.end(),
                [&](Directive const& d){ return d.followerGuid == followerGuid; }),
            g_directives.end());
    }

    bool IsHenchman(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto const& d : g_directives)
            if (d.followerGuid == guid) return d.henchman;
        return false;
    }

    // Number of hired henchmen currently following the given leader.
    uint32 CountHenchmenFor(ObjectGuid leaderGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        uint32 n = 0;
        for (auto const& d : g_directives)
            if (d.henchman && d.leaderGuid == leaderGuid) ++n;
        return n;
    }

    // Total companions (enrolled alts + henchmen) following the given leader.
    // Used for the party-space cap before the WoW group exists.
    uint32 CountFollowersFor(ObjectGuid leaderGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        uint32 n = 0;
        for (auto const& d : g_directives)
            if (d.leaderGuid == leaderGuid) ++n;
        return n;
    }

    std::string RoleForGuid(ObjectGuid botGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto const& d : g_directives)
            if (d.followerGuid == botGuid) return d.role;
        return std::string();
    }

    // True if the bot's party has a LIVE tank-role member (enrolled alt or
    // hired henchman) on the same map. Lets a ranged bot decide between kiting
    // (a tank will peel the mob) and planting to fight (nobody will, so running
    // just drags the mob around forever). The human leader's own spec is
    // unknown to us, so a player who tanks personally reads as "no tank" — fine,
    // since the mob returns to them on threat and the bot only stands its ground
    // while the mob is actually on it.
    static bool PartyHasLiveTank(Player* bot)
    {
        if (!bot) return false;
        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(bot->GetGUID(), party);
        for (ObjectGuid const& gg : party)
        {
            if (RoleForGuid(gg) != "tank") continue;
            Player* t = ObjectAccessor::FindConnectedPlayer(gg);
            if (t && t->IsInWorld() && t->IsAlive()
                && t->GetMapId() == bot->GetMapId())
                return true;
        }
        return false;
    }

    // Pick a LEVEL- and RACE-appropriate mount for the bot, matching the
    // leader's GROUND-vs-FLYING type — instead of copying the leader's exact
    // mount (a level-20 alt riding the player's epic flyer looked absurd).
    // Racial mount IDs ported from mod-playerbots' factory table; tiers by the
    // standard riding-skill levels (20 slow ground, 40 fast ground, 60 slow
    // fly, 70 epic fly). Prefer a mount the bot actually owns (its own skin),
    // else fall back to the first racial id and cast it triggered so a mountless
    // henchman still gets the right model. Returns 0 if the bot can't ride yet.
    static uint32 ChooseBotMountSpell(Player* bot, bool leaderFlying)
    {
        uint8 const level = bot->GetLevel();
        if (level < 20) return 0;   // no riding skill yet

        std::vector<uint32> slow, fast;
        switch (bot->getRace())
        {
            case RACE_HUMAN:         slow={470,6648,458,472};       fast={23228,23227,23229}; break;
            case RACE_ORC:           slow={6654,6653,580};          fast={23250,23252,23251}; break;
            case RACE_DWARF:         slow={6899,6777,6898};         fast={23238,23239,23240}; break;
            case RACE_NIGHTELF:      slow={10789,8394,10793};       fast={23219,23220,63637}; break;
            case RACE_UNDEAD_PLAYER: slow={17463,17464,17462};      fast={17465,23246,66846}; break;
            case RACE_TAUREN:        slow={18990,18989,64657};      fast={23249,23248,23247}; break;
            case RACE_GNOME:         slow={10969,17453,10873,17454};fast={23225,23223,23222}; break;
            case RACE_TROLL:         slow={10796,10799,8395};       fast={23241,23242,23243}; break;
            case RACE_DRAENEI:       slow={34406,35711,35710};      fast={35713,35712,35714}; break;
            case RACE_BLOODELF:      slow={33660,35020,35022,35018};fast={35025,35026,35027}; break;
            default:
                if (bot->GetTeamId() == TEAM_HORDE) { slow={6654,6653,580};    fast={23250,23252,23251}; }
                else                                { slow={470,6648,458,472}; fast={23228,23227,23229}; }
        }
        std::vector<uint32> fslow, ffast;
        if (bot->GetTeamId() == TEAM_ALLIANCE) { fslow={32235,32239,32240}; ffast={32242,32289,32290,32292}; }
        else                                   { fslow={32244,32245,32243}; ffast={32295,32297,32246,32296}; }

        // Fly only when the leader is flying AND the bot can actually fly here
        // (level 60+, and Cold Weather Flying for Northrend). Otherwise ground.
        bool canFly = leaderFlying && level >= 60;
        if (canFly && bot->GetMapId() == 571 /*Northrend*/ && !bot->HasSpell(54197 /*Cold Weather Flying*/))
            canFly = false;

        std::vector<uint32> const& tier =
            canFly ? (level >= 70 ? ffast : fslow)
                   : (level >= 40 ? fast  : slow);
        std::vector<uint32> const& use = tier.empty() ? slow : tier;
        if (use.empty()) return 0;

        for (uint32 id : use)
            if (bot->HasSpell(id)) return id;   // its own learned skin
        return use.front();                     // else cast a racial one triggered
    }

    // True if `botGuid` is the account's designated dungeon lead tank, decided
    // by DIRECTIVE ROLE (the lowest-guid follower with role "tank"), so a hired
    // HENCHMAN tank can lead the route too — the old slot-based check excluded
    // henchmen (they have no account_party slot).
    bool IsLeadTank(ObjectGuid botGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        uint32 account = 0;
        bool found = false;
        for (auto const& d : g_directives)
            if (d.followerGuid == botGuid) { account = d.account; found = true; break; }
        if (!found) return false;
        ObjectGuid lead;
        for (auto const& d : g_directives)
            if (d.account == account && d.role == "tank")
                if (lead.IsEmpty() ||
                    d.followerGuid.GetCounter() < lead.GetCounter())
                    lead = d.followerGuid;
        return !lead.IsEmpty() && lead == botGuid;
    }

    // Stable 0-based ordinal of `follower` among all of `leaderGuid`'s
    // companions (sorted by guid). Drives formation spread — distinct per
    // companion regardless of account_party slot, so HENCHMEN (which have no
    // slot) fan out too instead of all stacking on the default angle.
    int FormationIndexFor(ObjectGuid follower, ObjectGuid leaderGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<uint32> guids;
        for (auto const& d : g_directives)
            if (d.leaderGuid == leaderGuid) guids.push_back(d.followerGuid.GetCounter());
        std::sort(guids.begin(), guids.end());
        for (size_t i = 0; i < guids.size(); ++i)
            if (guids[i] == follower.GetCounter()) return int(i);
        return 0;
    }

    bool BotHasActiveFollowDirective(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto const& d : g_directives)
            if (d.followerGuid == guid) return true;
        return false;
    }

    void HoldFollower(ObjectGuid followerGuid, uint32 durationMs)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_holdUntilMs[followerGuid.GetCounter()] = getMSTime() + durationMs;
    }

    bool IsFollowerHeld(ObjectGuid followerGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_holdUntilMs.find(followerGuid.GetCounter());
        if (it == g_holdUntilMs.end()) return false;
        if (getMSTime() >= it->second)
        {
            g_holdUntilMs.erase(it);
            return false;
        }
        return true;
    }

    // followerGuidLow -> ms until which the bot is actively leading the
    // dungeon path. Distinct from HoldFollower (which TankFollowPath also
    // honours, so it can't hold itself): this signal is read ONLY by the
    // follow ticker so it leaves the leading tank entirely to TankFollowPath.
    // Without it, the ticker's "constant distance for 3 ticks = stuck"
    // catch-up teleport fires when the leader stops and the tank idles at its
    // lookahead — snapping the tank onto the leader.
    static std::unordered_map<uint32, uint32> g_tankLeadUntilMs;

    void MarkTankLeading(ObjectGuid tankGuid, uint32 durationMs)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_tankLeadUntilMs[tankGuid.GetCounter()] = getMSTime() + durationMs;
    }

    static bool IsTankLeading(ObjectGuid tankGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tankLeadUntilMs.find(tankGuid.GetCounter());
        if (it == g_tankLeadUntilMs.end()) return false;
        if (getMSTime() >= it->second)
        {
            g_tankLeadUntilMs.erase(it);
            return false;
        }
        return true;
    }

    // followerGuidLow -> ms until which a lead tank is mid ranged-pull. While set,
    // AssistTarget holds the tank at throwing range and lets the pulled mob close
    // instead of chasing into the pack ("don't face-pull a whole room"). Short
    // window: once the mob reaches melee the hold ends naturally; if nothing comes
    // (no ranged pull landed) it expires and the tank falls back to closing in.
    static std::unordered_map<uint32, uint32> g_tankPullUntilMs;

    static void MarkTankPulling(ObjectGuid tankGuid, uint32 durationMs)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_tankPullUntilMs[tankGuid.GetCounter()] = getMSTime() + durationMs;
    }

    bool IsTankPulling(ObjectGuid tankGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tankPullUntilMs.find(tankGuid.GetCounter());
        if (it == g_tankPullUntilMs.end()) return false;
        if (getMSTime() >= it->second)
        {
            g_tankPullUntilMs.erase(it);
            return false;
        }
        return true;
    }

    // Henchmen currently being MOVED between groups during a (re-)hire. While a
    // guid is in here, the group-removal dismiss hook ignores it — otherwise
    // pulling a freshly-spawned henchman out of a STALE group (one left over in
    // the DB after leaving an LFG dungeon) would re-fire the dismiss hook and the
    // henchman would greet then instantly leave ("hello / see you later").
    static std::unordered_set<uint32> g_regrouping;

    void SetHenchmanRegrouping(ObjectGuid henchGuid, bool on)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (on) g_regrouping.insert(henchGuid.GetCounter());
        else    g_regrouping.erase(henchGuid.GetCounter());
    }

    bool IsHenchmanRegrouping(ObjectGuid henchGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_regrouping.count(henchGuid.GetCounter()) != 0;
    }

    ObjectGuid GetLeaderFor(ObjectGuid followerGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto const& d : g_directives)
            if (d.followerGuid == followerGuid) return d.leaderGuid;
        return ObjectGuid::Empty;
    }

    // Enumerate the whole party (leader + all follower bots) that `member`
    // belongs to, straight from our in-memory follow directives. This is the
    // authoritative party roster — independent of the WoW Group, which can
    // form incompletely from bot-spawn timing races and leave a healer bot
    // ungrouped (and therefore blind to the leader's health). `member` itself
    // is included. Returns nothing if the member has no directive.
    void GetPartyGuidsFor(ObjectGuid member, std::vector<ObjectGuid>& out)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        uint32 account = 0;
        bool found = false;
        // Match whether `member` is a FOLLOWER or the LEADER. The leader is
        // never stored as a followerGuid, so a leader-only match is essential —
        // without it, GetPartyGuidsFor(leader) returned empty, which silently
        // broke DismissAllHenchmen(leader) (left stale henchman directives that
        // inflated the party-full count) and party enumeration for the leader.
        for (auto const& d : g_directives)
            if (d.followerGuid == member || d.leaderGuid == member)
            { account = d.account; found = true; break; }
        if (!found) return;
        ObjectGuid leader;
        for (auto const& d : g_directives)
            if (d.account == account)
            {
                out.push_back(d.followerGuid);
                leader = d.leaderGuid;   // same for every row in the account
            }
        if (leader && std::find(out.begin(), out.end(), leader) == out.end())
            out.push_back(leader);
    }

    // Throttled log helper — at most one line per bot per 4 seconds, per
    // unique reason. Otherwise diagnostic logging here floods the file.
    static void AssistLog(uint32 guidLow, char const* reason)
    {
        static thread_local std::unordered_map<uint64, uint32> lastMs;
        uint64 key = (uint64(guidLow) << 32) ^ std::hash<std::string>{}(reason);
        uint32 nowMs = getMSTime();
        uint32& last = lastMs[key];
        if (nowMs - last < 4000) return;
        last = nowMs;
        LOG_INFO("module", "[WowPsParty Assist] guid={} {}", guidLow, reason);
    }

    // Throttled per (bot, reason) diagnostic for the gathering path. Only fires
    // for bots that actually have a gather profession (the skill gate runs
    // first), so it won't spam for ordinary followers.
    static void GatherLog(uint32 guidLow, std::string const& reason)
    {
        static thread_local std::unordered_map<uint64, uint32> lastMs;
        uint64 key = (uint64(guidLow) << 32) ^ std::hash<std::string>{}(reason);
        uint32 nowMs = getMSTime();
        uint32& last = lastMs[key];
        if (nowMs - last < 4000) return;
        last = nowMs;
        LOG_INFO("module", "[WowPsParty Gather] guid={} {}", guidLow, reason);
    }

    // Reads the assigned tank slot for an account from the formation cache.
    // -1 if none. Caller must NOT hold g_mutex.
    static int GetTankSlotForAccount(uint32 account)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_formations.find(account);
        if (it == g_formations.end()) return -1;
        return it->second.tankSlot;
    }

    // Reads a bot's party slot from the directive registry.
    // -1 if the bot isn't tracked.
    static int GetSlotForGuid(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto const& d : g_directives)
            if (d.followerGuid == guid) return int(d.slot);
        return -1;
    }

    // ---- per-member target-selection mode ----------------------------------
    // Cached so AssistTarget (runs every tick) never hits the DB. Separate
    // mutex from g_mutex to avoid lock-ordering tangles with the directive
    // registry that AssistTarget also reads.
    static std::unordered_map<uint32, std::string> g_targetMode;  // guidLow -> mode
    static std::mutex g_targetModeMutex;

    void TargetModeCacheSet(uint32 guidLow, std::string const& mode)
    {
        std::lock_guard<std::mutex> lock(g_targetModeMutex);
        if (mode.empty() || mode == "master")
            g_targetMode.erase(guidLow);   // default needs no entry
        else
            g_targetMode[guidLow] = mode;
    }

    std::string GetTargetMode(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(g_targetModeMutex);
        auto it = g_targetMode.find(guidLow);
        return it == g_targetMode.end() ? std::string("master") : it->second;
    }

    void TargetModeRefreshFromDB(uint32 guidLow)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `strategies_csv` FROM `party_loadout` WHERE `guid` = {}", guidLow);
        std::string mode = q ? q->Fetch()[0].Get<std::string>() : std::string();
        TargetModeCacheSet(guidLow, mode);
    }

    // ---- "lead in dungeons" toggle (tank) ----------------------------------
    // Whether this member is allowed to lead the recorded dungeon path + pull.
    // Stored in party_loadout.glyphs_csv ("0" = off). Default ON so existing
    // tanks keep leading; only an explicit "0" disables it.
    static std::unordered_map<uint32, bool> g_leadDungeon;  // guidLow -> false only when off
    static std::mutex g_leadMutex;

    void LeadDungeonCacheSet(uint32 guidLow, bool on)
    {
        std::lock_guard<std::mutex> lock(g_leadMutex);
        if (on) g_leadDungeon.erase(guidLow);   // default ON needs no entry
        else    g_leadDungeon[guidLow] = false;
    }

    bool GetLeadInDungeon(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(g_leadMutex);
        return g_leadDungeon.find(guidLow) == g_leadDungeon.end();  // absent = ON
    }

    void LeadDungeonRefreshFromDB(uint32 guidLow)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `glyphs_csv` FROM `party_loadout` WHERE `guid` = {}", guidLow);
        std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
        LeadDungeonCacheSet(guidLow, v != "0");
    }

    // ---- retarget throttle --------------------------------------------------
    // Timestamp (getMSTime) of each bot's last target SWITCH. Used to stop the
    // "spinbot" when several mobs flank the tank and the nearest-loose pick
    // would flip every tick: once a bot has a live valid victim, it won't
    // switch to a different one for RETARGET_COOLDOWN_MS — but a dead/gone
    // victim always retargets immediately.
    static constexpr uint32 RETARGET_COOLDOWN_MS = 3000;
    static std::unordered_map<uint32, uint32> g_lastRetargetMs;  // guidLow -> ms
    static std::mutex g_retargetMutex;

    static bool RetargetReady(uint32 guidLow, uint32 nowMs)
    {
        std::lock_guard<std::mutex> lock(g_retargetMutex);
        auto it = g_lastRetargetMs.find(guidLow);
        return it == g_lastRetargetMs.end()
            || (nowMs - it->second) >= RETARGET_COOLDOWN_MS;
    }

    static void MarkRetarget(uint32 guidLow, uint32 nowMs)
    {
        std::lock_guard<std::mutex> lock(g_retargetMutex);
        g_lastRetargetMs[guidLow] = nowMs;
    }

    // Nearest hostile (within `range`) whose current victim is NOT `bot` —
    // i.e. an add that's loose on the casters/healer. Returns nullptr if every
    // nearby hostile is already on the bot (or there are none). Built from the
    // party's attacker lists so it needs no grid search.
    static Unit* PickLooseTarget(Player* bot)
    {
        constexpr float LOOSE_MAX_RANGE = 30.0f;   // don't chase across the room
        Unit* best = nullptr;
        float bestDist = 1e9f;
        auto consider = [&](Unit* a)
        {
            if (!a || !a->IsAlive()) return;
            if (!a->IsInCombat()) return;               // never pull idle mobs
            if (a->GetVictim() == bot) return;          // already on us
            if (!bot->IsValidAttackTarget(a)) return;
            float const d = bot->GetDistance(a);
            if (d > LOOSE_MAX_RANGE) return;            // out of grab range
            if (d < bestDist) { bestDist = d; best = a; }
        };
        auto considerAttackersOf = [&](Unit* u)
        {
            if (!u || u->IsTotem()) return;   // don't grab a mob just hitting a totem
            for (Unit* a : u->getAttackers())
                consider(a);
        };
        considerAttackersOf(bot);                       // mobs on me but not targeting me (taunted off, etc.)
        for (Unit* ctrl : bot->m_Controlled)            // and mobs on my own pet
            considerAttackersOf(ctrl);
        if (Group* g = bot->GetGroup())
        {
            for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
            {
                Player* m = itr->GetSource();
                if (!m || !m->IsInWorld() || m == bot) continue;
                if (m->GetMapId() != bot->GetMapId()) continue;
                considerAttackersOf(m);
                for (Unit* ctrl : m->m_Controlled)      // a mob that pulled onto a member's PET
                    considerAttackersOf(ctrl);
            }
        }
        return best;
    }

    // The party tank's current victim, for focus-fire ("tank" mode). The tank
    // may be a follower bot (found via the directive registry) or the
    // controlled char itself (then the leader's victim is the tank's victim).
    static Unit* TankVictim(Player* bot, Player* leader)
    {
        int const tankSlot = GetTankSlotForAccount(bot->GetSession()->GetAccountId());
        if (tankSlot < 0) return leader ? leader->GetVictim() : nullptr;
        if (Group* g = bot->GetGroup())
        {
            for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
            {
                Player* m = itr->GetSource();
                if (!m) continue;
                if (GetSlotForGuid(m->GetGUID()) == tankSlot)
                    return m->GetVictim();          // bot tank
            }
        }
        // Tank isn't a follower bot → it's the controlled char (the leader).
        return leader ? leader->GetVictim() : nullptr;
    }

    void TankLeadEngagement(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld()) return;
        if (!bot->GetSession()) return;
        if (bot->HasUnitFlag(UNIT_FLAG_POSSESSED)) return;
        if (bot->IsNonMeleeSpellCast(false, false, true)) return;
        if (IsFollowerHeld(bot->GetGUID())) return;

        // Is this the lead tank? Role-based so a hired henchman tank counts.
        if (!IsLeadTank(bot->GetGUID())) return;
        if (!GetLeadInDungeon(bot->GetGUID().GetCounter())) return;  // leading disabled

        ObjectGuid const leaderGuid = GetLeaderFor(bot->GetGUID());
        if (!leaderGuid) return;
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader || !leader->IsInWorld()) return;
        if (leader->GetMapId() != bot->GetMapId()) return;
        if (!leader->GetMap() || !leader->GetMap()->IsDungeon()) return;

        // 30-yard leash. If we've drifted out of leash, let PartyFollow's
        // MoveFollow yank us back instead of chasing a far mob.
        if (bot->GetDistance(leader) > 30.0f) return;

        // Don't steal initiative from the user. If the leader's mid-fight
        // (or even just targeting something), AssistTarget already syncs
        // the tank's victim — we don't need to pick our own.
        if (leader->GetVictim()) return;

        // Already attacking something alive? leave it alone.
        if (Unit* v = bot->GetVictim())
            if (v->IsAlive()) return;

        // Find the nearest hostile to the leader within 40y of them.
        // SelectNearbyTarget(exclude, dist) returns the nearest unit that
        // `this` considers a valid attack target — perfect for "what's
        // about to fight us".
        Unit* nearest = leader->SelectNearbyTarget(nullptr, 40.0f);
        if (!nearest || !nearest->IsAlive()) return;
        if (!bot->IsValidAttackTarget(nearest)) return;

        bool const ok = bot->Attack(nearest, true);
        bot->SetFacingToObject(nearest);

        // Ranged pull: a tank holding a thrown/gun/bow doesn't charge into the
        // pack. It closes only to throwing range, then the rotation (Heroic Throw
        // etc.) and the ranged auto-attack pull a single mob while AssistTarget
        // holds it there (see IsTankPulling) so the mobs come to us instead of us
        // running head-first into a room full of them. Melee-only tanks keep the
        // old behaviour of closing straight in.
        Item* const rangedW = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        bool const canRangedPull = rangedW && rangedW->GetTemplate()->IsRangedWeapon();
        float const dist = bot->GetDistance(nearest);
        if (canRangedPull && dist > 8.0f)
        {
            if (dist > 26.0f)
                bot->GetMotionMaster()->MoveChase(nearest, 22.0f);   // close to throw range
            else
                bot->GetMotionMaster()->Clear();                     // hold ground
            MarkTankPulling(bot->GetGUID(), 8000);
        }
        else
            bot->GetMotionMaster()->MoveChase(nearest);              // melee: close in

        LOG_INFO("module", "[WowPsParty TankLead] guid={} {} mob_guid={} entry={} dist={:.1f} ok={}",
                 bot->GetGUID().GetCounter(), canRangedPull ? "RANGE-PULL" : "PULL",
                 nearest->GetGUID().GetCounter(), nearest->GetEntry(), dist, ok);
    }

    // ===== Gathering (mining / herbalism) ==================================
    //
    // A follower bot that the player trained in Mining or Herbalism will, while
    // OUT OF COMBAT and travelling with the party, peel off to harvest a nearby
    // node (within 30y) that's within its skill, then resume following. Only the
    // player's own alts gather — henchmen are temporary combat companions and
    // are skipped. There's no toggle: training the profession IS the opt-in.

    static constexpr float GATHER_SCAN_RANGE = 30.0f;  // node search radius
    static constexpr float GATHER_REACH      = 5.0f;   // interaction distance
    static constexpr float GATHER_LEADER_LEASH = 40.0f; // don't gather if lagging
    static constexpr uint32 GATHER_APPROACH_TIMEOUT_MS = 6000; // give up if stuck
    static constexpr uint32 GATHER_AVOID_MS = 30000;   // ignore an unreachable node

    // Per-bot gather state. Committing to one node stops the bot oscillating
    // between two equidistant nodes; the avoid slot remembers a node we gave up
    // reaching (wedged on geometry) so we don't immediately re-pick it and spin.
    struct GatherState
    {
        ObjectGuid node;            // node we're walking toward
        uint32     commitMs   = 0;  // when we committed (stuck timeout)
        ObjectGuid avoid;           // a node we abandoned as unreachable
        uint32     avoidUntil = 0;
    };
    static std::unordered_map<uint32, GatherState> g_gather;  // botLow -> state
    static std::mutex g_gatherMutex;

    // If `go` is a mining/herb node, return its profession skill + required
    // skill value. False for anything else (treasure chests, lockpick doors,
    // quest objects — their lock isn't a mining/herb skill lock).
    static bool NodeGatherSkill(GameObject* go, uint32& skillIdOut, uint32& reqOut)
    {
        if (!go) return false;
        LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
        if (!lock) return false;
        for (uint8 i = 0; i < 8; ++i)
        {
            if (lock->Type[i] != LOCK_KEY_SKILL) continue;
            uint32 const skillId = SkillByLockType(LockType(lock->Index[i]));
            if (skillId == SKILL_MINING || skillId == SKILL_HERBALISM)
            {
                skillIdOut = skillId;
                // The node's actual required skill — NOT floored to 2. A fresh
                // miner (skill 1) must be able to mine a Copper Vein (req 1);
                // flooring to 2 rejected every low-level node. Matches the
                // engine's Spell::CanOpenLock check (skillValue >= lock->Skill).
                reqOut     = lock->Skill[i];
                return true;
            }
        }
        return false;
    }

    // True if `bot` can gather `go` right now: spawned, ready (not mid-harvest),
    // a mining/herb node, bot has the profession AND enough skill, and hasn't
    // already harvested this spawn.
    static bool IsGatherableBy(Player* bot, GameObject* go)
    {
        if (!go || !go->isSpawned()) return false;
        if (go->getLootState() != GO_READY) return false;
        uint32 skillId = 0, req = 0;
        if (!NodeGatherSkill(go, skillId, req)) return false;
        if (!bot->HasSkill(skillId)) return false;
        if (uint32(bot->GetSkillValue(skillId)) < req) return false;
        if (go->IsInSkillupList(bot->GetGUID())) return false;
        return true;
    }

    static GameObject* FindNearestGatherNode(Player* bot, float range, ObjectGuid avoid)
    {
        std::list<GameObject*> gos;
        NearbySpawnedGOCheck check(bot, range);
        Acore::GameObjectListSearcher<NearbySpawnedGOCheck> searcher(bot, gos, check);
        Cell::VisitObjects(bot, searcher, range);

        GameObject* best = nullptr;
        float bestDist = range + 1.0f;
        for (GameObject* go : gos)
        {
            if (avoid && go->GetGUID() == avoid) continue;  // unreachable, skip
            if (!IsGatherableBy(bot, go)) continue;
            float const d = bot->GetDistance(go);
            if (d < bestDist) { bestDist = d; best = go; }
        }
        return best;
    }

    // Harvest the node directly into the bot's bags. Mirrors the essence of
    // Spell::EffectOpenLock (skill-up guarded by the per-GO skillup list, then
    // loot) but without a loot window — our party bots hard-return from
    // UpdateAI, so mod-playerbots' default loot AI never runs for them.
    static void GatherNode(Player* bot, GameObject* go)
    {
        uint32 skillId = 0, req = 0;
        if (!NodeGatherSkill(go, skillId, req)) return;

        bot->SetFacingToObject(go);

        if (!go->IsInSkillupList(bot->GetGUID()))
        {
            go->AddToSkillupList(bot->GetGUID());
            if (uint32 pure = bot->GetPureSkillValue(skillId))
                bot->UpdateGatherSkill(skillId, pure, req);
        }

        if (uint32 const lootId = go->GetGOInfo()->GetLootId())
            bot->AutoStoreLoot(lootId, LootTemplates_Gameobject, true);

        // Deplete it so it despawns + respawns like a real harvested vein.
        go->SetLootState(GO_JUST_DEACTIVATED);

        LOG_INFO("module",
            "[WowPsParty Gather] {} harvested go entry={} skill={} req={}",
            bot->GetName(), go->GetEntry(), skillId, req);
    }

    void TickGathering(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld() || !bot->GetSession()) return;
        if (bot->IsInCombat()) return;                     // only when idle
        if (bot->HasUnitFlag(UNIT_FLAG_POSSESSED)) return; // the controlled body
        if (bot->IsNonMeleeSpellCast(false, false, true)) return;
        if (IsHenchman(bot->GetGUID())) return;            // only the player's alts
        if (IsTankLeading(bot->GetGUID())) return;         // busy leading a dungeon

        // Fast skill gate — most bots have neither profession, exit immediately.
        bool const canMine = bot->HasSkill(SKILL_MINING);
        bool const canHerb = bot->HasSkill(SKILL_HERBALISM);
        if (!canMine && !canHerb) return;

        uint32 const gLow = bot->GetGUID().GetCounter();
        uint32 const now  = getMSTime();

        // Nowhere to put the mats — don't harvest (AutoStoreLoot silently drops
        // items that don't fit, which would deplete the node for nothing) and
        // don't even approach. Resumes once a bag slot frees up.
        if (bot->GetFreeInventorySpace() == 0) { GatherLog(gLow, "skip: bags full"); return; }

        ObjectGuid const leaderGuid = GetLeaderFor(bot->GetGUID());
        if (!leaderGuid) { GatherLog(gLow, "skip: no leader directive"); return; }
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader || !leader->IsInWorld()) { GatherLog(gLow, "skip: leader not in world"); return; }
        if (leader->GetMapId() != bot->GetMapId()) { GatherLog(gLow, "skip: leader other map"); return; }
        // Don't wander off to gather while still catching up to the party.
        if (bot->GetDistance(leader) > GATHER_LEADER_LEASH)
        { GatherLog(gLow, "skip: lagging leader (>40y)"); return; }

        // Read this bot's committed node + avoid entry.
        ObjectGuid committed, avoid;
        uint32 commitMs = 0, avoidUntil = 0;
        {
            std::lock_guard<std::mutex> lock(g_gatherMutex);
            auto& st  = g_gather[gLow];
            committed = st.node;
            commitMs  = st.commitMs;
            avoid     = st.avoid;
            avoidUntil = st.avoidUntil;
        }

        GameObject* node = committed
            ? ObjectAccessor::GetGameObject(*bot, committed) : nullptr;

        if (!IsGatherableBy(bot, node))
        {
            // Lost/invalid committed node — pick the nearest valid one, skipping
            // any node we recently gave up reaching.
            node = FindNearestGatherNode(bot, GATHER_SCAN_RANGE,
                                         (now < avoidUntil) ? avoid : ObjectGuid::Empty);
            std::lock_guard<std::mutex> lock(g_gatherMutex);
            auto& st = g_gather[gLow];
            st.node     = node ? node->GetGUID() : ObjectGuid::Empty;
            st.commitMs = node ? now : 0;
        }
        else if (commitMs && (now - commitMs) > GATHER_APPROACH_TIMEOUT_MS &&
                 !bot->IsWithinDistInMap(node, GATHER_REACH))
        {
            // Committed but can't reach it (wedged on geometry). Abandon it and
            // avoid re-picking it for a while; next tick re-scans for another.
            std::lock_guard<std::mutex> lock(g_gatherMutex);
            auto& st = g_gather[gLow];
            st.avoid      = node->GetGUID();
            st.avoidUntil = now + GATHER_AVOID_MS;
            st.node       = ObjectGuid::Empty;
            st.commitMs   = 0;
            return;
        }
        if (!node) return;

        if (bot->IsWithinDistInMap(node, GATHER_REACH))
        {
            GatherNode(bot, node);
            std::lock_guard<std::mutex> lock(g_gatherMutex);
            auto& st = g_gather[gLow];
            st.node = ObjectGuid::Empty;
            st.commitMs = 0;
        }
        else
        {
            // Walk to the node and keep the 1Hz follow re-asserter off us so it
            // doesn't yank us back to the leader mid-approach. MovePoint paths
            // around geometry (generatePath defaults true).
            HoldFollower(bot->GetGUID(), 2500);
            bot->SetFacingToObject(node);
            bot->GetMotionMaster()->MovePoint(0xA17,
                node->GetPositionX(), node->GetPositionY(), node->GetPositionZ());
        }
    }

    void AssistTarget(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld()) return;
        uint32 const gLow = bot->GetGUID().GetCounter();

        // User-controlled body: never touch its target/motion.
        if (bot->HasUnitFlag(UNIT_FLAG_POSSESSED)) { AssistLog(gLow, "skip: possessed"); return; }
        // Don't interrupt a cast in progress.
        if (bot->IsNonMeleeSpellCast(false, false, true)) { AssistLog(gLow, "skip: casting"); return; }
        // Rotation engine has parked this bot (drinking, etc).
        if (IsFollowerHeld(bot->GetGUID())) { AssistLog(gLow, "skip: held by rotation"); return; }

        ObjectGuid const leaderGuid = GetLeaderFor(bot->GetGUID());
        if (!leaderGuid)
        {
            AssistLog(gLow, "skip: no leader directive (not a managed bot)");
            return;
        }

        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader || !leader->IsInWorld()) { AssistLog(gLow, "skip: leader not in world"); return; }
        if (leader->GetMapId() != bot->GetMapId()) { AssistLog(gLow, "skip: leader on different map"); return; }

        // Party leash: once the leader is >50y away, stop engaging and let the
        // follow ticker bring us back (it walks at 50y, teleports past 100y).
        // Yielding here stops the AI tick from re-acquiring a target between
        // the 1Hz follow ticks.
        if (bot->GetDistance(leader) > 50.0f)
        {
            if (bot->GetVictim()) bot->AttackStop();
            AssistLog(gLow, "skip: beyond party leash (>50y) — rejoining leader");
            return;
        }

        // Target priority:
        //   1. Leader's explicit victim (you click, everyone follows).
        //   2. Whatever's currently swinging at the bot itself.
        //   3. Whatever's swinging at any party member on the same map.
        // Without #3 we got the "mage solos a mob while the whole party
        // stands around" bug: only the leader's target made anything fire.
        auto pickPartyDefenseTarget = [&]() -> Unit*
        {
            static constexpr float PARTY_DEFEND_RANGE = 30.0f;
            // self-defense — anything swinging at US or OUR pet, always (it's
            // already on us / right next to us, no range cap).
            for (Unit* a : bot->getAttackers())
                if (a && a->IsAlive() && bot->IsValidAttackTarget(a))
                    return a;
            for (Unit* ctrl : bot->m_Controlled)
                if (ctrl && !ctrl->IsTotem())   // a pet/guardian, not a fire-and-forget totem
                    for (Unit* a : ctrl->getAttackers())
                        if (a && a->IsAlive() && bot->IsValidAttackTarget(a))
                            return a;
            // party-defense via our directive roster (leader + all bots +
            // henchmen), NOT the WoW group — the group can form incompletely,
            // which left bots ignoring a member under attack. This is what makes
            // heroes and henchmen actually defend EACH OTHER. Capped to mobs
            // already NEAR us (PARTY_DEFEND_RANGE): a DPS must not sprint across a
            // room to peel for a distant ally — that pulls every pack on the way
            // and wipes the group. Distant threats are the tank's / leader's job.
            // A member's PET counts too: a mob that aggroed onto the leader's
            // (or a bot's) pet never shows in the player's getAttackers, so
            // without the pet scan the tank just stands around while a pet-pull
            // fight rages ("my pet pulls, the paladin does nothing").
            auto scanAttackers = [&](Unit* u) -> Unit*
            {
                if (!u || u->IsTotem()) return nullptr;   // don't peel for a totem
                for (Unit* a : u->getAttackers())
                    if (a && a->IsAlive() && bot->IsValidAttackTarget(a)
                        && bot->IsWithinDistInMap(a, PARTY_DEFEND_RANGE))
                        return a;
                return nullptr;
            };
            std::vector<ObjectGuid> party;
            GetPartyGuidsFor(bot->GetGUID(), party);
            for (ObjectGuid const& gg : party)
            {
                Player* m = ObjectAccessor::FindConnectedPlayer(gg);
                if (!m || !m->IsInWorld() || m == bot) continue;
                if (m->GetMapId() != bot->GetMapId()) continue;
                if (Unit* a = scanAttackers(m)) return a;
                for (Unit* ctrl : m->m_Controlled)
                    if (Unit* a = scanAttackers(ctrl)) return a;
            }
            return nullptr;
        };

        Unit* leaderTarget = leader->GetVictim();
        bool const leaderTargetValid = leaderTarget && leaderTarget->IsAlive()
                                       && bot->IsValidAttackTarget(leaderTarget);

        // Per-member target mode decides who we engage. "master" (default)
        // keeps the original behaviour; the others let the user tell the tank
        // to grab loose adds while everyone else focus-fires the tank's kill.
        std::string const mode = GetTargetMode(gLow);
        Unit* desired = nullptr;
        if (mode == "nearest")
        {
            desired = bot->SelectNearbyTarget(nullptr, 40.0f);
            if (!desired) desired = pickPartyDefenseTarget();
        }
        else if (mode == "loose")
        {
            // Prefer a loose add (in combat, not on us). If there isn't one,
            // fall back to party-defense — mobs attacking us or an ally — so
            // the tank still engages the lone mob already on him instead of
            // idling. pickPartyDefenseTarget is combat-only, so this never
            // pulls an idle/non-combat monster (the bug the nearest-mob
            // fallback used to cause).
            desired = PickLooseTarget(bot);
            if (!desired) desired = pickPartyDefenseTarget();
        }
        else if (mode == "tank")
        {
            Unit* tv = TankVictim(bot, leader);
            if (tv && tv->IsAlive() && bot->IsValidAttackTarget(tv))
                desired = tv;
            else
                desired = pickPartyDefenseTarget();
        }
        else // "master" / default
        {
            desired = leaderTargetValid ? leaderTarget : pickPartyDefenseTarget();
        }
        // Final safety: never hand back a dead/invalid target.
        if (desired && (!desired->IsAlive() || !bot->IsValidAttackTarget(desired)))
            desired = nullptr;

        // Retarget throttle. If we're already on a live, valid victim, don't
        // abandon it for a DIFFERENT one more than once per cooldown — that's
        // the spinbot fix when several mobs flank the tank. A dead/gone victim
        // (currentValid == false) drops straight through and retargets now.
        uint32 const nowMs = getMSTime();
        Unit* const current = bot->GetVictim();
        bool const currentValid = current && current->IsAlive()
                                  && bot->IsValidAttackTarget(current);
        if (currentValid)
        {
            if (!desired)
            {
                desired = current;   // nothing new worth switching to — stay put
            }
            else if (desired != current)
            {
                // Decide whether to abandon a LIVE victim for a different mob.
                //  master (DPS): ONLY the leader explicitly retargeting pulls us
                //    off — a party-defense pick must never make a DPS drop a
                //    half-dead mob to chase a fresh add (the "interrupts itself /
                //    pulls the room" bug). Finish the kill.
                //  other modes (tank grabbing loose adds): keep the throttled
                //    switch so the tank can peel.
                bool const allowSwitch = (mode == "master")
                    ? (leaderTargetValid && desired == leaderTarget && RetargetReady(gLow, nowMs))
                    : RetargetReady(gLow, nowMs);
                if (!allowSwitch)
                    desired = current;   // keep finishing the current victim
            }

            // Combo classes (rogue / feral druid) NEVER abandon a target they've
            // built combo points on — combo is lost on a switch, so finish the
            // cycle (build to a finisher) before moving on. This gives a rogue the
            // "focus one mob, don't spread combo" behaviour WITHOUT the room-pulling
            // "nearest" target mode. Once a finisher dumps combo back to 0 the
            // normal retarget rules above resume. (GetComboPoints() is 0 for every
            // non-combo class, so this is a no-op for them.)
            if (bot->GetComboPoints() > 0)
                desired = current;
        }

        if (!desired)
        {
            // Nothing to fight anywhere — drop combat so PartyFollow can
            // resume movement.
            if (bot->GetVictim())
            {
                AssistLog(gLow, "no-targets: AttackStop");
                bot->AttackStop();
            }
            return;
        }

        // Kite mode: the rotation has a keep_distance rule, so it OWNS the feet
        // (hops away from the enemy / toward the healer between casts). Lock the
        // victim + facing and yield all movement — installing the chase or the
        // dead-zone back-out here would fight the kite, and only one mover can run
        // at a time. Don't force-face while the bot is mid-hop (let the spline
        // steer); face the target when planted so the next cast lands.
        if (WowPsParty::BotIsKiting(bot->GetGUID()))
        {
            if (bot->GetVictim() != desired)
            {
                MarkRetarget(gLow, nowMs);
                bot->Attack(desired, false);   // ranged: never chase into melee
            }
            if (!bot->isMoving())
                bot->SetFacingToObject(desired);
            AssistLog(gLow, "kite mode: rotation owns movement");
            return;
        }

        // Lead-tank ranged pull. While the pull window is live and the target
        // hasn't closed to melee yet, hold ground (cancel any chase) and let the
        // mob come — the rotation throws to pull, AssistTarget doesn't drag the
        // tank into the pack. The instant the mob reaches melee (or the window
        // lapses) this falls through to the normal chase/engage below.
        if (IsTankPulling(bot->GetGUID()) && !bot->IsWithinMeleeRange(desired))
        {
            if (bot->GetVictim() != desired)
            {
                MarkRetarget(gLow, nowMs);
                bot->Attack(desired, true);
            }
            float const d = bot->GetDistance(desired);
            MovementGeneratorType const mg =
                bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
            if (d > 26.0f)
            {
                if (mg != CHASE_MOTION_TYPE)
                    bot->GetMotionMaster()->MoveChase(desired, 22.0f);
            }
            else if (mg == CHASE_MOTION_TYPE)
                bot->GetMotionMaster()->Clear();   // arrived at range — hold
            bot->SetFacingToObject(desired);
            AssistLog(gLow, "tank pull-hold: holding ground, letting mob close");
            return;
        }

        // ===== Engage + position ============================================
        // Single source of truth for combat movement (one mover, no clashes).
        uint8 const acls = bot->getClass();
        bool rangedCaster =
            acls == CLASS_MAGE   || acls == CLASS_WARLOCK || acls == CLASS_PRIEST ||
            acls == CLASS_HUNTER || acls == CLASS_SHAMAN  || acls == CLASS_DRUID;
        // Hybrids aren't ranged in every spec. A TANK is never ranged; an
        // ENHANCEMENT shaman or a FERAL druid (talent tree 1) fights in melee;
        // and a druid already shifted into bear/cat form is melee. Without this a
        // bear-tank druid kited to "firing range" forever, and an enhancement
        // shaman / feral cat would too. Role "tank" + the tree check both cover
        // the first tick before the bot has shifted form.
        if (rangedCaster)
        {
            bool const melee =
                RoleForGuid(bot->GetGUID()) == "tank" ||
                ((acls == CLASS_DRUID || acls == CLASS_SHAMAN)
                    && WowPsParty::PrimaryTalentTree(bot) == 1) ||
                bot->IsInFeralForm();
            if (melee) rangedCaster = false;
        }

        // Make sure the victim is set (drives auto-attack / has_target). Melee
        // gets a melee swing; ranged does NOT (it must never run into melee).
        bool const newVictim = bot->GetVictim() != desired;
        if (newVictim)
        {
            MarkRetarget(gLow, nowMs);
            bot->Attack(desired, !rangedCaster);
            LOG_INFO("module", "[WowPsParty Assist] guid={} ENGAGE victim_guid={} ranged={}",
                     gLow, desired->GetGUID().GetCounter(), rangedCaster ? 1 : 0);
        }

        MovementGeneratorType const mg =
            bot->GetMotionMaster()->GetCurrentMovementGeneratorType();

        if (rangedCaster)
        {
            // RANGED bands (AC's chase never enforces a MIN range, and ChaseAngle
            // ORBITS — it's relative to the mob's facing, so as the mob turns to
            // face the bot the target point moves and the bot chases it forever:
            // the "spazz on the same spot". So: no angle, and DON'T MOVE when
            // already in a safe firing position.
            //   < 8y        too close -> back straight out to ~18y
            //   8..30y +LoS SAFE      -> stand still and shoot (no movement)
            //   > 30y / noLoS         -> close in (plain chase, no angle), once
            float const d   = bot->GetDistance(desired);
            bool  const los = bot->IsWithinLOSInMap(desired);

            if (d < 8.0f)
            {
                // Forced into melee with no tank to peel: STAND AND FIGHT rather
                // than kite forever (backing out only drags the mob around the
                // room). Holding still also buys the pet time to reach the mob
                // and Growl it off; once the mob leaves us we resume ranged next
                // tick. Hunters flip on melee swings (their shots are dead-zoned
                // this close); casters just hold and keep casting point-blank.
                if (desired->GetVictim() == bot && !PartyHasLiveTank(bot))
                {
                    if (mg != IDLE_MOTION_TYPE)
                    {
                        bot->StopMoving();
                        bot->GetMotionMaster()->Clear();
                        bot->GetMotionMaster()->MoveIdle();
                    }
                    if (acls == CLASS_HUNTER)
                        bot->Attack(desired, true);   // white melee swings in the dead zone
                    bot->SetFacingToObject(desired);
                    AssistLog(gLow, "ranged: no tank, standing ground to fight in melee");
                    return;
                }
                // A tank will take it (or it's on someone else) — back out just
                // PAST the dead zone (13y), NOT all the way to 18y. A ranged
                // special shot's effective min range is ~10y for a normal mob
                // (spell min + melee range), so 13y is just clear of it: close
                // enough to stay (less running, safer indoors) yet far enough to
                // actually fire — 10y left the bot IN the dead zone, only able to
                // auto-shoot. The rotation's own too-close check nudges it further
                // for big mobs. Drop any melee.
                if (bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    bot->Attack(desired, false);
                if (mg != POINT_MOTION_TYPE)
                {
                    float bx, by, bz;
                    desired->GetNearPoint(bot, bx, by, bz, 0.0f, 13.0f, desired->GetAngle(bot));
                    bot->GetMotionMaster()->MovePoint(0, bx, by, bz);
                    AssistLog(gLow, "ranged: too close, backing out to firing range");
                }
                bot->SetFacingToObject(desired);
                return;
            }

            if (d <= 30.0f && los)
            {
                // SAFE — the user's "don't move when it can ranged attack". Kill
                // any leftover movement ONCE (so Auto Shot can fire), then leave
                // the feet completely alone; just keep facing the target.
                if (bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    bot->Attack(desired, false);   // back at range — stop meleeing, resume shots
                // CRUCIAL: a back-out (POINT motion) carries the bot from the <8y
                // dead zone out to ~18y, and it passes THROUGH this 8..30y band on
                // the way. Stopping all non-idle motion here cut the back-out at
                // the 8y edge, stranding the hunter next to the mob — next tick it
                // was <8y again → back-out → stopped at 8y → "kite out, walk back
                // into melee, forever". While the back-out is still running, leave
                // it; only hold once it (or a chase) has actually arrived.
                bool const backingOut =
                    (mg == POINT_MOTION_TYPE) && bot->isMoving();
                if (!backingOut && mg != IDLE_MOTION_TYPE)
                {
                    bot->StopMoving();
                    bot->GetMotionMaster()->Clear();
                    bot->GetMotionMaster()->MoveIdle();
                    AssistLog(gLow, "ranged: in firing range — holding position");
                }
                if (!bot->HasInArc(float(M_PI), desired))
                    bot->SetFacingToObject(desired);
                return;
            }

            // Out of range or no line of sight -> close in. Plain chase (no angle
            // = no orbit). Install once; a running chase keeps maintaining range.
            if (bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                bot->Attack(desired, false);   // chasing back to range — stop meleeing, resume shots
            if (mg != CHASE_MOTION_TYPE)
                bot->GetMotionMaster()->MoveChase(desired, ChaseRange(15.0f, 25.0f));
            bot->SetFacingToObject(desired);
            return;
        }

        // MELEE — close to contact, fanned out by formation angle so the melee
        // companions surround the mob (orbiting is fine when you're in contact).
        int const fi = FormationIndexFor(bot->GetGUID(), GetLeaderFor(bot->GetGUID()));
        float const chaseAngle = float(fi) * (2.0f * float(M_PI) / 5.0f);
        if (newVictim || mg != CHASE_MOTION_TYPE)
            bot->GetMotionMaster()->MoveChase(desired, {}, ChaseAngle(chaseAngle));
        else if (!bot->HasInArc(float(M_PI), desired))
            bot->SetFacingToObject(desired);
    }

    void ClearFollowersForAccount(uint32 account)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        EraseByAccount_NoLock(account);
        LOG_INFO("module", "[WowPsParty Follow] Cleared directives for account={}", account);
    }

    namespace
    {
        // Auto-vote GREED on every pending group-loot roll for this bot. We
        // hard-return out of mod-playerbots' UpdateAI, suppressing its default
        // loot-roll action, so without this our party bots (heroes AND hired
        // henchmen) never respond to a roll and the player has to click greed
        // for each. Kevin's rule: all party bots greed on everything when the
        // party is on group loot (which it is whenever a henchman is present).
        static void AutoGreedRolls(Player* bot)
        {
            if (!bot) return;
            Group* g = bot->GetGroup();
            if (!g) return;
            for (Roll* roll : g->GetRolls())
            {
                if (!roll) continue;
                auto it = roll->playerVote.find(bot->GetGUID());
                if (it == roll->playerVote.end() || it->second != NOT_EMITED_YET)
                    continue;
                g->CountRollVote(bot->GetGUID(), roll->itemGUID, GREED);
            }
        }

        // Apply a single directive: install MoveFollow if safe.
        // Returns true if the directive should be kept, false to mark
        // for removal (leader gone, etc.).
        bool ApplyDirective(Directive const& d)
        {
            Player* follower = ObjectAccessor::FindConnectedPlayer(d.followerGuid);
            Player* leader   = ObjectAccessor::FindConnectedPlayer(d.leaderGuid);
            if (!follower || !leader) return true;
            if (!follower->IsInWorld() || !leader->IsInWorld()) return true;
            if (follower == leader) return true;

            // Greed any pending loot rolls before the combat/death returns.
            AutoGreedRolls(follower);

            // Auto-accept a pending resurrect. A bot has no client to click
            // the "Accept" dialog, so a healer's Resurrection / Redemption
            // sets the request data but the bot would otherwise lie dead
            // forever (Kevin saw the paladin never stand back up). Accept on
            // its behalf, then scrub the death-state motion the same way the
            // .party rez command does, or MoveFollow can't drive the revived
            // bot and the stuck-detector teleports it every few yards.
            if (!follower->IsAlive() && follower->isResurrectRequested())
            {
                follower->ResurectUsingRequestData();
                follower->GetMotionMaster()->Clear();
                follower->GetMotionMaster()->MoveIdle();
                follower->StopMoving();
                LOG_INFO("module",
                    "[WowPsParty Follow] {} auto-accepted resurrect",
                    follower->GetName());
                return true;
            }

            // Universal dead->alive scrub. ResurrectPlayer / ResurectUsingRequestData
            // leave stale death-state motion on the MotionMaster; without a clean
            // slate MoveFollow can't drive the revived bot and the catch-up-teleport
            // stuck-detector pops it every few yards ("only teleports after a wipe").
            // The auto-accept and `.party rez` already scrub, but a dungeon wipe can
            // revive bots by a path that hits NEITHER (spirit healer at the graveyard,
            // resurrect-at-corpse, a battle-rez we didn't auto-accept). Catch the
            // dead->alive EDGE here so every revive path gets the same clean slate.
            {
                static thread_local std::unordered_map<uint32, bool> wasAlive;
                uint32 const gl = d.followerGuid.GetCounter();
                bool const nowAlive = follower->IsAlive();
                auto it = wasAlive.find(gl);
                // First sight: assume no transition (don't scrub a bot that was
                // alive all along — only a genuine dead->alive edge matters).
                bool const prevAlive = (it == wasAlive.end()) ? nowAlive : it->second;
                wasAlive[gl] = nowAlive;
                if (nowAlive && !prevAlive)
                {
                    follower->GetMotionMaster()->Clear();
                    follower->GetMotionMaster()->MoveIdle();
                    follower->StopMoving();
                    if (follower->getStandState() != UNIT_STAND_STATE_STAND)
                        follower->SetStandState(UNIT_STAND_STATE_STAND);
                    LOG_INFO("module",
                        "[WowPsParty Follow] {} revived — scrubbed death-state motion",
                        follower->GetName());
                    return true;   // one clean settle tick; MoveFollow re-asserts next tick
                }
            }

            // Cross-map: leader has entered a dungeon (or any other instance)
            // and the follower is still on the old map. Yank the follower
            // through with TeleportTo. Skip while the follower is mid-cast or
            // currently being teleported themselves; we'll retry next tick.
            if (follower->GetMapId() != leader->GetMapId())
            {
                if (follower->IsBeingTeleported()) return true;
                if (follower->IsNonMeleeSpellCast(false, false, true)) return true;
                follower->TeleportTo(
                    leader->GetMapId(),
                    leader->GetPositionX(), leader->GetPositionY(),
                    leader->GetPositionZ(), leader->GetOrientation());
                LOG_INFO("module",
                    "[WowPsParty Follow] cross-map teleport: {} -> map={} (chasing {})",
                    follower->GetName(), leader->GetMapId(), leader->GetName());
                return true;
            }

            // Persistently disable mod-playerbots' follow strategy on each
            // tick. UpdateAIGroupMaster (called from PlayerbotAI::UpdateAI
            // during combat) re-enables "+follow" via FindNewMaster, so a
            // one-shot disable from SetActiveFollowers gets undone after
            // any fight. Re-disabling on every tick is idempotent if already
            // off. Done BEFORE the combat/cast early-returns so it persists
            // even when we skip the actual MoveFollow install.
            if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(follower))
                ai->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);

            // Mount matching — keep the follower's mounted state synced with the
            // leader's so the party doesn't trail on foot during travel. The bot
            // mounts its OWN level/race-appropriate mount (not a clone of the
            // leader's), matching only the leader's ground-vs-flying type. Skip
            // while in combat / casting (can't mount then anyway).
            if (!follower->IsInCombat()
                && !follower->IsNonMeleeSpellCast(false, false, true))
            {
                bool const leaderMounted = leader->IsMounted();
                bool const botMounted    = follower->IsMounted();
                if (leaderMounted && !botMounted)
                {
                    // Is the leader on a FLYING mount? (a mount aura whose speed
                    // effect is flight speed, not ground.)
                    bool leaderFlying = false;
                    Unit::AuraEffectList const& m =
                        leader->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
                    if (!m.empty())
                        if (SpellInfo const* ls = m.front()->GetSpellInfo())
                            leaderFlying =
                                ls->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                                ls->Effects[2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED;

                    if (uint32 const mountSpell = ChooseBotMountSpell(follower, leaderFlying))
                        follower->CastSpell(follower, mountSpell, true);
                }
                else if (!leaderMounted && botMounted)
                {
                    follower->Dismount();
                }
            }

            // ---- Party leash ----------------------------------------------
            // If the controlled char has run off, the bot abandons whatever
            // it's doing (combat, drinking, holding) and rejoins. >50y: break
            // off and walk back. >100y: snap directly to the leader. Runs
            // BEFORE the combat / hold / cast early-returns so it overrides
            // them. AssistTarget + TickRotation also yield past 50y so the AI
            // tick doesn't re-engage between these 1Hz follow ticks.
            {
                float const leaderDist = follower->GetDistance(leader);
                if (leaderDist > 100.0f)
                {
                    if (follower->GetVictim()) follower->AttackStop();
                    if (follower->IsInCombat()) follower->CombatStop();
                    if (follower->getStandState() != UNIT_STAND_STATE_STAND)
                        follower->SetStandState(UNIT_STAND_STATE_STAND);
                    follower->GetMotionMaster()->Clear();
                    follower->StopMoving();
                    follower->TeleportTo(leader->GetMapId(),
                        leader->GetPositionX(), leader->GetPositionY(),
                        leader->GetPositionZ(), leader->GetOrientation());
                    LOG_INFO("module",
                        "[WowPsParty Leash] {} >100y from leader — teleport in",
                        follower->GetName());
                    return true;
                }
                if (leaderDist > 50.0f)
                {
                    if (follower->GetVictim()) follower->AttackStop();
                    if (follower->IsInCombat()) follower->CombatStop();
                    if (follower->getStandState() != UNIT_STAND_STATE_STAND)
                        follower->SetStandState(UNIT_STAND_STATE_STAND);
                    follower->GetMotionMaster()->Clear();
                    follower->GetMotionMaster()->MoveFollow(leader, PET_FOLLOW_DIST,
                        follower->GetFollowAngle());
                    return true;
                }
            }

            // Hands-off if the bot is already engaging something — combat
            // state in AC only flips on damage exchange, but a bot that's
            // m_attacking a mob (sword raised) needs the chase generator
            // installed by AssistTarget to actually walk into melee. Old
            // check only saw IsInCombat() so a warrior with a target but
            // no aggro got MoveFollow'd back to the leader and never
            // closed the distance.
            if (follower->IsInCombat())   return true;
            if (follower->GetVictim())    return true;
            if (follower->IsCharmed())    return true;

            // The PARTY is fighting but THIS bot momentarily isn't — e.g. a tank
            // that just lost all its threat, or a ranged dps between targets.
            // Do NOT drag it to a follow/lead position; yield so AssistTarget
            // re-engages it (the tank taunts loose mobs, ranged hold at range).
            // Without this the lead tank walks to the front and stops holding
            // aggro ("tank breaks mid-combat"), and ranged get pulled into the
            // leader's melee.
            //
            // GetVictim() as well as IsInCombat(): AC only flips IsInCombat() on
            // a DAMAGE exchange, so during the pull window (tank charging in,
            // sword raised, GetVictim() set, first hit not yet landed) the party
            // still reads out-of-combat — and a trailing ranged bot follows the
            // tank straight into melee before AssistTarget can hold it at range,
            // then backs out once damage flips combat. That's the "runs forward
            // into the mob for no reason, once per pull, then backs out". Yielding
            // on a member's victim closes that gap so the hold happens first.
            {
                std::vector<ObjectGuid> party;
                GetPartyGuidsFor(d.followerGuid, party);
                for (ObjectGuid const& gg : party)
                {
                    if (gg == d.followerGuid) continue;
                    Player* m = ObjectAccessor::FindConnectedPlayer(gg);
                    if (m && m->IsInWorld() && m->IsAlive()
                        && m->GetMapId() == follower->GetMapId()
                        && (m->IsInCombat() || m->GetVictim()))
                        return true;
                }
            }

            // Rotation engine has asked us to leave this bot stationary.
            // Active hold = drinking, holding-for-healer-mana, etc.
            if (IsFollowerHeld(d.followerGuid)) return true;

            // Tank is actively leading the dungeon route — hands off entirely.
            // TankFollowPath owns its movement; if we re-assert MoveFollow or
            // run the "stuck = constant distance" catch-up teleport here, a
            // tank idling at its lookahead (because the leader stopped) gets
            // yanked onto the leader after ~3 ticks. The >100y hard leash
            // above still rescues a genuinely-lost tank.
            if (IsTankLeading(d.followerGuid)) return true;

            // DON'T skip if follower->GetCharm() -- the session player
            // (Kevtank) IS the controller during a swap, has a charm, but
            // we WANT his body to walk toward the new controlled body.
            // Earlier "MotionMaster destabilises charm" theory turned out
            // to be the mod-playerbots AI interaction, not MoveFollow per
            // se. Now that PartyMgr.cpp doesn't poke the AI on swap, this
            // is safe.

            // Skip if mid-cast. Re-asserting MoveFollow interrupts spells
            // (UNIT_STATE_CASTING gets cleared by movegen change). Wait
            // until cast finishes, then re-assert on the next tick.
            if (follower->IsNonMeleeSpellCast(false, false, true))
                return true;

            // (We used to skip re-asserting when the follower was within
            // PET_FOLLOW_DIST + 1.5y of the leader to save packets. That
            // broke role-aware positioning: a tank starting stacked on the
            // leader needs to be REPOSITIONED 12y ahead, even when dist is
            // small. We always re-assert now; MoveSpline caches the spline
            // state so identical re-asserts produce no extra packets.)
            float const dist = follower->GetDistance(leader);

            // Catch-up teleport: MoveFollow can't reliably path through
            // doorways, building interiors, or across complex terrain. We
            // use two conditions:
            //   (a) distance > 50y → always teleport.
            //   (b) bot has been at the *same* distance > 8y for 3+
            //       consecutive ticks → it's stuck; teleport.
            // Tracker is keyed by follower guid.
            uint32 const guidLow = d.followerGuid.GetCounter();
            // Stuck = the follower is far from the leader AND has barely moved in
            // WORLD space for several ticks. The previous detector compared the
            // follower's DISTANCE TO THE LEADER and called it stuck when that
            // stayed constant — but a bot FOLLOWING a moving leader holds that
            // distance roughly constant (that IS following), so a bot walking a
            // few yards behind a continuously-walking leader got falsely flagged
            // and teleported on nearly every step (legs never moving). Tracking
            // the follower's OWN movement fixes it: a walking bot is moving, so
            // it's never flagged; only a genuinely frozen one is.
            struct StuckSample { float x = 0.0f, y = 0.0f; uint32 idle = 0; };
            static thread_local std::unordered_map<uint32, StuckSample> stuckTracker;
            StuckSample& s = stuckTracker[guidLow];
            float const dxs = follower->GetPositionX() - s.x;
            float const dys = follower->GetPositionY() - s.y;
            float const movedSelf = std::sqrt(dxs * dxs + dys * dys);
            s.x = follower->GetPositionX();
            s.y = follower->GetPositionY();
            if (dist > 8.0f && movedSelf < 1.0f) ++s.idle;
            else                                  s.idle = 0;
            bool const farAway    = dist > 50.0f;
            bool const stuckClose = s.idle >= 3;   // far + not moving for 3 ticks
            if (farAway || stuckClose)
            {
                if (follower->IsBeingTeleported())  return true;
                if (follower->IsNonMeleeSpellCast(false, false, true)) return true;
                follower->TeleportTo(
                    leader->GetMapId(),
                    leader->GetPositionX(), leader->GetPositionY(),
                    leader->GetPositionZ(), leader->GetOrientation());
                LOG_INFO("module",
                    "[WowPsParty Follow] catch-up teleport: {} dist={:.1f} idle_ticks={}",
                    follower->GetName(), dist, s.idle);
                s = StuckSample{};
                return true;
            }

            // CHARMER skipped entirely. Earlier attempts (MoveFollow, then
            // NearTeleportTo) both triggered AC paths that cleared
            // UNIT_FLAG_POSSESSED on the controlled body, breaking possess
            // and letting the AI take over (confirmed in user testing
            // 2026-05-28). Tank/vacated body stays put during possess --
            // user manually walks back or swaps to tank to retrieve him.
            // Trade UX nicety for charm stability; charm stability is the
            // critical requirement.
            if (follower->GetCharm()) return true;

            // Spread followers around the leader so they don't stack on a
            // single point. Layout depends on context:
            //
            //   Open-world: all followers fan out BEHIND the leader, on
            //     four fixed bearings — slot 1 rear-right, slot 2 rear-
            //     left, slot 3 directly behind, slot 4 right flank.
            //   Dungeon + a designated tank: the tank takes a lead
            //     position 12y AHEAD of the leader (angle PI). The other
            //     followers still fan out behind. MoveFollow's leash keeps
            //     the tank from running off — they only re-position once
            //     the leader walks, so if the user pauses the tank pauses.
            // Distinct rear-arc bearings so companions fan out instead of
            // stacking. Indexed by FORMATION ORDINAL (not account slot), so
            // henchmen — which have no slot — also spread. Extra companions
            // beyond the table wrap to an outer ring.
            static constexpr float FORM_ANGLES[6] = {
                float(M_PI),            // directly behind
                float(M_PI) * 0.72f,    // behind-left
                float(M_PI) * 1.28f,    // behind-right
                float(M_PI) * 0.5f,     // left flank
                float(M_PI) * 1.5f,     // right flank
                float(M_PI) * 0.9f,     // back-left inner
            };

            bool const inDungeon = leader->GetMap() && leader->GetMap()->IsDungeon();
            bool const isLeadTank = inDungeon && IsLeadTank(d.followerGuid);

            float angle = follower->GetFollowAngle();
            float followDist = PET_FOLLOW_DIST;
            if (isLeadTank)
            {
                // If the dungeon has a recorded path, the path-follow
                // ticker drives the tank's motion. Skip MoveFollow so the
                // two systems don't fight each other on every tick.
                if (WowPsParty::GetPathWaypointCount(leader->GetMapId()) >= 2)
                    return true;
                // MoveFollow's angle is relative to the leader's facing:
                // 0 = directly in front, M_PI = directly behind (see FORM_ANGLES
                // above). The lead tank must be IN FRONT — the old M_PI here put
                // it 12y BEHIND the leader, which is exactly the "tank trails far
                // behind" bug. Keep it a few yards ahead so it body-pulls.
                angle = 0.0f;
                followDist = 8.0f;
            }
            else
            {
                int const fi = FormationIndexFor(d.followerGuid, d.leaderGuid);
                angle = FORM_ANGLES[fi % 6];
                followDist = PET_FOLLOW_DIST + float(fi / 6) * 2.5f;
            }

            // If the bot was sitting (post-drink), stand up before moving
            // so the spline doesn't fight the seated stand-state.
            if (follower->getStandState() != UNIT_STAND_STATE_STAND)
                follower->SetStandState(UNIT_STAND_STATE_STAND);

            follower->GetMotionMaster()->Clear();
            follower->GetMotionMaster()->MoveFollow(
                leader, followDist, angle);

            static thread_local std::unordered_map<uint32, uint32> lastLogMs;
            uint32 nowMs = getMSTime();
            uint32& last = lastLogMs[d.followerGuid.GetCounter()];
            if (nowMs - last > 10000)
            {
                last = nowMs;
                LOG_INFO("module",
                    "[WowPsParty Follow] tick: {} -> MoveFollow {} dist={:.1f}",
                    follower->GetName(), leader->GetName(), dist);
            }
            return true;
        }

    } // anonymous namespace

    // WorldScript OnUpdate hook -- ticks ~every 1s, applies all directives.
    // File-scope class (not in anon ns) so AddPartyFollowScripts can `new` it.
    class PartyFollowWorldScript : public WorldScript
    {
    public:
        PartyFollowWorldScript() : WorldScript("PartyFollowWorldScript", {
            WORLDHOOK_ON_UPDATE
        }) { }

        void OnUpdate(uint32 diff) override
        {
            // Always tick path-recording even if no directives exist —
            // recording happens before the user has assigned tanks/healers.
            WowPsParty::TickPathRecording(diff);

            _accum += diff;
            if (_accum < TICK_INTERVAL_MS) return;
            _accum = 0;

            std::vector<Directive> snapshot;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                snapshot = g_directives;
            }

            for (auto const& d : snapshot)
                (void)ApplyDirective(d);
        }

    private:
        uint32 _accum = 0;
    };

    void InstallFollowTicker()
    {
        // No-op now: registration moved to AddPartyFollowScripts() called from
        // the module loader at server startup (proper AC script-init point).
        // Kept as a stable public no-op so callers don't need to be removed.
    }
}

// Module-loader entry point; called from party_of_5_loader.cpp at startup.
void AddPartyFollowScripts()
{
    new WowPsParty::PartyFollowWorldScript();
    LOG_INFO("module", "[WowPsParty Follow] ticker registered (interval=1000ms)");
}

// Trampoline for the patched mod-playerbots UpdateAI. Lets us avoid an
// include cycle between the two modules.
bool WowPsParty_BotHasActiveFollowDirective_Trampoline(ObjectGuid guid)
{
    return WowPsParty::BotHasActiveFollowDirective(guid);
}

// Trampoline: is this bot a hired henchman? The patched UpdateAI uses this to
// keep our follow/tank-lead but let DEFAULT mod-playerbots AI run combat.
bool WowPsParty_IsHenchman_Trampoline(ObjectGuid guid)
{
    return WowPsParty::IsHenchman(guid);
}

// Trampoline for the patched mod-playerbots UpdateAI to run our minimal
// combat assist instead of the default strategy engine.
void WowPsParty_AssistTarget_Trampoline(Player* bot)
{
    WowPsParty::AssistTarget(bot);
}

// Tank-lead trampoline — same dispatch point, called immediately after
// AssistTarget so dungeon-tank pulls fire when the leader is idle.
void WowPsParty_TankLeadEngagement_Trampoline(Player* bot)
{
    WowPsParty::TankLeadEngagement(bot);
}

// Tank path-follow trampoline — walks the tank along the recorded path.
// Gated to the ASSIGNED TANK only; the other followers just MoveFollow the
// leader. Without this gate every party bot tried to walk the path and
// conga-lined through the dungeon.
void WowPsParty_TankFollowPath_Trampoline(Player* bot)
{
    if (!bot || !bot->GetSession()) return;
    // Lead tank = lowest-guid follower with role "tank" (alt OR henchman).
    if (!WowPsParty::IsLeadTank(bot->GetGUID())) return;
    if (!WowPsParty::GetLeadInDungeon(bot->GetGUID().GetCounter())) return;  // user disabled leading
    WowPsParty::TankFollowPath(bot);
}

// Gathering trampoline — out-of-combat mining/herbalism for the player's alts.
void WowPsParty_TickGathering_Trampoline(Player* bot)
{
    WowPsParty::TickGathering(bot);
}

// Trampoline from the patched core LFGMgr::JoinLfg: our managed party bots
// (alts + henchmen) have their AI paused, so they never answer the dungeon-
// finder role check on their own — it would stall until it times out. Set each
// one's role-check role from its assigned WowPsParty role (account_party.role
// for alts / the henchman directive), so the role check completes with the
// party's real tank/healer/dps layout. No-op when the group has no party bots.
void WowPsParty_SetPartyBotLfgRoles_Trampoline(ObjectGuid groupGuid)
{
    Group* grp = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
    if (!grp)
        return;

    for (GroupReference* itr = grp->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* m = itr->GetSource();
        if (!m)
            continue;
        ObjectGuid const g = m->GetGUID();
        // Only our managed party bots (alts + henchmen) — never the real player.
        if (!WowPsParty::BotHasActiveFollowDirective(g))
            continue;

        std::string const role = WowPsParty::RoleForGuid(g);
        uint8 lfgRole = lfg::PLAYER_ROLE_DAMAGE;
        if (role == "tank")
            lfgRole = lfg::PLAYER_ROLE_TANK;
        else if (role == "healer")
            lfgRole = lfg::PLAYER_ROLE_HEALER;

        sLFGMgr->UpdateRoleCheck(groupGuid, g, lfgRole);
        LOG_INFO("module",
            "[WowPsParty LFG] role-check: bot guid={} -> {}",
            g.GetCounter(), role.empty() ? "dps" : role.c_str());
    }
}
