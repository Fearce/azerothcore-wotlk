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
#include "PathGenerator.h"   // NavReachable: is a recovery spot actually walkable?
#include "StringFormat.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "ThreatManager.h"   // threat-cap throttle: bots back off near the tank's threat
#include "CombatManager.h"   // tank gather window: count mobs in combat with the tank
#include "SpellInfo.h"   // SpellInfo::Effects[] for the leader's mount-type check
#include "Vehicle.h"     // vehicle behaviour: GetVehicleKit / seats / passengers
#include "Battlefield.h"     // re-group gate: don't reform the party mid Wintergrasp war
#include "BattlefieldMgr.h"  // sBattlefieldMgr->GetBattlefieldToZoneId

// Gathering (mining / herbalism) for follower bots.
#include "Bag.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Creature.h"
#include "Formulas.h"   // Acore::XP::GetGrayLevel — skip trivially-low (gray) mobs from auto-pull
#include "DBCStores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "LootMgr.h"
#include "ObjectMgr.h"   // sObjectMgr->GetItemTemplate — henchman-loot under-threshold check

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "WorldPacket.h"
#include "Opcodes.h"

// Battleground entry for managed party bots (gated AI never clicks "Enter Battle").
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Random.h"   // urand/frand for the movement-humanization jitter + wander

#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <utility>
#include <vector>

// Defined in PartyHooks.cpp (global scope, same symbol the Spell.cpp skinning
// patch calls): finish a party-killed corpse's leftover normal loot and flag it
// UNIT_FLAG_SKINNABLE. Our bots leave corpse loot unfinished, so the engine
// usually never sets that flag — without this a bot skinner finds nothing to skin.
bool WowPsParty_ForceSkinReady(Player* skinner, Creature* creature);

namespace WowPsParty
{
    bool IsLogVerbose();   // from PartyBootstrap.cpp

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

        // Predicate for the grid searcher: a dead, skinnable-TYPE corpse within
        // range that the party killed AND hasn't been skinned yet. We deliberately
        // do NOT require the engine's UNIT_FLAG_SKINNABLE here — our bots leave
        // corpse loot unfinished, so that flag is usually never set, and gating on
        // it meant bots never skinned anything. A genuine party kill (loot
        // recipient/group set) is force-flagged at harvest. CRITICAL: once skinned,
        // SkinCorpse stamps loot_type = LOOT_SKINNING — exclude those, or the
        // recipient/group (which stay set) would re-detect the same corpse every
        // tick and skin it 20-30 times (leather + skill dupe). The per-bot
        // skill/level gate happens in the caller.
        struct NearbySkinnableCheck
        {
            NearbySkinnableCheck(WorldObject const* src, float range)
                : _src(src), _range(range) {}
            bool operator()(Creature* c) const
            {
                if (!c || c->IsAlive() || !_src->IsWithinDist(c, _range, false))
                    return false;
                CreatureTemplate const* tmpl = c->GetCreatureTemplate();
                if (!tmpl || tmpl->SkinLootId == 0) return false;   // not a skinnable beast
                if (c->loot.loot_type == LOOT_SKINNING) return false;  // already skinned
                return c->HasUnitFlag(UNIT_FLAG_SKINNABLE)
                    || c->GetLootRecipient() || c->GetLootRecipientGroup();
            }
            WorldObject const* _src;
            float _range;
        };

        // Does `self` (a henchman) actually have something to loot on this corpse?
        // GOLD (split across the party no matter who loots it) OR an under-threshold
        // item that round-robin assigned to THIS henchman. Items being ROLLED for
        // (over-threshold greens) and other members' round-robin trash are NOT ours.
        // Without this gate the henchman walks to a corpse it can take nothing from
        // and, because the corpse keeps UNIT_DYNFLAG_LOOTABLE while the roll runs,
        // re-targets it forever — the "stuck walking to a corpse being rolled" bug.
        // Computed by item QUALITY vs the group loot threshold, NOT li.is_underthreshold
        // — the latter is only set once a player opens the corpse (GroupLoot), which
        // hasn't happened at grid-scan time.
        static bool HenchClaimable(Creature const* c, ObjectGuid self, Group const* grp)
        {
            Loot const& loot = c->loot;
            // Only walk to a corpse that round-robin assigned to THIS henchman. The old
            // `if (gold>0) return true` made EVERY henchman (and the player's corpses)
            // claimable to all of them just for the shared gold, so they swarmed and
            // walked on top of the corpse the human was trying to loot (Kevin). Each
            // member loots its OWN assigned corpses and WoW splits the gold to the party
            // regardless of who opens it, so nothing is stranded — and a henchman never
            // crowds the player's (or another bot's) corpse. An UNASSIGNED gold corpse
            // (no round-robin owner) is still fair game so loose gold isn't left behind.
            if (loot.roundRobinPlayer != self)
                return loot.roundRobinPlayer.IsEmpty() && loot.gold > 0;
            if (loot.gold > 0) return true;                    // our corpse: take the gold
            uint32 const threshold = grp ? uint32(grp->GetLootThreshold())
                                         : uint32(ITEM_QUALITY_UNCOMMON);
            for (LootItem const& li : loot.items)
            {
                if (li.is_looted || li.freeforall) continue;
                ItemTemplate const* t = sObjectMgr->GetItemTemplate(li.itemid);
                if (t && t->Quality < threshold) return true;  // under-threshold trash that's OURS
            }
            return false;
        }

        // Predicate for the henchman corpse-loot grid search: a dead creature
        // within range that the henchman's OWN WoW group killed, still carries the
        // native UNIT_DYNFLAG_LOOTABLE "sparkle", and has loot THIS henchman may
        // actually take (HenchClaimable). LOOT_SKINNING corpses are the alts' skinner.
        struct NearbyLootableCorpseCheck
        {
            NearbyLootableCorpseCheck(WorldObject const* src, float range, Group* grp, ObjectGuid self)
                : _src(src), _range(range), _self(self), _grp(grp) {}
            bool operator()(Creature* c) const
            {
                if (!c || c->IsAlive() || !_src->IsWithinDist(c, _range, false))
                    return false;
                if (!c->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
                    return false;                    // nothing lootable left
                if (c->loot.loot_type == LOOT_SKINNING)
                    return false;                    // skin loot — not ours to take
                if (c->GetLootRecipientGroup() != _grp)
                    return false;                    // only our group's kills
                return HenchClaimable(c, _self, _grp);  // ignore rolls / other members' trash
            }
            WorldObject const* _src;
            float _range;
            ObjectGuid _self;
            Group* _grp;
        };

        // Matches a live, attackable enemy whose name is in the focus list (names
        // pre-lowercased by the caller). Drives the rotation "focus:" override —
        // a must-kill add (Chaos Rift on Anomalus, Frost Tomb in Utgarde Keep).
        struct NearbyFocusEnemyCheck
        {
            NearbyFocusEnemyCheck(Player const* src, std::vector<std::string> const& lowNames, float range)
                : _src(src), _names(lowNames), _range(range) {}
            bool operator()(Creature* c) const
            {
                if (!c || !c->IsAlive()) return false;
                if (!_src->IsWithinDist(c, _range, false)) return false;
                if (!_src->IsValidAttackTarget(c)) return false;
                std::string n = c->GetName();
                for (char& ch : n) if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
                for (std::string const& fn : _names) if (n == fn) return true;
                return false;
            }
            Player const* _src;
            std::vector<std::string> const& _names;
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

        // followerGuidLow -> getMSTime() at which a "Come Hither" recall ends. Read
        // ONLY by TickRotation, to PAUSE the rotation during the recall so a ranged
        // DPS runs to the leader instead of hard-casting in place (faceAndCast would
        // re-plant it each tick). Deliberately separate from g_holdUntilMs: drink /
        // hold_position use that one and the rotation must KEEP running for them
        // (their actions re-assert the hold every tick).
        static std::unordered_map<uint32, uint32> g_recallUntilMs;

        // stop_attacking hold: guidLow -> expiry. While live the bot suppresses ALL
        // offence (AssistTarget won't engage; the rotation drops offensive verbs) but
        // heals/buffs still run. Re-armed each tick the stop_attacking rule's condition
        // holds, so it lapses shortly after the condition (e.g. Mirrored Soul) clears.
        static std::unordered_map<uint32, uint32> g_offensiveHoldUntilMs;

        static std::mutex                g_mutex;
        static std::vector<Directive>    g_directives;
        // account -> the ACTIVE leader's own account_party role. The leader isn't a
        // follower, so it has no Directive (RoleForGuid can't see it); captured in
        // SetActiveFollowers so the human-tank wait-gate can read the leader's role.
        static std::unordered_map<uint32, std::string> g_leaderRole;
        static std::atomic<bool>         g_tickerInstalled{false};
        static constexpr uint32          TICK_INTERVAL_MS = 1000;

        // ---- Leader-fall detection ------------------------------------------
        // When the leader jumps off a ledge / into a hole there's often NO navmesh
        // path down, so MoveFollow can't reach and followers stall at the rim —
        // and the >100y leash never fires because the drop is closer than that.
        // The 1 Hz pass samples each leader's Z; a sudden drop arms a delayed
        // teleport (a landing grace, re-armed while the leader keeps falling), and
        // when it comes due ApplyDirective snaps any STRANDED follower onto the
        // leader. g_leaderFall is touched ONLY on the world thread (OnUpdate +
        // ApplyDirective, which OnUpdate calls), so it needs no lock.
        struct LeaderFallState
        {
            float  lastX        = 0.0f;
            float  lastY        = 0.0f;
            float  lastZ        = 0.0f;
            // Sentinel (no real map id) so the FIRST sample always takes the
            // re-baseline branch — map 0 IS a real map (Eastern Kingdoms), so a
            // default 0 here would skip re-baselining and read a spurious drop
            // from lastZ=0 at a negative-Z spot.
            uint32 lastMapId    = 0xFFFFFFFFu;
            uint32 teleportDueMs = 0;   // 0 = unarmed; else getMSTime() to snap followers
        };
        static std::unordered_map<uint32, LeaderFallState> g_leaderFall;  // leaderGuidLow -> state
        // >this Z lost in ONE ~1s pass = a fall. 5y (was 10): smaller jumps strand
        // bots too — e.g. the Dalaran Sewers arena pipe-to-floor drop (~6-7y) left
        // them stuck up top. The per-pass sampling is what keeps this from firing on
        // gradual descents: a jump loses 5y+ in one second, stairs/slopes far less.
        static constexpr float  FALL_DROP_Z         = 5.0f;
        static constexpr float  FALL_MAX_XY         = 25.0f;  // but ignore big XY jumps (a teleport, not a fall)
        static constexpr uint32 FALL_LAND_GRACE_MS  = 3000;   // wait this long after the drop before snapping
        // Only snap a follower at least this far out (3D) — a bot that fell ALONG is
        // closer. 6y (was 8) so a bot stranded above a modest drop still qualifies
        // even when the leader lands right below it.
        static constexpr float  FALL_SNAP_MIN_DIST  = 6.0f;

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

        // JOIN characters so a deleted-char orphan row never gets a follow
        // directive (which would also wrongly inflate followers_installed and
        // gate UpdateAI for a guid that can't spawn).
        QueryResult q = CharacterDatabase.Query(
            "SELECT ap.`guid`, ap.`slot`, COALESCE(ap.`role`, 'dps') "
            "FROM `account_party` ap "
            "JOIN `characters` c ON c.`guid` = ap.`guid` "
            "WHERE ap.`account` = {}", account);
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
            if (memberGuid == leaderGuid)
            {
                // The leader gets no directive, but remember its role so the
                // human-tank wait-gate (AssistTarget) can tell a tank lead apart.
                g_leaderRole[account] = memberRole.empty() ? "dps" : memberRole;
                continue;
            }

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

    // Every tracked follower (henchmen + enrolled alts), across all leaders.
    void GetAllFollowers(std::vector<ObjectGuid>& out)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        out.reserve(out.size() + g_directives.size());
        for (auto const& d : g_directives)
            out.push_back(d.followerGuid);
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
        // No FOLLOWER directive: this guid may be the LEADER, which gets none (only its
        // role is stashed in g_leaderRole). A follower's directive records its account +
        // leaderGuid, so find any directive that points at this guid as the leader and
        // return that account's leader role. WITHOUT this, a role condition misclassifies
        // the leader as roleless → am_tank=false / am_dps=true / !am_tank=true: a tank that
        // is (even briefly, mid control-switch) the leader fires !am_tank rules like
        // move_behind that it should be excluded from (Kevin: "the tank walks behind").
        for (auto const& d : g_directives)
            if (d.leaderGuid == botGuid)
            {
                auto it = g_leaderRole.find(d.account);
                return it == g_leaderRole.end() ? std::string() : it->second;
            }
        return std::string();
    }

    // The active leader's own account_party role (captured in SetActiveFollowers).
    // Distinct from RoleForGuid, which only knows FOLLOWER directives.
    std::string LeaderRole(uint32 account)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_leaderRole.find(account);
        return it == g_leaderRole.end() ? std::string() : it->second;
    }

    // Set the cached leader role for `guid`'s account from persistent storage:
    // account_party.role if the character is enrolled, else its per-character
    // party_loadout.role (the SOLO / un-enrollable case), else "dps". Called on the
    // controlled character's login and whenever it changes its own role, so a solo
    // player can tank/heal on a non-enrolled character and henchmen read it right.
    void SetLeaderRoleForChar(uint32 account, ObjectGuid guid)
    {
        uint32 const low = guid.GetCounter();
        std::string role;
        if (QueryResult q = CharacterDatabase.Query(
                "SELECT `role` FROM `account_party` WHERE `guid` = {}", low))
            role = q->Fetch()[0].Get<std::string>();
        else if (QueryResult q2 = CharacterDatabase.Query(
                "SELECT `role` FROM `party_loadout` WHERE `guid` = {}", low))
            role = q2->Fetch()[0].Get<std::string>();
        if (role.empty()) role = "dps";
        std::lock_guard<std::mutex> lock(g_mutex);
        g_leaderRole[account] = role;
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

    // True if `memberGuid`'s party has a LIVE tank right now — a tank-role
    // follower (alt bot / henchman) OR the human leader set to tank — alive and
    // in-world. Ranged bots read this to STOP kiting when a tank is present: with
    // a tank they should stand at range behind it and let it hold threat, not hop
    // around (which drags mobs and pulls them off the tank onto the caster). When
    // there's no tank, kiting stays on (the tankless "stand and fight"/kite logic).
    bool PartyHasLiveTank(ObjectGuid memberGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        uint32 account = 0;
        ObjectGuid leaderGuid;
        for (auto const& d : g_directives)
            if (d.followerGuid == memberGuid)
                { account = d.account; leaderGuid = d.leaderGuid; break; }
        if (!account) return false;
        auto liveTank = [](ObjectGuid g) -> bool
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(g);
            return p && p->IsAlive() && p->IsInWorld();
        };
        // Human leader set to tank?
        auto lr = g_leaderRole.find(account);
        if (lr != g_leaderRole.end() && lr->second == "tank" && liveTank(leaderGuid))
            return true;
        // A tank-role follower (bot/henchman) alive + in-world?
        for (auto const& d : g_directives)
            if (d.account == account && d.role == "tank" && liveTank(d.followerGuid))
                return true;
        return false;
    }

    // The party's LIVE healer (human leader set to healer, or a healer-role
    // follower), or nullptr if there isn't one. Used to pace the lead tank's
    // pulls on the healer's mana. Mirrors PartyHasLiveTank's directive walk.
    Player* FindPartyHealer(ObjectGuid memberGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        uint32 account = 0;
        ObjectGuid leaderGuid;
        for (auto const& d : g_directives)
            if (d.followerGuid == memberGuid)
                { account = d.account; leaderGuid = d.leaderGuid; break; }
        if (!account) return nullptr;
        auto alive = [](ObjectGuid g) -> Player*
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(g);
            return (p && p->IsAlive() && p->IsInWorld()) ? p : nullptr;
        };
        auto lr = g_leaderRole.find(account);
        if (lr != g_leaderRole.end() && lr->second == "healer")
            if (Player* p = alive(leaderGuid)) return p;
        for (auto const& d : g_directives)
            if (d.account == account && d.role == "healer")
                if (Player* p = alive(d.followerGuid)) return p;
        return nullptr;
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

    bool IsBeingRecalled(ObjectGuid followerGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_recallUntilMs.find(followerGuid.GetCounter());
        if (it == g_recallUntilMs.end()) return false;
        if (getMSTime() >= it->second)
        {
            g_recallUntilMs.erase(it);
            return false;
        }
        return true;
    }

    void MarkOffensiveHold(ObjectGuid followerGuid, uint32 holdMs)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_offensiveHoldUntilMs[followerGuid.GetCounter()] = getMSTime() + holdMs;
    }

    bool IsOffensiveHeld(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_offensiveHoldUntilMs.find(guid.GetCounter());
        if (it == g_offensiveHoldUntilMs.end()) return false;
        if (getMSTime() >= it->second)
        {
            g_offensiveHoldUntilMs.erase(it);
            return false;
        }
        return true;
    }

    // Cleanse hold (mirror of the offensive hold): while live the bot suppresses its
    // dispel/cure (cure_party) but everything else runs. The `stop_cleansing` rotation
    // action re-arms it each tick its condition holds, so ONE Common rule can stop the
    // WHOLE party from cleansing a debuff that explodes on dispel (Mutating Injection)
    // until it's safe — no per-character cure rule edits.
    static std::unordered_map<uint32, uint32> g_cleanseHoldUntilMs;
    void MarkCleanseHold(ObjectGuid followerGuid, uint32 holdMs)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cleanseHoldUntilMs[followerGuid.GetCounter()] = getMSTime() + holdMs;
    }
    bool IsCleanseHeld(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_cleanseHoldUntilMs.find(guid.GetCounter());
        if (it == g_cleanseHoldUntilMs.end()) return false;
        if (getMSTime() >= it->second)
        {
            g_cleanseHoldUntilMs.erase(it);
            return false;
        }
        return true;
    }

    // Force a freshly-revived / stuck bot back into a MOVABLE state.
    //
    // Root/stun/etc. live in two places: the unit-state mask (m_state, gated by
    // the Follow/Chase MovementGenerators via UNIT_STATE_NOT_MOVE) and the
    // movement-info flags (MOVEMENTFLAG_ROOT). A HUMAN player clears these on
    // revive because the server round-trips the unroot through their game
    // client; a BOT has no client (GetClientControlling() == nullptr), so a
    // movement-blocking state can survive death->revive and silently freeze the
    // generators — the bot then stands still and the catch-up teleport pops it
    // every few seconds ("teleport around after a wipe"), and a revived healer
    // can't walk to buff/rez anyone ("doesn't move to friendly targets").
    // Clearing is safe here: callers run OUT OF COMBAT (revive edge, or the
    // follow ticker which has already returned if IsInCombat), so no legitimate
    // CC is active and death already stripped every aura.
    void ForceMovableState(Player* p)
    {
        if (!p) return;
        // The MovementGenerators gate only on the unit-state mask (m_state, via
        // UNIT_STATE_NOT_MOVE) and the movement-info flags (MOVEMENTFLAG_ROOT).
        // Clearing both directly is all a clientless bot needs to move again;
        // SetRooted() (protected) would only re-send a packet no bot client
        // reads. Auras were stripped on death, so nothing re-applies these.
        p->ClearUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_DIED
                          | UNIT_STATE_DISTRACTED | UNIT_STATE_NO_COMBAT_MOVEMENT);
        p->RemoveUnitMovementFlag(MOVEMENTFLAG_ROOT);
        // A clientless bot revived by some paths keeps a stale/zero movement speed (the
        // speed update normally round-trips through the player's client, which a bot
        // lacks). A follow generator then installs but its spline can't progress at 0
        // speed — unitState=FOLLOW yet moveFlags=0, frozen — so the catch-up teleport
        // pops it every few yards instead of walking ("teleports after a res"). Recompute
        // every speed from base+auras so it can actually move. Forced + clientless = a
        // pure server-side state set, no packet a bot would need to ack.
        for (uint8 mt = 0; mt < MAX_MOVE_TYPE; ++mt)
            p->UpdateSpeed(UnitMoveType(mt), true);
    }

    // MotionMaster point id for the "Come Hither" recall (just needs to be unique
    // among our explicit MovePoints so arrival callbacks don't collide).
    static constexpr uint32 RECALL_POINT_ID = 0xCA11;

    void RecallFollowers(Player* leader, uint32 holdMs)
    {
        if (!leader || !leader->IsInWorld()) return;
        ObjectGuid const leaderGuid = leader->GetGUID();
        uint32 const leaderMap = leader->GetMapId();
        float const lx = leader->GetPositionX();
        float const ly = leader->GetPositionY();
        float const lz = leader->GetPositionZ();

        // Snapshot the followers UNDER the lock, then act WITHOUT it: HoldFollower
        // takes the same (non-recursive) mutex, so holding it across the loop would
        // self-deadlock.
        std::vector<ObjectGuid> followers;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto const& d : g_directives)
                if (d.leaderGuid == leaderGuid)
                    followers.push_back(d.followerGuid);
        }

        for (ObjectGuid const& g : followers)
        {
            Player* bot = ObjectAccessor::FindPlayer(g);
            if (!bot || !bot->IsInWorld() || !bot->IsAlive()) continue;
            if (bot->GetMapId() != leaderMap) continue;
            // Interrupt any in-progress cast FIRST: a ranged DPS mid-Frostbolt would
            // otherwise keep re-casting in place (faceAndCast re-plants it every
            // rotation tick) and ignore the recall — exactly the reported bug.
            bot->InterruptNonMeleeSpells(true);
            // Run to the leader's exact spot — pathfinding + unit collision spread
            // them into a tight cluster. MovePoint overrides any in-progress
            // chase/kite; the holds keep the follow ticker + combat assist (via
            // IsFollowerHeld) AND the rotation (via IsBeingRecalled) off them.
            bot->GetMotionMaster()->MovePoint(RECALL_POINT_ID, lx, ly, lz);
            HoldFollower(g, holdMs);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_recallUntilMs[g.GetCounter()] = getMSTime() + holdMs;
            }
        }
    }

    // ===== Manual "pull one more" (keybind) ================================
    // tankLow -> live pull-more window: run the lead tank to + engage ONE specific
    // out-of-combat mob until `untilMs`. Re-armed on each keypress; the driver
    // (TickTankPullMore) reads it every tick and clears it once the mob aggros.
    struct PullMoreState { uint32 untilMs = 0; ObjectGuid target; };
    static std::unordered_map<uint32, PullMoreState> g_tankPullMore;

    static void SetTankPullMore(uint32 tankLow, ObjectGuid target, uint32 untilMs)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        PullMoreState& s = g_tankPullMore[tankLow];
        s.untilMs = untilMs;
        s.target  = target;
    }

    // Returns the armed target, or false (erasing the entry) once the window has
    // lapsed — so a lapsed pull-more stops driving and normal AI resumes.
    static bool GetTankPullMore(uint32 tankLow, ObjectGuid& targetOut)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tankPullMore.find(tankLow);
        if (it == g_tankPullMore.end()) return false;
        if (getMSTime() >= it->second.untilMs) { g_tankPullMore.erase(it); return false; }
        targetOut = it->second.target;
        return true;
    }

    static void ClearTankPullMore(uint32 tankLow)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_tankPullMore.erase(tankLow);
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

    // tankGuidLow -> live ranged-pull state. While the window is set, AssistTarget
    // holds the tank at range and lets the pulled mob close instead of chasing into
    // the pack ("don't face-pull a whole room"), and every OTHER party bot holds
    // fire (IsPartyPullPending) until the pack reaches the tank. The window is
    // refreshed each tick the mob is still inbound, so it survives the back-up; it
    // expires shortly after the mob lands (or if nothing comes). `retreatSet` marks
    // that the one-shot back-out point has been chosen so we don't kite forever.
    struct TankPullState
    {
        uint32 untilMs    = 0;
        uint32 startMs    = 0;
        bool   retreatSet = false;
        float  rx = 0.0f, ry = 0.0f, rz = 0.0f;
    };
    static std::unordered_map<uint32, TankPullState> g_tankPull;

    static void MarkTankPulling(ObjectGuid tankGuid, uint32 durationMs)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        TankPullState& s = g_tankPull[tankGuid.GetCounter()];
        if (getMSTime() >= s.untilMs)   // a FRESH pull (prior window lapsed)
        {
            s.retreatSet = false;       // — recompute the back-out point
            s.startMs    = getMSTime(); // — and restart the overall wait clock
        }
        s.untilMs = getMSTime() + durationMs;
    }

    // Total time since this pull began (0 if none) — caps how long the party
    // waits for a mob to close, so a pulled RANGED mob that plinks the tank from
    // afar (keeping it in combat) doesn't freeze the party indefinitely.
    static uint32 TankPullElapsedMs(uint32 tankLow)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tankPull.find(tankLow);
        if (it == g_tankPull.end() || it->second.startMs == 0) return 0;
        return getMSTime() - it->second.startMs;
    }

    // Hard cap on the pull-wait. Melee mobs cross even a long pull + back-up well
    // inside this; AT the cap the tank stops waiting and engages normally (closing
    // to the mob), releasing the party — so a RANGED mob that never walks into
    // melee can't kite the tank forever. The pull-hold ENDS the window at the cap
    // (ClearTankPulling), it doesn't just stop refreshing, so the tank engages in
    // ~5s, not 5s + the rolling-refresh residual.
    static constexpr uint32 PULL_MAX_WAIT_MS = 5000;

    // Settle window after a fight before the lead tank auto-pulls the next pack —
    // gives the party time to loot, regroup, and start drinking instead of being
    // yanked straight into the next group.
    static constexpr uint32 POST_COMBAT_PULL_DELAY_MS = 5000;

    // Mana the pull-pacing gate waits for before the lead tank proactively opens the
    // next pack ("don't chain-pull on fumes"). NOT "topped off": a healer between
    // dungeon pulls rarely sits at a literal 99% — it took chip damage, regen lags a
    // tick, or the player walks the party into the next pack (proximity aggro) before
    // the bar fills. A 99% bar is unreachable in practice and DEADLOCKED the body-pull
    // — the tank held forever on "healer mana <99%", OPEN+GATHER never fired, and combat
    // started by proximity/DPS with no gather (Mill: "no body pulls even with cap 7, it
    // just engages immediately"). 50% is just a FLOOR against chain-pulling a near-empty
    // healer: Mill wants the tank to open at 70% without waiting ("should not wait to
    // engage based on healer mana, i can be 70% and it's still OK"), so the gate must sit
    // well below that — it only holds a genuinely drained (<50%) healer for a few seconds
    // of regen, never a healer that's merely below full.
    static constexpr uint32 PULL_READY_MANA_PCT = 50;

    bool IsTankPulling(ObjectGuid tankGuid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tankPull.find(tankGuid.GetCounter());
        if (it == g_tankPull.end()) return false;
        if (getMSTime() >= it->second.untilMs)
        {
            g_tankPull.erase(it);
            return false;
        }
        return true;
    }

    // End a pull window NOW (so the tank engages normally and the party releases),
    // not after the rolling-refresh window drains. Used when the wait cap is hit.
    static void ClearTankPulling(uint32 tankLow)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_tankPull.erase(tankLow);
    }

    // The tank's chosen back-out point during a pull, or false if not set yet.
    static bool GetTankPullRetreat(uint32 tankLow, float& x, float& y, float& z)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tankPull.find(tankLow);
        if (it == g_tankPull.end() || !it->second.retreatSet) return false;
        x = it->second.rx; y = it->second.ry; z = it->second.rz;
        return true;
    }

    static void SetTankPullRetreat(uint32 tankLow, float x, float y, float z)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        TankPullState& s = g_tankPull[tankLow];
        s.rx = x; s.ry = y; s.rz = z; s.retreatSet = true;
    }

    // How far the tank steps straight back after its ranged pull connects — pulls
    // the pack into open space, away from neighbouring packs, and lets the tank
    // lock threat before the melee piles in. (The stand-off the tank fires FROM is
    // per-class via WowPsParty::TankPullHoldRange — a long opener pulls from ~26y,
    // a short one like Icy Touch from ~16y.)
    static constexpr float PULL_RETREAT_YDS = 12.0f;

    // ===== Tank initial MULTI-PULL (pull_count:N) ===========================
    //
    // A lead tank can open on a CLUSTER of up to N mobs (default 3) instead of one,
    // when the party is strong enough. Distinct from the single-mob ranged-pull-and-
    // isolate flow (g_tankPull above): a multi-pull BODY-PULLS a tight cluster so the
    // pack stacks on the tank's melee, and the party holds fire (IsPartyPullPending)
    // until the tank has built a real THREAT lead on the pack — giving it time to lock
    // them. The release is PURE THREAT, no timer (Kevin: "we don't like timers, base it
    // on threat just like the human tank"): the gather completes exactly when the bot
    // tank holds an engage lead on its cluster, mirroring the human-tank wait-gate
    // (TankHasEngageLead). Only an INITIAL pull triggers it (TankLeadEngagement already
    // requires the whole party out of combat + rested AND the tank to have no live
    // victim), so it never chain-pulls: the party still eats/drinks between packs.
    struct TankGatherState
    {
        uint32                  untilMs = 0;   // GC backstop only — see TANK_GATHER_GC_MS / IsTankGathering
        std::vector<ObjectGuid> set;           // the mobs this opener means to gather
    };
    static std::unordered_map<uint32, TankGatherState> g_tankGather;   // tankLow -> state

    // GC backstop: the gather hold releases on THREAT once the tank engages (see
    // IsTankGathering), not on this clock. It bounds the one case threat can't — a pull
    // that gets STUCK before the tank ever engages (mob evaded/unreachable mid-approach,
    // so no engage-lead ever lands) — and evicts an abandoned entry (tank DC'd). Long
    // enough to never cut off a legit approach + threat build (~a few seconds), short
    // enough that a stuck pull doesn't freeze the party for long.
    static constexpr uint32 TANK_GATHER_GC_MS  = 12000;
    static constexpr float  PULL_Z_TOLERANCE   = 6.0f;   // "similar Z-level" as the tank (used by the maintain-N gather)

    static void MarkTankGathering(uint32 tankLow, std::vector<ObjectGuid> set)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        uint32 const now = getMSTime();
        // Opportunistic prune: a solo tank's gather entry is never read by
        // IsPartyPullPending (no other members), so sweep GC-expired entries here so the
        // map can't slowly accumulate stale tank guids.
        for (auto it = g_tankGather.begin(); it != g_tankGather.end(); )
            it = (it->first != tankLow && now >= it->second.untilMs)
                     ? g_tankGather.erase(it) : std::next(it);
        TankGatherState& s = g_tankGather[tankLow];
        s.untilMs = now + TANK_GATHER_GC_MS;
        s.set     = std::move(set);
    }

    // Pure-threat gather gate. Defined below the threat helpers it mirrors
    // (TankHasEngageLead / IsBossUnit); forward-declared here, beside its state.
    static bool IsTankGathering(Player* tank);


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

    // The account's hired HENCHMAN guid-counters. The addon needs this to tell a
    // managed henchman apart from a SECOND HUMAN sharing the WoW group: both are
    // non-alt group members, and without this the client wrongly tags the other
    // player as a henchman (shows them in the rotation editor, etc.).
    void GetHenchmanGuidsForAccount(uint32 account, std::vector<uint32>& out)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto const& d : g_directives)
            if (d.account == account && d.henchman)
                out.push_back(d.followerGuid.GetCounter());
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

    // Throttled per (bot, reason) diagnostic for the henchman corpse-loot path.
    static void HenchLootLog(uint32 guidLow, std::string const& reason)
    {
        static thread_local std::unordered_map<uint64, uint32> lastMs;
        uint64 key = (uint64(guidLow) << 32) ^ std::hash<std::string>{}(reason);
        uint32 nowMs = getMSTime();
        uint32& last = lastMs[key];
        if (nowMs - last < 4000) return;
        last = nowMs;
        LOG_INFO("module", "[WowPsParty HenchLoot] guid={} {}", guidLow, reason);
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

    // ---- "wait for tank threat" toggle (DPS) -------------------------------
    // Whether a DPS bot holds / throttles under the human tank's threat before
    // it engages (so it doesn't rip aggro) or blasts instantly. Stored in
    // party_loadout.wait_tank_threat as '' (unset), '1' (wait) or '0' (blast).
    // The cache holds only EXPLICIT overrides; an absent entry falls back to the
    // per-type default (henchman -> wait, hero -> blast) in GetWaitTankThreat.
    static std::unordered_map<uint32, int> g_waitTankThreat;  // guidLow -> 0/1 (explicit only)
    static std::mutex g_waitTankMutex;

    // val: 0 = explicit blast, 1 = explicit wait, <0 = clear (back to default).
    void WaitTankThreatCacheSet(uint32 guidLow, int val)
    {
        std::lock_guard<std::mutex> lock(g_waitTankMutex);
        if (val < 0) g_waitTankThreat.erase(guidLow);
        else         g_waitTankThreat[guidLow] = val ? 1 : 0;
    }

    bool GetWaitTankThreat(ObjectGuid guid)
    {
        {
            std::lock_guard<std::mutex> lock(g_waitTankMutex);
            auto it = g_waitTankThreat.find(guid.GetCounter());
            if (it != g_waitTankThreat.end()) return it->second != 0;
        }
        // Unset: per-type default. IsHenchman takes a DIFFERENT lock, so this
        // call is outside the g_waitTankMutex scope above (no nested locking).
        return IsHenchman(guid);   // henchman -> wait, hero -> blast as it used to
    }

    // Wait-for-tank default WHEN A HUMAN TANK IS LEADING (caller gates on
    // HumanTankLeadActive). There the whole point is "let me hold aggro", so a DPS
    // waits BY DEFAULT — including hero alts, which GetWaitTankThreat would default
    // to BLAST (the "my own DPS rip threat off me, no chance to grab aggro" bug).
    // An explicit per-bot toggle still wins: wait_tank_threat='0' opts that DPS back
    // into blasting.
    bool WaitForHumanTank(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_waitTankMutex);
        auto it = g_waitTankThreat.find(guid.GetCounter());
        if (it != g_waitTankThreat.end()) return it->second != 0;   // explicit override
        return true;                                                // default: wait for the human tank
    }

    void WaitTankThreatRefreshFromDB(uint32 guidLow)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `wait_tank_threat` FROM `party_loadout` WHERE `guid` = {}", guidLow);
        std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
        WaitTankThreatCacheSet(guidLow, v == "1" ? 1 : (v == "0" ? 0 : -1));
    }

    // ---- "safe pull" toggle (TANK) -----------------------------------------
    // Whether the lead tank OPENS a pack with a ranged pull + step-back (tag the
    // mob, back off, let the pack close in open space) or just barges straight
    // into melee. Stored in party_loadout.safe_pull as '' (unset), '1' (safe
    // pull) or '0' (barge). Like wait_tank_threat it has a per-type default
    // (GetSafePull): a HERO/alt tank safe-pulls by default, a HENCHMAN barges
    // (the ranged opener is unnecessary for hired fill). An explicit toggle wins.
    static std::unordered_map<uint32, int> g_safePull;   // guidLow -> 0/1 (explicit only)
    static std::mutex g_safePullMutex;

    // val: 0 = explicit barge, 1 = explicit safe pull, <0 = clear (back to default).
    void SafePullCacheSet(uint32 guidLow, int val)
    {
        std::lock_guard<std::mutex> lock(g_safePullMutex);
        if (val < 0) g_safePull.erase(guidLow);
        else         g_safePull[guidLow] = val ? 1 : 0;
    }

    bool GetSafePull(ObjectGuid guid)
    {
        {
            std::lock_guard<std::mutex> lock(g_safePullMutex);
            auto it = g_safePull.find(guid.GetCounter());
            if (it != g_safePull.end()) return it->second != 0;
        }
        // Unset: OFF by default for EVERYONE — the tank BODY-PULLS (walks in) by default
        // and only does the ranged-pull-and-step-back opener when safe_pull is explicitly
        // enabled in the editor (Mill: "taunt/ranged is only for safe-pull, which should
        // default to disabled"). Previously heroes safe-pulled by default, which made the
        // opener stand at range and Heroic-Throw instead of walking in.
        return false;
    }

    void SafePullRefreshFromDB(uint32 guidLow)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `safe_pull` FROM `party_loadout` WHERE `guid` = {}", guidLow);
        std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
        SafePullCacheSet(guidLow, v == "1" ? 1 : (v == "0" ? 0 : -1));
    }

    // ---- "anchor on tank" toggle (NON-TANK) --------------------------------
    // When ON for a non-tank bot, that bot formation-follows the party TANK
    // instead of the human leader (only while the leader isn't the tank), so
    // melee reach the front fast when the leader is ranged. Stored in
    // party_loadout.anchor_tank as '' (unset), '1' (anchor) or '0' (off).
    // Unlike safe_pull there is NO per-type default: unset = OFF everywhere.
    static std::unordered_map<uint32, int> g_anchorTank;   // guidLow -> 0/1 (explicit only)
    static std::mutex g_anchorTankMutex;

    // val: 0 = explicit off, 1 = explicit anchor, <0 = clear (back to default OFF).
    void AnchorTankCacheSet(uint32 guidLow, int val)
    {
        std::lock_guard<std::mutex> lock(g_anchorTankMutex);
        if (val < 0) g_anchorTank.erase(guidLow);
        else         g_anchorTank[guidLow] = val ? 1 : 0;
    }

    bool BotAnchorOnTank(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_anchorTankMutex);
        auto it = g_anchorTank.find(guid.GetCounter());
        if (it != g_anchorTank.end()) return it->second != 0;
        return false;   // unset -> OFF everywhere (no per-type default)
    }

    void AnchorTankRefreshFromDB(uint32 guidLow)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `anchor_tank` FROM `party_loadout` WHERE `guid` = {}", guidLow);
        std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
        AnchorTankCacheSet(guidLow, v == "1" ? 1 : (v == "0" ? 0 : -1));
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

    // --- Ranged-caster anti-bunching (the corner spread step) ---------------
    // When several casters chase the same victim around a tight corner they all
    // plant on the first pixel where LoS+range clear, stacking on one point. On
    // the chase->arrive transition we let a stacked caster take ONE step a few
    // yards into the open (fanned by its formation index). The cooldown stops it
    // re-firing every tick once the chase generator resumes under the POINT move.
    static constexpr float  RANGED_SPREAD_RADIUS  = 5.0f;   // "stacked" if a peer is this close
    static constexpr uint32 SPREAD_STEP_COOLDOWN_MS = 6000;
    static std::unordered_map<uint32, uint32> g_lastSpreadMs;  // guidLow -> ms
    static std::mutex g_spreadMutex;

    static bool SpreadStepReady(uint32 guidLow, uint32 nowMs)
    {
        std::lock_guard<std::mutex> lock(g_spreadMutex);
        auto it = g_lastSpreadMs.find(guidLow);
        return it == g_lastSpreadMs.end()
            || (nowMs - it->second) >= SPREAD_STEP_COOLDOWN_MS;
    }

    static void MarkSpreadStep(uint32 guidLow, uint32 nowMs)
    {
        std::lock_guard<std::mutex> lock(g_spreadMutex);
        g_lastSpreadMs[guidLow] = nowMs;
    }

    // Throttle for the in-combat ranged back-out's safe-spot grid scan. A bot boxed
    // in by a fresh pack (no safe retreat → holding) would otherwise re-run the scan
    // every ~100ms AI tick; the surroundings change slowly, so re-evaluate ~1.4x/s.
    static constexpr uint32 BACKOUT_SCAN_COOLDOWN_MS = 700;
    static std::unordered_map<uint32, uint32> g_lastBackoutScanMs;  // guidLow -> ms
    static std::mutex g_backoutScanMutex;

    static bool BackoutScanReady(uint32 guidLow, uint32 nowMs)
    {
        std::lock_guard<std::mutex> lock(g_backoutScanMutex);
        auto it = g_lastBackoutScanMs.find(guidLow);
        return it == g_lastBackoutScanMs.end()
            || (nowMs - it->second) >= BACKOUT_SCAN_COOLDOWN_MS;
    }

    static void MarkBackoutScan(uint32 guidLow, uint32 nowMs)
    {
        std::lock_guard<std::mutex> lock(g_backoutScanMutex);
        g_lastBackoutScanMs[guidLow] = nowMs;
    }

    // Ranged DPS in a tight corner / tiny room oscillate: step in to regain LoS to the
    // target, the fire-band backs them out, LoS drops, repeat — pacing forever. Track
    // how long a bot has been unable to settle WITH LoS; past the give-up window it
    // stands its ground instead of pacing. Cleared the instant LoS returns.
    static constexpr uint32 LOS_SEEK_GIVEUP_MS = 2000;   // no LoS spot this long -> hold
    static std::unordered_map<uint32, uint32> g_losSeekStartMs;  // guidLow -> ms first seeking
    static std::mutex g_losSeekMutex;

    // Elapsed ms since this bot started seeking LoS (0 on the first call; starts the clock).
    static uint32 LosSeekElapsed(uint32 guidLow, uint32 nowMs)
    {
        std::lock_guard<std::mutex> lock(g_losSeekMutex);
        auto it = g_losSeekStartMs.find(guidLow);
        if (it == g_losSeekStartMs.end()) { g_losSeekStartMs[guidLow] = nowMs; return 0; }
        return nowMs - it->second;
    }
    static void ClearLosSeek(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(g_losSeekMutex);
        g_losSeekStartMs.erase(guidLow);
    }

    // When a ranged bot GIVES UP finding a clean firing spot (tiny room / tight
    // corner — e.g. the SM Cathedral hidden-boss room, where Mynya + Egoreno stood
    // blind in the doorway and dealt ZERO damage), it must stop trying to reach ideal
    // range and instead fight from wherever it has LoS, even point-blank. "Stand down"
    // means PAUSE the get-to-range / back-out routine — NOT freeze out of LoS. While
    // suppressed the bot still closes INTO the target's LoS and fights normally; only
    // the retreat-to-firing-range bands are held off, so it doesn't immediately walk
    // back out of LoS. Auto-expires; re-arms on the next give-up if the room's still
    // too tight (Kevin, 2026-06-24).
    static constexpr uint32 RANGE_ROUTINE_SUPPRESS_MS = 15000;
    static std::unordered_map<uint32, uint32> g_rangeSuppressUntilMs;  // guidLow -> expiry ms
    static std::mutex g_rangeSuppressMutex;

    static void SuppressRangeRoutine(uint32 guidLow, uint32 nowMs)
    {
        std::lock_guard<std::mutex> lock(g_rangeSuppressMutex);
        g_rangeSuppressUntilMs[guidLow] = nowMs + RANGE_ROUTINE_SUPPRESS_MS;
    }
    static bool RangeRoutineSuppressed(uint32 guidLow, uint32 nowMs)
    {
        std::lock_guard<std::mutex> lock(g_rangeSuppressMutex);
        auto it = g_rangeSuppressUntilMs.find(guidLow);
        return it != g_rangeSuppressUntilMs.end() && nowMs < it->second;
    }

    // Threat-cap throttle (replaces a fixed timer): even once the tank HAS the
    // mob, a DPS keeps DPSing only while its own threat stays below this fraction
    // of the tank's threat ON THAT MOB. The instant it climbs past, it drops the
    // target (auto-attack AND rotation idle — the rotation casts on GetVictim())
    // so the tank can rebuild a lead, then resumes. 0.80 sits well under WoW's
    // 110%/130% pull thresholds, so the bots never rip it off (Kevin: "base it on
    // the actual threat, not a timer"). Auto-scales to any tank/DPS/gear.
    static constexpr float THREAT_CAP_RATIO = 0.80f;

    // ---- Human-tank ENGAGE LEAD (pure threat, NO timer) ----------------------
    // A mob only counts as "properly engaged" by the tank once the tank holds at
    // least this fraction of the mob's MAX HEALTH in threat on it. Below it, DPS
    // hold and the healer holds heals — so a bare right-click, a passive patrol
    // that wandered onto the tank, or any other near-zero "light" aggro never
    // releases the party (Kevin: "they attack before i even did anything", "an
    // enemy patrol sneaks up"). Above it the existing THREAT_CAP_RATIO governs.
    // A fraction of max-HP AUTO-SCALES with level/content with no timer, and it's
    // PER MOB, so pulling one extra mob onto an already-locked pack only gates DPS
    // on that ONE fresh mob, never the whole party (the timer's fatal flaw).
    // Threat ≈ damage dealt (×modifiers), and damage-to-kill ≈ HP, so this fraction
    // is really "how many tank GCDs of lead": 0.03 was ~ONE paladin swing (RF
    // doubles threat) so the bots piled on a single tag; 0.15 was too sluggish.
    // 0.07 gives the tank a few actions' head start before anyone assists. Bosses
    // are exempt (IsBossUnit). Tune here if a pull feels too eager (raise) or too
    // sluggish (lower).
    static constexpr float ENGAGE_THREAT_HEALTH_FRAC = 0.07f;
    // Emergency release: a tank/member at or below this HP gets DPS + heals NOW,
    // engage-lead or not — nobody dies waiting for threat.
    static constexpr float TANK_GATHER_LOW_PCT = 55.0f;

    // While HOLDING for the tank to build threat, a bot just LOOSELY LEASHES near the
    // tank — NOT a formation spot (Kevin: "they don't need to stay in a certain spot
    // relative to me, just stay within a distance"). It only moves to catch up when it
    // drifts past HOLD_LEASH; within that it stands where it is.
    static constexpr float HOLD_LEASH = 15.0f;

    // True if a party member that's ALSO standing off (>8y from the shared
    // victim, i.e. another caster/hunter rather than a melee at the mob) is
    // stacked within `radius` of `bot`.
    static bool IsBunchedAtStandoff(Player* bot, Unit* victim, float radius)
    {
        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(bot->GetGUID(), party);
        for (ObjectGuid g : party)
        {
            if (g == bot->GetGUID()) continue;
            Player* m = ObjectAccessor::FindPlayer(g);
            if (!m || !m->IsAlive() || m->FindMap() != bot->FindMap()) continue;
            if (bot->GetDistance(m) > radius) continue;
            if (victim && m->GetDistance(victim) <= 8.0f) continue;  // a melee at the mob, not a stacked caster
            return true;
        }
        return false;
    }

    // A reachable, still-in-LoS point a few yards into the open from where the
    // caster currently stands, fanned laterally by its formation index so the
    // party spreads around the chokepoint instead of stacking on it. Returns
    // false (caller just holds at the corner) if no such point is safe — better
    // to bunch than to walk into a wall or break the LoS we just earned.
    static bool ComputeRangedSpreadSpot(Player* bot, Unit* victim, float hold,
                                        float& outX, float& outY, float& outZ)
    {
        int const fi = FormationIndexFor(bot->GetGUID(), GetLeaderFor(bot->GetGUID()));
        // Fan around the bot's current bearing to the victim (~12 deg/step,
        // centred near index 2 so the party splays both ways).
        float const angle = victim->GetAngle(bot) + (float(fi) - 2.0f) * 0.21f;
        // Pull a few yards INTO the open (toward the victim, where LoS is freer)
        // but stay a sane firing distance out. Clamp into [10, hold].
        float dist = bot->GetDistance(victim) - 3.0f;
        if (dist > hold)  dist = hold;
        if (dist < 10.0f) dist = 10.0f;

        float x, y, z;
        victim->GetNearPoint(bot, x, y, z, 0.0f, dist, angle);

        // Navmesh-reachable? (snaps to a valid coord; refuses to cross geometry.)
        if (!bot->GetMap()->CanReachPositionAndGetValidCoords(bot, x, y, z))
            return false;

        // Don't trade the corner LoS we just earned for a wall.
        float const cz = z + bot->GetCollisionHeight();
        float vx, vy, vz;
        victim->GetHitSpherePointFor({ x, y, cz }, vx, vy, vz);
        if (!bot->GetMap()->isInLineOfSight(x, y, cz, vx, vy, vz,
                bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2))
            return false;

        // Only worth a move if it's a meaningful step (avoid micro-jitter).
        if (bot->GetExactDist2d(x, y) < 2.0f)
            return false;

        outX = x; outY = y; outZ = z;
        return true;
    }

    // Nearest hostile (within `range`) whose current victim is NOT `bot` —
    // i.e. an add that's loose on the casters/healer. Returns nullptr if every
    // nearby hostile is already on the bot (or there are none). Built from the
    // party's attacker lists so it needs no grid search.
    static Unit* PickLooseTarget(Player* bot)
    {
        // Cap the grab to adds NEAR the party. AssistTarget melee-chases the
        // picked add, and that chase is uncapped — at 30y the tank sprinted clear
        // across the room to a far add and body-pulled every pack en route (the
        // bear-tank "chain-pulls until we die" report). 18y still grabs adds on
        // the healer/casters positioned behind the tank; FARTHER adds are pulled
        // with a ranged taunt rule (cast_loose_enemy:Growl) instead of a sprint.
        constexpr float LOOSE_MAX_RANGE = 18.0f;
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

    // "lowest" target mode: the LOWEST-current-health enemy the party is already
    // engaged with, within the bot's range — focus-fire to secure the next kill
    // instead of spreading damage. Like PickLooseTarget it only considers mobs in
    // combat (attacking, or being attacked by, the bot / its pet / a group member
    // or their pets), so it never pulls an idle mob; it just ranks them by health
    // remaining rather than distance. Returns nullptr if nothing qualifies.
    static Unit* PickLowestHealthTarget(Player* bot)
    {
        constexpr float MAX_RANGE = 40.0f;
        Unit* best = nullptr;
        uint32 bestHp = 0;
        bool   found  = false;
        auto consider = [&](Unit* a)
        {
            if (!a || !a->IsAlive() || !a->IsInCombat()) return;
            if (!bot->IsValidAttackTarget(a)) return;
            if (bot->GetDistance(a) > MAX_RANGE) return;
            uint32 const hp = a->GetHealth();
            if (!found || hp < bestHp) { bestHp = hp; best = a; found = true; }
        };
        auto considerAround = [&](Unit* u)
        {
            if (!u) return;
            for (Unit* a : u->getAttackers()) consider(a);   // mobs attacking u
            if (Unit* v = u->GetVictim()) consider(v);       // and the mob u is attacking
        };
        considerAround(bot);
        for (Unit* ctrl : bot->m_Controlled) considerAround(ctrl);
        if (Group* g = bot->GetGroup())
        {
            for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
            {
                Player* m = itr->GetSource();
                if (!m || !m->IsInWorld() || m == bot || m->GetMapId() != bot->GetMapId())
                    continue;
                considerAround(m);
                for (Unit* ctrl : m->m_Controlled) considerAround(ctrl);
            }
        }
        return best;
    }

    // "highest" target mode: the HIGHEST-current-health enemy the party is already
    // engaged with, within range — the mirror of PickLowestHealthTarget. Useful to
    // focus the beefiest target (the one that'll take longest to die / hits hardest)
    // while incidental adds get cleaned up by splash. Same combat-only gating, so it
    // never pulls an idle mob.
    static Unit* PickHighestHealthTarget(Player* bot)
    {
        constexpr float MAX_RANGE = 40.0f;
        Unit* best = nullptr;
        uint32 bestHp = 0;
        bool   found  = false;
        auto consider = [&](Unit* a)
        {
            if (!a || !a->IsAlive() || !a->IsInCombat()) return;
            if (!bot->IsValidAttackTarget(a)) return;
            if (bot->GetDistance(a) > MAX_RANGE) return;
            uint32 const hp = a->GetHealth();
            if (!found || hp > bestHp) { bestHp = hp; best = a; found = true; }
        };
        auto considerAround = [&](Unit* u)
        {
            if (!u) return;
            for (Unit* a : u->getAttackers()) consider(a);   // mobs attacking u
            if (Unit* v = u->GetVictim()) consider(v);       // and the mob u is attacking
        };
        considerAround(bot);
        for (Unit* ctrl : bot->m_Controlled) considerAround(ctrl);
        if (Group* g = bot->GetGroup())
        {
            for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
            {
                Player* m = itr->GetSource();
                if (!m || !m->IsInWorld() || m == bot || m->GetMapId() != bot->GetMapId())
                    continue;
                considerAround(m);
                for (Unit* ctrl : m->m_Controlled) considerAround(ctrl);
            }
        }
        return best;
    }

    // "nearest" target mode: the NEAREST enemy the party is ALREADY engaged with,
    // within the bot's range. Like PickLowestHealthTarget it only considers mobs
    // in combat (attacking, or attacked by, the bot / its pet / a group member or
    // their pets), so it NEVER walks the bot out to an idle, out-of-combat mob —
    // the old SelectNearbyTarget grid search did exactly that, marching bots to
    // their leash to poke a neutral monster. It just ranks the live combatants by
    // distance. Returns nullptr if nothing qualifies (assist loop then falls back
    // to party-defense).
    static Unit* PickNearestEngagedTarget(Player* bot)
    {
        constexpr float MAX_RANGE = 40.0f;
        Unit* best = nullptr;
        float bestDist = 1e9f;
        auto consider = [&](Unit* a)
        {
            if (!a || !a->IsAlive() || !a->IsInCombat()) return;
            if (!bot->IsValidAttackTarget(a)) return;
            float const d = bot->GetDistance(a);
            if (d > MAX_RANGE) return;
            if (d < bestDist) { bestDist = d; best = a; }
        };
        auto considerAround = [&](Unit* u)
        {
            if (!u) return;
            for (Unit* a : u->getAttackers()) consider(a);   // mobs attacking u
            if (Unit* v = u->GetVictim()) consider(v);       // and the mob u is attacking
        };
        considerAround(bot);
        for (Unit* ctrl : bot->m_Controlled) considerAround(ctrl);
        if (Group* g = bot->GetGroup())
        {
            for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
            {
                Player* m = itr->GetSource();
                if (!m || !m->IsInWorld() || m == bot || m->GetMapId() != bot->GetMapId())
                    continue;
                considerAround(m);
                for (Unit* ctrl : m->m_Controlled) considerAround(ctrl);
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

    // ===== Maintain-N pull (pull_count) =====================================
    // At the START of a pull the lead tank tops its engaged headcount up toward N by
    // WALKING to nearby un-aggroed mobs to proximity-aggro them (their neighbours
    // social-aggro in on their own — so we never body-pull a "cluster", we just walk
    // close). A candidate is added only if its social group fits the remaining
    // capacity, so the tank never overshoots N. DAZED/rooted/snared -> it taunts the
    // add in instead (TankRangedPullSpell — Heroic Throw / Avenger's Shield / Icy
    // Touch / Faerie Fire, never gun/bow); the FINAL add that hits N is also taunted
    // rather than walked. Nothing safe left to add, the tank's hurt, or it's stuck ->
    // it stops and fights what it has. Only ever armed on a fresh out-of-combat pull,
    // so it can't chain-pull the instance — the party rests between fights as before.
    static constexpr float  GATHER_SCAN         = 90.0f;   // candidate search radius (LoS-gated). 90y so the
                                                           // maintain-N gather reaches the NEXT pack (logs showed
                                                           // un-aggroed mobs sitting 50-80y out, beyond the old 60y,
                                                           // so the tank just stalled — Mill's "won't fetch more").
    static constexpr float  SOCIAL_AGGRO_R      = 10.0f;   // a candidate drags in un-aggroed mobs within this of it
    static constexpr uint32 GATHER_DRIVE_MAX_MS = 18000;   // safety bail if a gather gets stuck pre-engage (raised
                                                           // with GATHER_SCAN: a 90y walk-in needs more than 12s)
    static constexpr float  GATHER_LOW_HP_PCT   = 45.0f;   // tank this hurt -> stop gathering, fight + get healed
    static constexpr uint32 GATHER_REACH_GRACE_MS  = 3000;  // after committing to an add, give the tank up to
                                                            // this long to REACH it before re-checking for more
                                                            // (Mill: pace each multi-pull; don't re-arm every
                                                            // tick). Re-checks early the moment the add is grabbed.
    static constexpr uint32 GATHER_NO_ADD_GRACE_MS = 2000;  // once nothing's pullable, hold the gather this long
                                                            // (a straggler may wander in) before concluding and
                                                            // resuming combat.

    // The maintain-N gather is the BODY-PULL phase: the lead tank MOVES only (no rotation,
    // no attack — TankIsBodyPulling suppresses TickRotation), grabbing up to N mobs by
    // walking into them (proximity aggro). It CONCLUDES — and combat resumes — when it has
    // N, nothing pullable is left (after a grace), or the tank's HP drops. curAdd/curAddMs
    // pace each pull; noAddSinceMs runs the conclude grace.
    struct TankGatherDrive
    {
        uint32     startMs;
        uint32     targetN;
        ObjectGuid curAdd;             // the add we're walking to (empty = none)
        uint32     curAddMs     = 0;   // when we committed to curAdd
        uint32     noAddSinceMs = 0;   // start of the current no-reachable-add streak (0 = none)
    };
    static std::unordered_map<uint32, TankGatherDrive> g_tankGatherDrive;   // tankLow -> active gather
    static std::mutex g_tankGatherDriveMutex;

    static void StartTankGather(uint32 tankLow, uint32 targetN)
    {
        std::lock_guard<std::mutex> lock(g_tankGatherDriveMutex);
        uint32 const now = getMSTime();
        // Opportunistic prune: a tank that left mid-gather (logout / left party / left
        // the dungeon) never reaches EndTankGather, so its entry would linger. Sweep
        // any entry far older than the drive cap so a re-formed/re-hired guid can't
        // inherit a phantom gather. (Mirrors MarkTankGathering's prune of g_tankGather.)
        for (auto it = g_tankGatherDrive.begin(); it != g_tankGatherDrive.end(); )
            it = (it->first != tankLow && now - it->second.startMs > GATHER_DRIVE_MAX_MS * 3)
                     ? g_tankGatherDrive.erase(it) : std::next(it);
        g_tankGatherDrive[tankLow] = { now, targetN };
    }
    bool TankGatherActive(uint32 tankLow)
    {
        std::lock_guard<std::mutex> lock(g_tankGatherDriveMutex);
        return g_tankGatherDrive.count(tankLow) != 0;
    }

    // True for the WHOLE body-pull/gather: the lead tank MOVES only and runs NO rotation
    // (TickRotation early-returns on this), gathering up to N by walking into mobs. It does
    // NOT resume at the first pack — only when the gather CONCLUDES (reached N, nothing
    // pullable, or HP-bail), at which point combat resumes, the tank builds threat, and the
    // DPS engage (Mill). Tied to TankGatherActive, which PERSISTS for the whole gather. The
    // gather is only ever armed for a multi-pull (pull_count >= 2), which is ALWAYS a body-
    // pull regardless of safe_pull (safe_pull's ranged opener is the single-pull path). BAILS
    // (rotation runs — fight back / defensive) when HP is low.
    bool TankIsBodyPulling(Player* bot)
    {
        if (!bot || !IsLeadTank(bot->GetGUID())) return false;
        if (bot->GetHealthPct() <= GATHER_LOW_HP_PCT) return false; // in danger -> fight / bail
        return TankGatherActive(bot->GetGUID().GetCounter());       // gather active -> move only
    }
    static bool GetTankGather(uint32 tankLow, uint32& targetN, uint32& startMs)
    {
        std::lock_guard<std::mutex> lock(g_tankGatherDriveMutex);
        auto it = g_tankGatherDrive.find(tankLow);
        if (it == g_tankGatherDrive.end()) return false;
        targetN = it->second.targetN; startMs = it->second.startMs;
        return true;
    }
    // Copy the whole drive entry (false if none). Caller works on the copy so it can call
    // FindNextSafeAdd / EndTankGather / DriveTankChase without holding the drive mutex.
    static bool GetTankGatherFull(uint32 tankLow, TankGatherDrive& out)
    {
        std::lock_guard<std::mutex> lock(g_tankGatherDriveMutex);
        auto it = g_tankGatherDrive.find(tankLow);
        if (it == g_tankGatherDrive.end()) return false;
        out = it->second;
        return true;
    }
    // Write back the gather's pacing progress (current add + its timers). No-op if the
    // drive ended in between (we never resurrect a concluded gather).
    static void UpdateTankGatherProgress(uint32 tankLow, ObjectGuid curAdd, uint32 curAddMs, uint32 noAddSinceMs)
    {
        std::lock_guard<std::mutex> lock(g_tankGatherDriveMutex);
        auto it = g_tankGatherDrive.find(tankLow);
        if (it == g_tankGatherDrive.end()) return;
        it->second.curAdd = curAdd; it->second.curAddMs = curAddMs; it->second.noAddSinceMs = noAddSinceMs;
    }
    void EndTankGather(uint32 tankLow)
    {
        { std::lock_guard<std::mutex> lock(g_tankGatherDriveMutex); g_tankGatherDrive.erase(tankLow); }
        // Collapse the DPS-hold GC backstop so it can't linger ~12s after the drive
        // ends: the tank now FIGHTS, so its threat lead governs the real release (per
        // IsTankGathering); this 2s floor just stops a stuck/no-lead tail from holding
        // DPS for the full GC. Separate lock scope -> never nested with the drive mutex.
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tankGather.find(tankLow);
        if (it != g_tankGather.end())
        {
            uint32 const soon = getMSTime() + 2000;
            if (it->second.untilMs > soon) it->second.untilMs = soon;
        }
    }

    // Can the tank walk an add down at (near) full speed? A daze/snare/root or hard CC
    // means it must taunt instead (or bail).
    static bool TankCanWalkFreely(Player* tank)
    {
        if (!tank) return false;
        if (tank->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED
                               | UNIT_STATE_FLEEING)) return false;
        if (tank->HasAuraType(SPELL_AURA_MOD_ROOT)) return false;
        return tank->GetSpeedRate(MOVE_RUN) >= 0.95f;   // not snared / dazed
    }

    // Can the tank still CAST a taunt right now? Hard CC / silence blocks it.
    static bool TankCanCastNow(Player* tank)
    {
        if (!tank) return false;
        if (tank->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING)) return false;
        if (tank->IsNonMeleeSpellCast(false, false, true)) return false;
        return !tank->HasUnitFlag(UNIT_FLAG_SILENCED);
    }

    static bool IsBossUnit(Unit* u);   // fwd decl (defined further down); used by the gather
    static bool NavReachable(Player* bot, float x, float y, float z, float straight);  // fwd decl (defined below)

    // Distinct live hostile creatures currently in combat with the tank.
    static uint32 CountEngagedHostiles(Player* tank)
    {
        if (!tank) return 0;
        uint32 n = 0;
        for (auto const& [refGuid, ref] : tank->GetCombatManager().GetPvECombatRefs())
        {
            Unit* const o = ref->GetOther(tank);
            if (o && o->IsAlive() && o->ToCreature()) ++n;
        }
        return n;
    }

    // Any NON-tank party member (DPS/healer/the human leader) currently in combat. During
    // a body-pull the whole party HOLDS (no casts, no auto-attack — IsPartyPullPending), and
    // the gather's own mobs aggro the TANK, not them — so a non-tank in combat means a
    // SEPARATE pack just aggroed the group (a stray proximity pull, a patrol). The gather
    // must ABORT so the tank stops walking off to its planned cluster and instead peels the
    // new threat off the party (Mill: "we accidentally pulled a different pack and the tank
    // ignored it — really hard fight").
    static bool AnyNonTankPartyMemberInCombat(Player* tank)
    {
        if (!tank) return false;
        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(tank->GetGUID(), party);
        for (ObjectGuid const& g : party)
        {
            if (g == tank->GetGUID()) continue;
            Player* m = ObjectAccessor::FindConnectedPlayer(g);
            if (!m || !m->IsInWorld() || !m->IsAlive()) continue;
            if (m->GetMapId() != tank->GetMapId()) continue;
            if (m->IsInCombat()) return true;
        }
        return false;
    }

    // Is the tank currently in combat with a boss? Used to suppress the maintain-N
    // gather during a boss fight (Mill: "do not multi-pull into bosses") — a boss is
    // tank-and-spank, never a cluster to grow.
    static bool TankFightingBoss(Player* tank)
    {
        if (!tank) return false;
        for (auto const& [refGuid, ref] : tank->GetCombatManager().GetPvECombatRefs())
        {
            Unit* const o = ref->GetOther(tank);
            if (o && o->IsAlive() && IsBossUnit(o)) return true;
        }
        return false;
    }

    // A creature so far below the tank that it's GRAY (gives no XP) won't auto-aggro when
    // the tank walks up — its aggro radius collapses to ~nothing. Scarlet Monastery
    // Graveyard's lvl-8 "tortured victim" packs are RED (hostile faction) but trivial, so
    // the body-pull marched into them and stalled forever (Mill). Skip them from any
    // AUTO-pull (the player can still hit them by hand). Tradeoff: a heavily over-levelled
    // party (e.g. an 80 in SM) finds all the trash gray too and won't auto-body-pull it —
    // acceptable, since trivial trash dies to incidental cleave without a formal pull.
    static bool WontAutoAggro(Player* tank, Unit* u)
    {
        if (!tank || !u || !u->ToCreature()) return false;
        return u->GetLevel() <= Acore::XP::GetGrayLevel(tank->GetLevel());
    }

    // A clean, un-aggroed pull candidate the tank can reach + actually see (not through a
    // wall). Bosses are never gathered (Mill: no multi-pull into bosses). PASSIVE / neutral
    // (yellow) mobs are excluded: a body-pull works by PROXIMITY aggro, but a passive mob
    // won't aggro when the tank walks up — the tank would just stand on it. Only genuinely
    // HOSTILE (red) mobs that will react to the tank can be body-pulled (Mill).
    static bool GatherEligible(Player* tank, Unit* u)
    {
        if (!u || !u->IsAlive() || u->IsInCombat()) return false;   // un-aggroed only
        if (u->IsTotem() || !u->ToCreature()) return false;
        if (IsBossUnit(u)) return false;                            // never gather a boss
        if (WontAutoAggro(tank, u)) return false;                  // trivial/gray -> won't aggro (SM GY victims)
        if (!tank->IsValidAttackTarget(u)) return false;
        if (!u->IsHostileTo(tank)) return false;                   // passive/neutral (yellow) -> can't body-pull
        if (std::fabs(u->GetPositionZ() - tank->GetPositionZ()) > PULL_Z_TOLERANCE) return false;
        if (!tank->IsWithinLOSInMap(u, VMAP::ModelIgnoreFlags::M2)) return false;
        // Must be NAVMESH-reachable — a mob in LoS across a gap/ledge the tank can't path to
        // would otherwise pin the gather forever (the tank body-pulls but never closes; live
        // log: OPEN+GATHER entry=4308 dist=27 -> END(drive-timeout) engaged=0/8). Reachability
        // is the last (priciest) check so it only runs for an otherwise-eligible candidate.
        float const dx = u->GetPositionX() - tank->GetPositionX();
        float const dy = u->GetPositionY() - tank->GetPositionY();
        return NavReachable(tank, u->GetPositionX(), u->GetPositionY(), u->GetPositionZ(),
                            std::sqrt(dx * dx + dy * dy));
    }

    // How many mobs `cand` would drag in: itself + un-aggroed neighbours within
    // SOCIAL_AGGRO_R (single hop, matching how social aggro fans out from the pulled
    // mob). `pool` is the eligible set.
    static uint32 SocialGroupSize(Unit* cand, std::vector<Unit*> const& pool)
    {
        uint32 n = 1;
        for (Unit* u : pool)
            if (u != cand && cand->GetDistance(u) <= SOCIAL_AGGRO_R)
                ++n;
        return n;
    }

    // Nearest eligible un-aggroed candidate within GATHER_SCAN whose social group fits
    // `capacity` (= N - engaged). Returns it + its group size in grpOut, or nullptr if
    // none fit. STRICT: an add is taken only if it won't push the engaged headcount over
    // N — the cap is absolute, never overshot (Mill: "the multi-pull cap is absolute, do
    // NOT overshoot it"). A pack bigger than the remaining room is simply left alone.
    static Unit* FindNextSafeAdd(Player* tank, uint32 capacity, uint32& grpOut)
    {
        grpOut = 0;
        if (!tank || capacity == 0) return nullptr;
        std::list<Unit*> nearby;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(tank, tank, GATHER_SCAN);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(tank, nearby, check);
        Cell::VisitObjects(tank, searcher, GATHER_SCAN);

        std::vector<Unit*> pool;
        for (Unit* u : nearby) if (GatherEligible(tank, u)) pool.push_back(u);
        std::sort(pool.begin(), pool.end(),
                  [&](Unit* a, Unit* b){ return tank->GetDistance(a) < tank->GetDistance(b); });
        for (Unit* cand : pool)
        {
            uint32 const g = SocialGroupSize(cand, pool);
            if (g <= capacity) { grpOut = g; return cand; }   // nearest that SAFELY fits
        }
        return nullptr;
    }

    // Cast the tank's fast ranged-pull / taunt (never gun/bow) at `target`. True if it
    // went out (known, off cooldown, in range, in LoS).
    static bool TryTankFastRangedPull(Player* tank, Unit* target)
    {
        if (!tank || !target || !TankCanCastNow(tank)) return false;
        uint32 const spellId = WowPsParty::TankRangedPullSpell(tank);
        if (!spellId || tank->HasSpellCooldown(spellId)) return false;
        SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId);
        if (!si) return false;
        if (tank->GetDistance(target) > si->GetMaxRange(false, tank)) return false;
        if (!tank->IsWithinLOSInMap(target, VMAP::ModelIgnoreFlags::M2)) return false;
        tank->CastSpell(target, spellId, false);
        return true;
    }

    // Drive a body-pull toward `add` by MovePoint, NOT MoveChase. Diagnostics proved a
    // MoveChase(add) on an OUT-OF-COMBAT player-bot installs a CHASE generator (mg=5) that
    // emits NO spline — the tank holds at distCur=27.9, movedSelf~0, moveFlags=0x0, never
    // closing (Mill/Amaenna "stands still looking at the mob", engaged 0/N -> drive-timeout).
    // MovePoint is the proven walk-to-coordinate primitive for these bots (recall, LoS-
    // recovery, scout all use it). The opener is stationary while un-aggroed, so one
    // MovePoint walks the whole way; on arrival proximity-aggro grabs the pack and the
    // gather re-picks the next add. Re-issuing every tick resets the spline (stutter-in-
    // place), so only (re)issue when the target add changed, the mob WANDERED off the last
    // destination, or our point-move generator was lost (rotation/combat took over + back).
    static void DriveTankChase(Player* tank, Unit* add)
    {
        static thread_local std::unordered_map<uint32, uint64> lastChase;                  // tankLow -> add raw guid
        static thread_local std::unordered_map<uint32, std::array<float, 3>> lastDest;     // tankLow -> issued dest
        uint32 const low = tank->GetGUID().GetCounter();
        uint64 const ag = add->GetGUID().GetRawValue();
        float const ax = add->GetPositionX(), ay = add->GetPositionY(), az = add->GetPositionZ();
        MovementGeneratorType const mg = tank->GetMotionMaster()->GetCurrentMovementGeneratorType();
        uint64& lc = lastChase[low];
        std::array<float, 3>& ld = lastDest[low];
        bool const targetChanged = lc != ag;
        bool const wandered = std::fabs(ld[0] - ax) + std::fabs(ld[1] - ay) > 3.0f;
        bool const lostGen = mg != POINT_MOTION_TYPE;
        if (!targetChanged && !wandered && !lostGen) return;   // already walking to it — don't reset the spline
        tank->SetFacingToObject(add);
        tank->GetMotionMaster()->MovePoint(0, ax, ay, az);
        lc = ag;
        ld = { ax, ay, az };
    }

    // One tick of an active maintain-N BODY-PULL gather. The tank MOVES only here (the
    // rotation is suppressed for the whole gather, see TankIsBodyPulling): it walks up to
    // N mobs in one at a time, by proximity, pacing each pull, and CONCLUDES (resume combat)
    // when it has N / nothing's pullable / HP-bails. Runs from TankLeadEngagement.
    static void TankGatherStep(Player* tank)
    {
        uint32 const tankLow = tank->GetGUID().GetCounter();
        TankGatherDrive d;
        if (!GetTankGatherFull(tankLow, d)) return;
        uint32 const now = getMSTime();

        // ---- CONCLUDE conditions (then the rotation resumes and the tank fights) -------
        uint32 const engaged = CountEngagedHostiles(tank);
        char const* end = nullptr;
        if      (now - d.startMs > GATHER_DRIVE_MAX_MS)     end = "drive-timeout";  // overall safety cap
        else if (tank->GetHealthPct() <= GATHER_LOW_HP_PCT) end = "hp-bail";        // in danger
        else if (AnyNonTankPartyMemberInCombat(tank))       end = "party-aggro";    // a DPS/healer pulled a DIFFERENT
                                                                                    // pack -> abort, resume normal AI so
                                                                                    // the tank peels/grabs it (Mill)
        else if (TankFightingBoss(tank))                    end = "boss";           // never gather into a boss
        else if (engaged >= d.targetN)                      end = "reached-N";      // reached N -> fight
        if (end)
        {
            LOG_INFO("module", "[WowPsParty TankGather] guid={} END({}) engaged={}/{} ageMs={} hp={:.0f}",
                     tankLow, end, engaged, d.targetN, now - d.startMs, tank->GetHealthPct());
            EndTankGather(tankLow); return;
        }

        // ---- pace each pull: keep walking the committed add in until it's grabbed (in
        //      combat / dead / in melee) or its reach-grace expires, BEFORE looking for the
        //      next one (Mill: "give it time to reach the target of every multi-pull before
        //      it re-arms the check"). ------------------------------------------------------
        if (d.curAdd)
        {
            Unit* const cur = ObjectAccessor::GetUnit(*tank, d.curAdd);
            bool const reached = !cur || !cur->IsAlive() || cur->IsInCombat()
                              || tank->IsWithinMeleeRange(cur);
            if (!reached && now - d.curAddMs < GATHER_REACH_GRACE_MS)
            {
                DriveTankChase(tank, cur);    // still walking it in — give it time, don't re-pick
                return;
            }
            UpdateTankGatherProgress(tankLow, ObjectGuid::Empty, 0, d.noAddSinceMs);  // reached / timed out -> re-pick
        }

        // ---- find the next safe add (STRICT cap: its social group must fit N - engaged) --
        uint32 grp = 0;
        Unit* const add = FindNextSafeAdd(tank, d.targetN - engaged, grp);
        if (add)
        {
            // Hold the DPS for the WHOLE gather: re-mark with the current pack + the incoming
            // add (IsTankGathering / TankGatherActive keep the party back until we conclude).
            std::vector<ObjectGuid> set;
            for (auto const& [rg, ref] : tank->GetCombatManager().GetPvECombatRefs())
                if (Unit* o = ref->GetOther(tank))
                    if (o->IsAlive() && o->ToCreature()) set.push_back(o->GetGUID());
            set.push_back(add->GetGUID());
            MarkTankGathering(tankLow, set);

            UpdateTankGatherProgress(tankLow, add->GetGUID(), now, 0);  // commit + clear the no-add clock
            DriveTankChase(tank, add);                                  // BODY-PULL: walk it in (no taunt)
            return;
        }

        // ---- nothing pullable right now: hold a short grace (a straggler may wander into
        //      range), then CONCLUDE and resume combat. ------------------------------------
        if (d.noAddSinceMs == 0) { UpdateTankGatherProgress(tankLow, ObjectGuid::Empty, 0, now); return; }
        if (now - d.noAddSinceMs <= GATHER_NO_ADD_GRACE_MS) return;     // still within grace — wait
        LOG_INFO("module", "[WowPsParty TankGather] guid={} CONCLUDE: engaged={}/{} no more pullable -> resume combat",
                 tankLow, engaged, d.targetN);
        EndTankGather(tankLow);
    }

    void TankLeadEngagement(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld()) return;
        if (!bot->GetSession()) return;
        if (bot->HasUnitFlag(UNIT_FLAG_POSSESSED)) return;
        if (bot->IsNonMeleeSpellCast(false, false, true)) return;
        if (IsFollowerHeld(bot->GetGUID())) return;
        // Mounted = transport fly-by: don't auto-pull (a mounted unit can't attack,
        // and engaging here would fight the follow ticker keeping it riding). Once
        // the party commits on foot, the mount guard dismounts the tank and it pulls.
        if (bot->IsMounted()) return;

        // Is this the lead tank? Role-based so a hired henchman tank counts.
        if (!IsLeadTank(bot->GetGUID())) return;
        if (!GetLeadInDungeon(bot->GetGUID().GetCounter())) return;  // leading disabled

        ObjectGuid const leaderGuid = GetLeaderFor(bot->GetGUID());
        if (!leaderGuid) return;
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader || !leader->IsInWorld()) return;
        if (leader->GetMapId() != bot->GetMapId()) return;
        if (!leader->GetMap() || !leader->GetMap()->IsDungeon()) return;

        // Active maintain-N gather: drive it (walk/taunt to the next safe add) BEFORE
        // the out-of-combat-only pull logic below — the tank is mid-gather (in combat),
        // so the settle/leash/victim guards would otherwise bail. Runs after
        // AssistTarget this tick, so its MoveChase wins.
        if (TankGatherActive(bot->GetGUID().GetCounter()))
        {
            TankGatherStep(bot);
            return;
        }

        // (No in-combat re-arm: the gather PERSISTS for the whole body-pull — it grabs up
        // to N, paces each pull, and only EndTankGathers when it has N / nothing's pullable /
        // the tank HP-bails. One armed gather covers the whole pull, then the rotation
        // resumes and the tank fights; the next pull arms a fresh gather once the party is
        // out of combat + rested. Mill: pull-to-N-once-then-fight, with downtime after.)

        // Don't auto-pull the instant a fight ends — hold off until the WHOLE party
        // has been out of combat for POST_COMBAT_PULL_DELAY, so it can loot / regroup
        // / start drinking before the next pack is yanked in. CRUCIAL: the settle is
        // measured from the last tick ANY member was in combat, NOT from the tank's
        // own combat edge. The tank usually kills its mob and drops combat seconds
        // before the dps finish, so a tank-only timer expired mid-fight and the tank
        // pulled the instant the last dps dropped combat — "it pulls before the
        // party's combat drops" (Kevin). A party-wide edge also absorbs the 1-2s lag
        // before AC clears a member's combat flag. Skip too while anyone's
        // drinking/eating (BotIsConsuming matches by aura TYPE, so it catches the
        // human's higher-rank water/food, not just the bots' rank-1 Drink/Food).
        {
            // thread_local: TankLeadEngagement runs from the map-update thread pool,
            // and a bot is always updated by the thread owning its map — race-free
            // without a lock (matches wasAlive/stuckTracker below).
            static thread_local std::unordered_map<uint32, uint32> lastPartyCombatMs;
            uint32 const now = getMSTime();
            uint32& lastCombat = lastPartyCombatMs[bot->GetGUID().GetCounter()];

            bool anyInCombat = bot->IsInCombat();
            bool anyConsuming = false;
            std::vector<ObjectGuid> party;
            GetPartyGuidsFor(bot->GetGUID(), party);
            for (ObjectGuid const& g : party)
            {
                Player* m = ObjectAccessor::FindConnectedPlayer(g);
                if (!m || !m->IsInWorld() || m->GetMapId() != bot->GetMapId()) continue;
                if (m->IsInCombat())          anyInCombat = true;
                if (WowPsParty::BotIsConsuming(m)) anyConsuming = true;
            }

            if (anyInCombat) { lastCombat = now; return; }   // reset the settle timer
            if (anyConsuming) return;                         // recovering — don't pull
            if (lastCombat != 0 && now - lastCombat < POST_COMBAT_PULL_DELAY_MS)
                return;                                       // settling after the party went quiet
        }

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

        // Find the nearest hostile TO THE TANK within 28y of IT — not the leader.
        // The lead tank walks at the FRONT, so the pack it should open on is around
        // the tank, not around the human DPS trailing behind (Kevin: "the centre
        // point of the multi-pull range should be the tank, not the leader"); a
        // leader-centred search missed the pack the tank was standing in and it just
        // single-pulled. The tank can't out-run the party: a 30y leader-leash above
        // (line ~1636) already blocks a pull while it's far ahead. 22y (was 40y, then
        // leader-40y, then 28y) keeps it from opening on a still-distant mob — 28y still
        // reached out far enough that it was "very hard to not clear every single mob"
        // (Kevin), so the opener radius was tightened. This is the INITIAL-pull scan only;
        // the during-multi-pull gather range (GATHER_SCAN, FindNextSafeAdd) is unchanged.
        // SelectNearbyTarget returns the nearest unit `this` considers a valid attack target.
        Unit* nearest = bot->SelectNearbyTarget(nullptr, 22.0f);
        if (!nearest || !nearest->IsAlive()) return;
        if (!bot->IsValidAttackTarget(nearest)) return;
        if (!nearest->IsHostileTo(bot)) return;   // never auto-engage a PASSIVE/neutral (yellow) mob (Mill)
        if (WontAutoAggro(bot, nearest)) return;  // trivial/gray (won't aggro) — don't open on it (SM GY victims)
        // SelectNearbyTarget returns the nearest valid target even if it's NAVMESH-UNREACHABLE
        // (a mob across a gap/ledge). Opening on it locks the tank onto a mob it can never
        // close on — the live "OPEN+GATHER entry=4308 dist=27 -> 18s timeout, engaged=0, no
        // body pull". If the nearest isn't reachable, switch to the nearest REACHABLE eligible
        // hostile (FindNextSafeAdd); if there's none, don't auto-pull this tick.
        {
            float const dx = nearest->GetPositionX() - bot->GetPositionX();
            float const dy = nearest->GetPositionY() - bot->GetPositionY();
            if (!NavReachable(bot, nearest->GetPositionX(), nearest->GetPositionY(),
                              nearest->GetPositionZ(), std::sqrt(dx * dx + dy * dy)))
            {
                uint32 grp = 0;
                Unit* const reachable = FindNextSafeAdd(bot, 8, grp);   // nearest reachable eligible hostile
                if (!reachable) return;
                nearest = reachable;
            }
        }

        // Pull pacing — don't yank the next out-of-combat pack until mana is
        // topped off, so the party never chain-pulls on fumes. A PALADIN tank
        // waits on its OWN mana (it's the mana-using tank and self-sustains);
        // every other tank waits on the PARTY HEALER's mana (a warrior/DK/bear's
        // own bar is rage/runic, so the healer is the real limiter on starting the
        // next fight). No healer in the party -> nothing to gate on, pull normally.
        // Only reached when the whole party is already out of combat (checked
        // above), so this gates exactly the proactive next-pull, never a defensive
        // in-combat engage. Mana refills out of combat, so the hold always clears.
        {
            Player* const manaUnit = (bot->getClass() == CLASS_PALADIN)
                                   ? bot : FindPartyHealer(bot->GetGUID());
            if (manaUnit)
            {
                uint32 const maxMana = manaUnit->GetMaxPower(POWER_MANA);
                if (maxMana > 0 &&
                    uint64(manaUnit->GetPower(POWER_MANA)) * 100 < uint64(maxMana) * PULL_READY_MANA_PCT)
                {
                    static thread_local std::unordered_map<uint32, uint32> manaLogMs;
                    uint32 const now = getMSTime();
                    uint32& ml = manaLogMs[bot->GetGUID().GetCounter()];
                    if (now - ml > 5000)
                    {
                        ml = now;
                        LOG_INFO("module",
                            "[WowPsParty TankLead] guid={} holding next pull — {} mana {}/{} (<{}%)",
                            bot->GetGUID().GetCounter(),
                            (bot->getClass() == CLASS_PALADIN) ? "own" : "healer",
                            manaUnit->GetPower(POWER_MANA), maxMana, PULL_READY_MANA_PCT);
                    }
                    return;
                }
            }
        }

        // Maintain-N BODY-PULL (pull_count >= 2): a TRUE body-pull — the tank WALKS into the
        // pack (DriveTankChase) and proximity-aggros it, running NO ability rotation and taking
        // NO ranged swing (TankIsBodyPulling -> TickRotation + AssistTarget skip). Crucially it
        // does NOT Attack() the opener: an auto-attack from range pulls that mob into combat at
        // distance, so IT walks to a stationary tank ("the tank did not walk to the mob ... the
        // mob was the one who walked to the tank", engaged stuck at 1/N -> drive-timeout — Mill,
        // Amaenna). The opener is registered as the committed add so the gather paces it in
        // before re-picking, then grabs up to N. Reachability is already guaranteed (the
        // NavReachable swap above) and re-checked per add by GatherEligible, so Attack()'s
        // unreachable probe isn't needed here. DPS + healer hold the whole gather
        // (IsTankGathering); on CONCLUDE the rotation resumes, the tank builds threat, DPS
        // engage. N==1 (or a BOSS — never multi-pull a boss) falls through to the single-pull
        // opener below, which DOES open with an auto-attack.
        uint32 const pullN = WowPsParty::BotInitialPullCount(bot->GetGUID());
        if (pullN >= 2 && !IsBossUnit(nearest))
        {
            uint32 const tankLow = bot->GetGUID().GetCounter();
            MarkTankGathering(tankLow, { nearest->GetGUID() });                    // DPS hold
            StartTankGather(tankLow, pullN);
            UpdateTankGatherProgress(tankLow, nearest->GetGUID(), getMSTime(), 0); // walk THIS one in first
            bot->SetFacingToObject(nearest);
            DriveTankChase(bot, nearest);                                          // BODY-PULL: close on it (deduped)
            LOG_INFO("module",
                "[WowPsParty TankLead] guid={} OPEN+GATHER target={} on entry={} dist={:.1f}",
                tankLow, pullN, nearest->GetEntry(), bot->GetDistance(nearest));
            return;
        }

        // Single-pull opener (N==1, or a boss). AUTO-ATTACK only — the ability ROTATION stays
        // suppressed for the pull (TankIsBodyPulling -> TickRotation + AssistTarget skip), so the
        // tank builds no early RANGED threat; white swings only land once it's in melee. Attack
        // ALSO detects an EVADING / unreachable mob (ok==false) so the tank BAILS instead of
        // standing forever on a mob it can't path to. Re-armed each tick, so it self-corrects.
        bool const ok = bot->Attack(nearest, true);
        if (!ok)
        {
            LOG_INFO("module",
                "[WowPsParty TankLead] guid={} CANT-ATTACK mob_guid={} entry={} dist={:.1f} "
                "(evading/immune/unreachable) — bailing, NOT holding the party",
                bot->GetGUID().GetCounter(), nearest->GetGUID().GetCounter(),
                nearest->GetEntry(), bot->GetDistance(nearest));
            return;
        }
        bot->SetFacingToObject(nearest);

        // Ranged pull: a tank that can open from range doesn't charge into the
        // pack. It closes only to ability range, then the rotation (Heroic Throw,
        // Avenger's Shield, ...) pulls one mob while AssistTarget holds it there
        // and backs it up (see IsTankPulling / the pull-hold), so the pack comes to
        // US in open space instead of us running head-first into a room full of it.
        // "Can range-pull" = a thrown/gun/bow weapon OR a class ranged-pull ability
        // — the latter is what stops a paladin (libram in the ranged slot, no
        // weapon) from barging in despite having Avenger's Shield. Melee-only tanks
        // keep the old behaviour of closing straight in.
        Item* const rangedW = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        bool const hasRangedWeapon = rangedW && rangedW->GetTemplate()->IsRangedWeapon();
        // Editor "safe pull" toggle OFF -> never range-pull; fall through to the
        // melee close-in below so the tank just barges straight into the pack.
        bool const canRangedPull = (hasRangedWeapon || WowPsParty::TankRangedPullSpell(bot) != 0)
                                && WowPsParty::GetSafePull(bot->GetGUID());
        float const dist = bot->GetDistance(nearest);
        if (canRangedPull && dist > 8.0f)
        {
            float const holdRange = WowPsParty::TankPullHoldRange(bot);
            if (dist > holdRange + 2.0f)
                bot->GetMotionMaster()->MoveChase(nearest, holdRange);   // close to pull range
            else
                bot->GetMotionMaster()->Clear();                        // in range — hold; the pull-hold backs us up
            MarkTankPulling(bot->GetGUID(), 8000);
        }
        else
        {
            // Barge (melee tank, or a tank with safe_pull OFF — e.g. a henchman now
            // that henchmen default to barge): there's no ranged-pull window, so the
            // party would otherwise rush in ahead of the tank with no threat down
            // (Kevin: "DPS instantly engage before the tank is even in melee range").
            // Mark a single-mob GATHER so DPS/healer hold (threat-based, via
            // IsPartyPullPending -> IsTankGathering) until the tank reaches the mob and
            // builds a real engage lead — the multi-pull hold, applied to one mob.
            MarkTankGathering(bot->GetGUID().GetCounter(), { nearest->GetGUID() });
            DriveTankChase(bot, nearest);                            // melee: close in, deduped
        }

        LOG_INFO("module", "[WowPsParty TankLead] guid={} {} mob_guid={} entry={} dist={:.1f} ok={}",
                 bot->GetGUID().GetCounter(), canRangedPull ? "RANGE-PULL" : "PULL",
                 nearest->GetGUID().GetCounter(), nearest->GetEntry(), dist, ok);
    }

    // ===== Manual "pull one more" (keybind) ================================
    //
    // A micro tool for chain-pulling in M+: the player presses a bind and the lead
    // tank runs to + body-pulls the SINGLE NEAREST out-of-combat mob, overriding
    // its normal follow/assist/rotation movement for a short window. Unlike the
    // automatic TankLeadEngagement (which only fires with the whole party out of
    // combat + rested), this is a deliberate manual override that works MID-FIGHT
    // — exactly the "grab the next pack now" control a Mythic+ chain-pull needs.

    // Defined later in this file (before AssistTarget).
    static bool NavReachable(Player* bot, float x, float y, float z, float straight);

    // How far the "pull more" BIND reaches. Deliberately huge so the player can
    // chain-pull across a whole dungeon by spamming the bind, dragging the tank (and
    // its current mob) toward the next pack a little at a time.
    static constexpr float PULL_MORE_BIND_RANGE = 200.0f;

    // Pull candidate for the "pull more" BIND ONLY. This is INTENTIONALLY far more
    // permissive than the rotation-editor auto-pull (GatherEligible / TankLeadEngagement,
    // which keep their tight 60y + LoS + 6y-Z rules — DO NOT route the auto-pull through
    // here): the bind searches a wide radius around the HUMAN, ignores line of sight (the
    // tank navmesh-paths to it even out of view), and uses a generous Z so multi-level
    // dungeons work. Returns the un-aggroed mob NEAREST THE HUMAN that the tank can
    // actually path to — the next pack the player is looking at — so repeated presses
    // drag the tank toward it.
    static Unit* FindPullExtraForBind(Player* tank, Player* leader, float range)
    {
        if (!tank || !leader) return nullptr;
        std::list<Unit*> nearby;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(leader, leader, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(leader, nearby, check);
        Cell::VisitObjects(leader, searcher, range);

        std::vector<Unit*> cands;
        for (Unit* u : nearby)
        {
            if (!u || !u->IsAlive() || u->IsInCombat()) continue;   // un-aggroed only (a NEW pack)
            if (!u->ToCreature() || u->IsTotem()) continue;          // real mobs, not totems/objects
            if (!tank->IsValidAttackTarget(u)) continue;             // hostile + attackable
            // NO LoS check (fetch packs out of sight), and a GENEROUS Z (vs the auto-pull's
            // 6y) so a ramp/upper platform doesn't exclude the next pack.
            if (std::fabs(u->GetPositionZ() - leader->GetPositionZ()) > 60.0f) continue;
            cands.push_back(u);
        }
        // Nearest THE HUMAN first (it's the next pack the player sees), and prefer one the
        // tank can navmesh-reach so a press isn't wasted on an unreachable mob.
        std::sort(cands.begin(), cands.end(),
                  [&](Unit* a, Unit* b){ return leader->GetDistance(a) < leader->GetDistance(b); });
        for (Unit* u : cands)
        {
            float const dx = u->GetPositionX() - tank->GetPositionX();
            float const dy = u->GetPositionY() - tank->GetPositionY();
            if (NavReachable(tank, u->GetPositionX(), u->GetPositionY(), u->GetPositionZ(),
                             std::sqrt(dx * dx + dy * dy)))
                return u;
        }
        return cands.empty() ? nullptr : cands.front();   // nothing cleanly reachable — try the nearest anyway
    }

    void PullNearestExtra(Player* leader, uint32 holdMs)
    {
        if (!leader || !leader->IsInWorld() || !leader->GetSession()) return;

        auto notify = [&](char const* msg)
        { ChatHandler(leader->GetSession()).PSendSysMessage("{}", msg); };   // fmt-style, not printf %s

        // Lead tank in the caller's party (lowest-guid tank-role follower).
        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(leader->GetGUID(), party);
        Player* tank = nullptr;
        for (ObjectGuid const& g : party)
            if (g != leader->GetGUID() && IsLeadTank(g))
            { tank = ObjectAccessor::FindPlayer(g); break; }

        if (!tank || !tank->IsAlive() || !tank->IsInWorld())
        { notify("|cff66ccff[WowPsParty]|r Pull more: no living bot tank in the party."); return; }
        if (tank->GetMapId() != leader->GetMapId())
        { notify("|cff66ccff[WowPsParty]|r Pull more: tank isn't with you."); return; }
        if (tank->HasUnitFlag(UNIT_FLAG_POSSESSED)) return;   // human is driving the tank

        // BIND-only finder: 200y around the HUMAN, ignores LoS, nearest-to-human, tank
        // navmesh-paths there. (The rotation-editor pull config is unaffected.)
        Unit* mob = FindPullExtraForBind(tank, leader, PULL_MORE_BIND_RANGE);
        if (!mob)
        {
            ClearTankPullMore(tank->GetGUID().GetCounter());
            notify("|cff66ccff[WowPsParty]|r Pull more: no out-of-combat mob within 200y.");
            return;
        }

        // Arm the window, then kick it THIS instant (don't wait for the next AI
        // tick): engage + run in, and hold the follow ticker / AssistTarget off so
        // they can't yank the tank back. The driver re-asserts both every tick.
        SetTankPullMore(tank->GetGUID().GetCounter(), mob->GetGUID(), getMSTime() + holdMs);
        tank->Attack(mob, true);
        tank->GetMotionMaster()->MoveChase(mob);
        HoldFollower(tank->GetGUID(), holdMs);
        LOG_INFO("module",
            "[WowPsParty PullMore] leader={} tank={} -> mob_guid={} entry={} dist={:.1f} hold={}ms",
            leader->GetGUID().GetCounter(), tank->GetGUID().GetCounter(),
            mob->GetGUID().GetCounter(), mob->GetEntry(), tank->GetDistance(mob), holdMs);
    }

    void TickTankPullMore(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld()) return;
        uint32 const low = bot->GetGUID().GetCounter();
        ObjectGuid targetGuid;
        if (!GetTankPullMore(low, targetGuid)) return;          // not armed / window lapsed

        // Only the lead tank is ever armed; bail (and disarm) if state ever drifts.
        if (!IsLeadTank(bot->GetGUID()))           { ClearTankPullMore(low); return; }
        if (bot->HasUnitFlag(UNIT_FLAG_POSSESSED)) { ClearTankPullMore(low); return; }

        Unit* target = ObjectAccessor::GetUnit(*bot, targetGuid);
        if (!target || !target->IsAlive() || !bot->IsValidAttackTarget(target))
        { ClearTankPullMore(low); return; }
        // Pull achieved — the mob is now in combat (it aggroed us / a swing landed).
        // Release so the normal combat AI folds the new add into the fight.
        if (target->IsInCombat()) { ClearTankPullMore(low); return; }

        // Drive: engage + run in. Attack(true) sets the victim so the tank auto-
        // melees the instant it's in range; MoveChase walks it there. This runs
        // LAST in the AI tick (after rotation + AssistTarget), so this MoveChase
        // wins for the tick; HoldFollower keeps the 1Hz follow re-asserter + the
        // AssistTarget combat-chase off it while it runs (re-armed every tick).
        if (bot->GetVictim() != target) bot->Attack(target, true);
        bot->GetMotionMaster()->MoveChase(target);
        HoldFollower(bot->GetGUID(), 1000);
    }

    // ===== Gathering (mining / herbalism / skinning) =======================
    //
    // A follower bot that the player trained in Mining, Herbalism or Skinning
    // will, while OUT OF COMBAT and travelling with the party, peel off to
    // harvest a nearby node OR skinnable corpse (within 30y) that's within its
    // skill, then resume following. Only the player's own alts gather —
    // henchmen are temporary combat companions and are skipped. There's no
    // toggle: training the profession IS the opt-in.

    // The leash and the scan range are deliberately EQUAL (Kevin: "make the gather
    // range the same as the leash range"): a node the bot can SEE is one it's
    // allowed to reach. The leash now only gates STARTING a new detour — once the
    // bot commits to a node it finishes it (commit-and-complete), so it never walks
    // halfway to a herb and then turns around. The only thing that interrupts a
    // committed gather is the 100y follow-teleport, which clears the node (so the
    // bot re-scans fresh at the leader rather than walking back out).
    static constexpr float GATHER_LEADER_LEASH = 75.0f; // only START a new gather within this of the leader
    static constexpr float GATHER_SCAN_RANGE   = GATHER_LEADER_LEASH; // node search radius == leash range
    static constexpr float GATHER_REACH        = 11.0f;  // interaction distance — long reach so bots harvest without walking on top of the node
    static constexpr float GATHER_ARRIVED_REACH = 18.0f; // harvest-from-here cap once the navmesh can't get any closer (veins up rocks/ledges)
    static constexpr uint32 GATHER_APPROACH_TIMEOUT_MS = 6000; // give up if stuck
    static constexpr uint32 GATHER_AVOID_MS = 30000;   // ignore an unreachable node
    static constexpr float GATHER_DUNGEON_ENEMY_CLEAR = 100.0f; // in a dungeon, don't gather a node with a live enemy this close

    // Per-bot gather state. Committing to one node stops the bot oscillating
    // between two equidistant nodes; the avoid slot remembers a node we gave up
    // reaching (wedged on geometry) so we don't immediately re-pick it and spin.
    struct GatherState
    {
        ObjectGuid node;            // node we're walking toward
        uint32     commitMs   = 0;  // when we committed (stuck timeout)
        ObjectGuid avoid;           // a node we abandoned as unreachable
        uint32     avoidUntil = 0;
        uint32     lastOffloadMs = 0; // last full-bags offload attempt (throttle)
    };
    static std::unordered_map<uint32, GatherState> g_gather;  // botLow -> state
    static std::mutex g_gatherMutex;

    // True while a bot is actively committed to walking to a gather node. The
    // follow systems (the 1 Hz re-asserter AND the 250 ms humanize tick) MUST
    // yield to this — TickGathering drives the bot to the node with its own
    // MovePoint, and a follow re-assert / free-stand / wander would yank it off
    // mid-approach so it never reaches the ore (Viv: "my miner just stands
    // there"). The 2500 ms HoldFollower the gather path sets is not enough on
    // its own: the humanize tick samples 4x as fast and would interrupt in any
    // brief gap between the bot's AI ticks, so gate on the committed node too.
    bool BotIsApproachingGatherNode(uint32 botLow)
    {
        std::lock_guard<std::mutex> lock(g_gatherMutex);
        auto it = g_gather.find(botLow);
        return it != g_gather.end() && it->second.node;
    }

    // Drop a bot's committed gather node. Called when the >100y follow teleport
    // yanks the bot back to the leader, so it doesn't immediately walk back out to
    // the now-distant node and re-trigger the teleport — it re-scans fresh instead.
    static void ClearGatherNode(uint32 botLow)
    {
        std::lock_guard<std::mutex> lock(g_gatherMutex);
        auto it = g_gather.find(botLow);
        if (it != g_gather.end()) { it->second.node = ObjectGuid::Empty; it->second.commitMs = 0; }
    }

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

    // In a DUNGEON, treat a node as ungatherable while any live enemy is within
    // GATHER_DUNGEON_ENEMY_CLEAR of it — otherwise the miner charges straight through
    // the pack guarding the vein and pulls the room (Kevin). Open world is unchanged:
    // a roaming mob near a node out there is fine to gather past. Checked per node so a
    // committed node also drops the instant a patrol wanders near it (IsHarvestableBy
    // re-validates every tick → re-scan, which finds nothing gatherable and follows).
    static bool NodeBlockedByDungeonEnemies(Player* bot, WorldObject* node)
    {
        Map* const m = bot->GetMap();
        if (!m || !m->IsDungeon()) return false;
        struct HostileNearCheck
        {
            HostileNearCheck(WorldObject const* c, Player const* v, float r)
                : center(c), viewer(v), range(r) {}
            bool operator()(Creature* u) const
            {
                return u && u->IsAlive() && !u->IsCritter() && !u->IsTotem()
                    && center->IsWithinDist(u, range) && viewer->IsValidAttackTarget(u);
            }
            WorldObject const* center; Player const* viewer; float range;
        };
        std::list<Creature*> crs;
        HostileNearCheck check(node, bot, GATHER_DUNGEON_ENEMY_CLEAR);
        Acore::CreatureListSearcher<HostileNearCheck> searcher(node, crs, check);
        Cell::VisitObjects(node, searcher, GATHER_DUNGEON_ENEMY_CLEAR);
        return !crs.empty();
    }

    // True if `bot` can gather `go` right now: spawned, ready (not mid-harvest),
    // a mining/herb node, bot has the profession AND enough skill, hasn't already
    // harvested this spawn, and (in a dungeon) no enemy is camped on it.
    static bool IsGatherableBy(Player* bot, GameObject* go)
    {
        if (!go || !go->isSpawned()) return false;
        if (go->getLootState() != GO_READY) return false;
        uint32 skillId = 0, req = 0;
        if (!NodeGatherSkill(go, skillId, req)) return false;
        if (!bot->HasSkill(skillId)) return false;
        if (uint32(bot->GetSkillValue(skillId)) < req) return false;
        if (go->IsInSkillupList(bot->GetGUID())) return false;
        if (NodeBlockedByDungeonEnemies(bot, go)) return false;
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

    static constexpr uint32 GATHER_OFFLOAD_THROTTLE_MS = 4000;

    // A gathering bot's own 16-slot backpack fills with mats and stops it
    // harvesting (TickGathering won't deplete a node it can't store). But the
    // party inventory is SHARED — which bot physically carries a stack is
    // invisible to the player — so slide gathered trade goods (ore / herbs /
    // cloth / leather / stone) over to a bot party-mate that has room and keep
    // gathering on the party's COLLECTIVE space. Only ITEM_CLASS_TRADE_GOODS
    // moves: never tools (the mining pick / skinning knife stay), gear, bags,
    // quest or soulbound items. Uses the same preserve-or-bounce cross-character
    // move the equip/move handlers use, so an item is never lost or duplicated.
    // Returns the number of slots freed on `bot`.
    static uint32 OffloadTradeGoodsToPeers(Player* bot)
    {
        std::vector<Player*> peers;
        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(bot->GetGUID(), party);
        for (ObjectGuid const& g : party)
        {
            if (g == bot->GetGUID()) continue;
            Player* p = ObjectAccessor::FindConnectedPlayer(g);
            // Bot party-mates only — never push mats into the human's own bags.
            if (p && p->IsInWorld() && sPlayerbotsMgr.GetPlayerbotAI(p) &&
                !p->HasUnitFlag(UNIT_FLAG_POSSESSED) && p->GetFreeInventorySpace() > 0)
                peers.push_back(p);
        }
        if (peers.empty()) return 0;

        std::vector<Item*> movable;
        auto consider = [&](Item* it)
        {
            if (!it || it->IsEquipped() || it->IsNotEmptyBag()) return;
            ItemTemplate const* t = it->GetTemplate();
            if (t && t->Class == ITEM_CLASS_TRADE_GOODS)
                movable.push_back(it);
        };
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = bot->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    consider(bag->GetItemByPos(j));

        uint32 freed = 0;
        size_t pi = 0;
        for (Item* it : movable)
        {
            while (pi < peers.size() && peers[pi]->GetFreeInventorySpace() == 0) ++pi;
            if (pi >= peers.size()) break;
            Player* peer = peers[pi];

            bot->MoveItemFromInventory(it->GetBagSlot(), it->GetSlot(), true);
            it->SetOwnerGUID(peer->GetGUID());
            it->FSetState(ITEM_CHANGED);
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            it->SaveToDB(tx);
            CharacterDatabase.CommitTransaction(tx);

            ItemPosCountVec dest;
            if (peer->CanStoreItem(NULL_BAG, NULL_SLOT, dest, it, false) == EQUIP_ERR_OK)
            {
                peer->MoveItemToInventory(dest, it, true);
                ++freed;
            }
            else
            {
                // Couldn't store (raced full) — give it straight back so it never strands.
                it->SetOwnerGUID(bot->GetGUID());
                it->FSetState(ITEM_CHANGED);
                ItemPosCountVec back;
                if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, back, it, false) == EQUIP_ERR_OK)
                    bot->MoveItemToInventory(back, it, true);
                CharacterDatabaseTransaction tx2 = CharacterDatabase.BeginTransaction();
                it->SaveToDB(tx2);
                CharacterDatabase.CommitTransaction(tx2);
            }
            if (freed >= 8) break;   // a handful of slots is plenty to keep gathering
        }
        return freed;
    }

    // ----- "Train me soon" reminders ---------------------------------------
    // As a gathering skill climbs into its final 5 points before the CURRENT
    // cap (e.g. 70..75 while the cap is 75), the bot calls out each new point in
    // party chat so Kevin remembers to train the next rank. Every value is
    // announced at most once — including the cap itself — so a bot parked at the
    // cap (nothing left to gain until the next rank is trained) never re-spams.
    static std::unordered_map<uint64, uint16> g_skillAnnounce;  // (guidLow<<16|skill) -> last announced value
    static std::mutex g_skillAnnounceMutex;

    static char const* GatherSkillName(uint32 skill)
    {
        switch (skill)
        {
            case SKILL_MINING:    return "Mining";
            case SKILL_HERBALISM: return "Herbalism";
            case SKILL_SKINNING:  return "Skinning";
            default:              return "Gathering";
        }
    }

    // Call right after a SUCCESSFUL UpdateGatherSkill (the skill actually went
    // up). Announces only the three gathering professions, only within the last
    // 5 points before the cap, and only once per value reached.
    static void AnnounceGatherSkillProgress(Player* bot, uint32 skill)
    {
        if (skill != SKILL_MINING && skill != SKILL_HERBALISM && skill != SKILL_SKINNING)
            return;

        int32 const value = bot->GetPureSkillValue(skill);
        int32 const cap   = bot->GetPureMaxSkillValue(skill);
        if (cap <= 0 || value < cap - 5) return;   // not yet in the final-5 band

        // One announcement per value: skip anything we've already reported for
        // this bot+skill. Skill is monotonic, so the cap fires exactly once.
        // Assumes SkillGain.Gathering = 1 (the realm default) — at a higher step
        // intermediate values are skipped, never duplicated, by this same guard.
        uint64 const key = (uint64(bot->GetGUID().GetCounter()) << 16) | uint16(skill);
        {
            std::lock_guard<std::mutex> lock(g_skillAnnounceMutex);
            uint16& last = g_skillAnnounce[key];
            if (value <= int32(last)) return;
            last = uint16(value);
        }

        Group* g = bot->GetGroup();
        if (!g) return;

        std::string const msg = (value >= cap)
            ? Acore::StringFormat("{} {}/{} — capped! Train my next rank so I can keep gathering.",
                                  GatherSkillName(skill), value, cap)
            : Acore::StringFormat("{} {}/{} — almost capped, train me soon!",
                                  GatherSkillName(skill), value, cap);

        WorldPacket data;
        ChatMsg const type = g->isRaidGroup() ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
        ChatHandler::BuildChatPacket(data, type, LANG_UNIVERSAL, bot, nullptr, msg.c_str());
        g->BroadcastPacket(&data, true, -1, bot->GetGUID());
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
                if (bot->UpdateGatherSkill(skillId, pure, req))
                    AnnounceGatherSkillProgress(bot, skillId);
        }

        if (uint32 const lootId = go->GetGOInfo()->GetLootId())
            bot->AutoStoreLoot(lootId, LootTemplates_Gameobject, true);

        // Deplete it so it despawns + respawns like a real harvested vein.
        go->SetLootState(GO_JUST_DEACTIVATED);

        LOG_INFO("module",
            "[WowPsParty Gather] {} harvested go entry={} skill={} req={}",
            bot->GetName(), go->GetEntry(), skillId, req);
    }

    // ----- Skinning (corpse harvesting) ------------------------------------
    // A bot trained in Skinning peels nearby skinnable corpses exactly like a
    // mining/herb node: walk over, harvest straight into bags, skill up. The
    // engine only sets UNIT_FLAG_SKINNABLE once a corpse's normal loot has been
    // removed (Creature::AllLootRemovedFromCorpse), so this naturally waits for
    // the kill to be looted first. A corpse's required skill is usually Skinning
    // (leather); a few special mobs want Herbalism/Mining/Engineering instead,
    // and IsSkinnableBy verifies the bot actually has whatever the corpse asks
    // for — so a skinner never harvests one it isn't qualified to.

    // Skill-up red-level for a harvested corpse, mirroring Spell::EffectSkinning.
    // (The can-I-skin gate uses a different, skill-based formula — see below.)
    static int32 CorpseSkinReq(int32 level)
    {
        return level < 10 ? 0 : level < 20 ? (level - 10) * 10 : level * 5;
    }

    static bool IsSkinnableBy(Player* bot, Creature* c)
    {
        if (!c || c->IsAlive()) return false;
        CreatureTemplate const* tmpl = c->GetCreatureTemplate();
        if (!tmpl || tmpl->SkinLootId == 0) return false;   // not a skinnable beast
        if (c->loot.loot_type == LOOT_SKINNING) return false;  // already skinned — never re-skin
        uint32 const skill = tmpl->GetRequiredLootSkill();
        if (!bot->HasSkill(skill)) return false;
        // Engine cast gate (Spell::CheckCast, skinning): the required value keys
        // off the bot's CURRENT skill, not the corpse-level bands above.
        int32 const skillValue = bot->GetSkillValue(skill);
        int32 const level      = c->GetLevel();
        int32 const reqValue   = (skillValue < 100) ? (level - 10) * 10 : level * 5;
        if (reqValue > skillValue) return false;
        // Ready if the engine already flagged it (regular loot was removed first, so the
        // quest item is already gone — safe to skin).
        if (c->HasUnitFlag(UNIT_FLAG_SKINNABLE)) return true;
        // Otherwise the force-skin path: a genuine party kill we can force ready at harvest
        // (our bots leave corpse loot unfinished, so the flag is usually never set — see
        // WowPsParty_ForceSkinReady, which force-CLEARS the leftover regular loot).
        if (!(c->GetLootRecipient() || c->GetLootRecipientGroup()))
            return false;
        // BUT never force-skin a corpse whose loot table can drop a QUEST item the
        // human leader still needs — force-clearing the loot would destroy that drop.
        // We DELIBERATELY do NOT also gate on UNIT_DYNFLAG_LOOTABLE: a QuestRequired
        // drop only enters loot for a player who HAS the quest, so the skinner bot (the
        // loot recipient, not on the quest) never sees it and the corpse carries NO
        // lootable flag whenever its regular loot happens to roll empty — and that is
        // exactly when Viv's Mistvale Giblets (a 40% QuestRequired drop off Elder Mistvale
        // Gorillas) got skinned out from under her. The static loot-table check is the
        // only signal that survives an empty regular roll. HaveQuestLootForPlayer ->
        // HasQuestForItem self-clears the instant the leader has collected enough of the
        // item (count met, before turn-in), so leather is only delayed while she still
        // genuinely needs the drop — then these corpses skin normally again.
        if (Player* leader = ObjectAccessor::FindConnectedPlayer(GetLeaderFor(bot->GetGUID())))
            if (LootTemplates_Creature.HaveQuestLootForPlayer(
                    c->GetCreatureTemplate()->lootid, leader))
                return false;
        return true;
    }

    static Creature* FindNearestSkinnable(Player* bot, float range, ObjectGuid avoid)
    {
        std::list<Creature*> crs;
        NearbySkinnableCheck check(bot, range);
        Acore::CreatureListSearcher<NearbySkinnableCheck> searcher(bot, crs, check);
        Cell::VisitObjects(bot, searcher, range);

        Creature* best = nullptr;
        float bestDist = range + 1.0f;
        for (Creature* c : crs)
        {
            if (avoid && c->GetGUID() == avoid) continue;
            if (!IsSkinnableBy(bot, c)) continue;
            float const d = bot->GetDistance(c);
            if (d < bestDist) { bestDist = d; best = c; }
        }
        return best;
    }

    // Harvest a skinnable corpse straight into the bot's bags. Mirrors
    // Spell::EffectSkinning (clear the flag, skill up with elite x2) but stores
    // the skin loot directly instead of opening a loot window — our party bots
    // hard-return from UpdateAI, so the default loot AI never runs for them.
    static void SkinCorpse(Player* bot, Creature* c)
    {
        // The engine usually never set UNIT_FLAG_SKINNABLE for us (bots leave
        // corpse loot unfinished), so finish the leftover loot + flag it exactly
        // like the human skinning-cast patch does. Bail if it isn't a real party
        // kill — IsSkinnableBy already vetted skill/level, this is the final gate.
        if (!c->HasUnitFlag(UNIT_FLAG_SKINNABLE) && !WowPsParty_ForceSkinReady(bot, c))
            return;

        uint32 const skill = c->GetCreatureTemplate()->GetRequiredLootSkill();
        int32 const reqValue = CorpseSkinReq(c->GetLevel());

        bot->SetFacingToObject(c);
        c->RemoveUnitFlag(UNIT_FLAG_SKINNABLE);

        if (uint32 const skinLootId = c->GetCreatureTemplate()->SkinLootId)
            bot->AutoStoreLoot(skinLootId, LootTemplates_Skinning, true);

        // Tag the corpse as skinned. Without this, Creature::AllLootRemovedFrom-
        // Corpse re-sets UNIT_FLAG_SKINNABLE (it only skips when loot_type is
        // LOOT_SKINNING) and the bot could skin it twice → leather dupe; the tag
        // also makes the corpse despawn next update like a real skinned one.
        // clear() resets loot_type to LOOT_NONE, so set the tag afterwards.
        c->loot.clear();
        c->loot.loot_type = LOOT_SKINNING;

        if (uint32 const pure = bot->GetPureSkillValue(skill))
            if (bot->UpdateGatherSkill(skill, pure, uint32(reqValue), c->isElite() ? 2 : 1))
                AnnounceGatherSkillProgress(bot, skill);

        LOG_INFO("module",
            "[WowPsParty Gather] {} skinned creature entry={} skill={} req={}",
            bot->GetName(), c->GetEntry(), skill, reqValue);
    }

    // ----- Unified harvest target (node OR corpse) -------------------------
    // The tick state machine commits to one target guid and walks to it; these
    // resolve / validate / harvest it regardless of whether it's a GameObject
    // node or a creature corpse, so mining/herb and skinning share one approach
    // loop (commit, stuck-timeout, avoid) instead of duplicating it.
    static WorldObject* ResolveHarvest(Player* bot, ObjectGuid guid)
    {
        if (!guid) return nullptr;
        if (WorldObject* go = ObjectAccessor::GetGameObject(*bot, guid)) return go;
        return ObjectAccessor::GetCreature(*bot, guid);
    }

    static bool IsHarvestableBy(Player* bot, WorldObject* obj)
    {
        if (!obj) return false;
        if (GameObject* go = obj->ToGameObject()) return IsGatherableBy(bot, go);
        if (Creature* c = obj->ToCreature())      return IsSkinnableBy(bot, c);
        return false;
    }

    static void HarvestTarget(Player* bot, WorldObject* obj)
    {
        if (GameObject* go = obj->ToGameObject()) { GatherNode(bot, go); return; }
        if (Creature* c = obj->ToCreature())      SkinCorpse(bot, c);
    }

    // Nearest harvestable of EITHER kind the bot is trained for, within range.
    static WorldObject* FindNearestHarvest(Player* bot, float range, ObjectGuid avoid,
                                           bool wantNodes, bool wantSkin)
    {
        WorldObject* best = nullptr;
        float bestDist = range + 1.0f;
        if (wantNodes)
            if (GameObject* go = FindNearestGatherNode(bot, range, avoid))
            { best = go; bestDist = bot->GetDistance(go); }
        if (wantSkin)
            if (Creature* c = FindNearestSkinnable(bot, range, avoid))
                if (bot->GetDistance(c) < bestDist) best = c;
        return best;
    }

    void TickGathering(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld() || !bot->GetSession()) return;
        if (bot->HasUnitFlag(UNIT_FLAG_POSSESSED)) return; // the controlled body
        // Henchmen never gather (by design) — exit SILENTLY before the diagnostic
        // gate below, or a gather-skilled rndbot henchman spams "skip: henchman"
        // every few seconds and drowns the combat-assist log.
        if (IsHenchman(bot->GetGUID())) return;

        // Skill gate FIRST — most bots have no gather profession, exit silently.
        // Past this point the bot CAN gather, so every other skip is logged
        // (throttled per reason) — that's how we diagnose "my miner won't mine".
        bool const canNode = bot->HasSkill(SKILL_MINING) || bot->HasSkill(SKILL_HERBALISM);
        bool const canSkin = bot->HasSkill(SKILL_SKINNING);
        if (!canNode && !canSkin) return;

        uint32 const gLow = bot->GetGUID().GetCounter();
        uint32 const now  = getMSTime();

        if (bot->IsInCombat())                            { GatherLog(gLow, "skip: in combat"); return; }
        if (bot->IsNonMeleeSpellCast(false, false, true)) { GatherLog(gLow, "skip: casting"); return; }
        if (IsTankLeading(bot->GetGUID()))                { GatherLog(gLow, "skip: leading the dungeon"); return; }

        // Nowhere to put the mats — don't harvest (AutoStoreLoot silently drops
        // items that don't fit, which would deplete the node for nothing) and
        // don't even approach. Resumes once a bag slot frees up.
        if (bot->GetFreeInventorySpace() == 0)
        {
            // Shared party inventory: try to slide gathered mats to a bot
            // party-mate with room so one full backpack doesn't stall the party.
            // Throttled, and only re-attempted if it actually frees space.
            bool throttled = false;
            {
                std::lock_guard<std::mutex> lock(g_gatherMutex);
                auto& st = g_gather[gLow];
                if (now - st.lastOffloadMs < GATHER_OFFLOAD_THROTTLE_MS) throttled = true;
                else st.lastOffloadMs = now;
            }
            if (throttled) { GatherLog(gLow, "skip: bags full"); return; }
            uint32 const freed = OffloadTradeGoodsToPeers(bot);
            if (freed == 0 || bot->GetFreeInventorySpace() == 0)
            { GatherLog(gLow, "skip: bags full (party also full)"); return; }
            GatherLog(gLow, "offloaded mats to a party-mate to keep gathering");
        }

        ObjectGuid const leaderGuid = GetLeaderFor(bot->GetGUID());
        if (!leaderGuid) { GatherLog(gLow, "skip: no leader directive"); return; }
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader || !leader->IsInWorld()) { GatherLog(gLow, "skip: leader not in world"); return; }
        if (leader->GetMapId() != bot->GetMapId()) { GatherLog(gLow, "skip: leader other map"); return; }

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

        // Commit-and-complete: only START a new gather while within leash of the
        // leader. If we've fallen behind with NO node committed yet, catch up
        // instead of peeling off — the follow leash reels us in. But a node we've
        // ALREADY committed to is pursued to completion regardless of the leash,
        // so the bot never walks halfway to a herb and then turns around. The only
        // interrupt for a committed gather is the >100y follow teleport, which
        // clears the node (ClearGatherNode) so there's no walk-back.
        if (!committed && bot->GetDistance(leader) > GATHER_LEADER_LEASH)
        {
            GatherLog(gLow, "skip: lagging leader, not starting a new gather — catching up");
            return;
        }

        WorldObject* target = ResolveHarvest(bot, committed);

        if (!IsHarvestableBy(bot, target))
        {
            // Lost/invalid committed target — pick the nearest valid node or
            // corpse, skipping any we recently gave up reaching.
            target = FindNearestHarvest(bot, GATHER_SCAN_RANGE,
                                        (now < avoidUntil) ? avoid : ObjectGuid::Empty,
                                        canNode, canSkin);
            std::lock_guard<std::mutex> lock(g_gatherMutex);
            auto& st = g_gather[gLow];
            st.node     = target ? target->GetGUID() : ObjectGuid::Empty;
            st.commitMs = target ? now : 0;
        }
        else if (commitMs && (now - commitMs) > GATHER_APPROACH_TIMEOUT_MS &&
                 !bot->IsWithinDistInMap(target, GATHER_REACH))
        {
            // Committed but can't reach it (wedged on geometry). Abandon it and
            // avoid re-picking it for a while; next tick re-scans for another.
            // DIAGNOSTIC (mining-hang report): name the node's skill + distance so
            // the log shows whether MINING nodes time out more than herbs.
            {
                uint32 dbgSkill = 0, dbgReq = 0;
                char const* dbgName = "corpse/other";
                if (GameObject* dgo = target->ToGameObject())
                    if (NodeGatherSkill(dgo, dbgSkill, dbgReq)) dbgName = GatherSkillName(dbgSkill);
                GatherLog(gLow, Acore::StringFormat(
                    "ABANDON {} node: unreachable after {}ms (dist={:.1f}) — re-scanning",
                    dbgName, GATHER_APPROACH_TIMEOUT_MS, bot->GetDistance(target)));
            }
            std::lock_guard<std::mutex> lock(g_gatherMutex);
            auto& st = g_gather[gLow];
            st.avoid      = target->GetGUID();
            st.avoidUntil = now + GATHER_AVOID_MS;
            st.node       = ObjectGuid::Empty;
            st.commitMs   = 0;
            return;
        }
        if (!target) return;

        // Re-read the commit time — the resolution block above may have just
        // (re)committed this target, so the local copy from the top of the tick can
        // be stale. Needed for the "arrived" grace below.
        {
            std::lock_guard<std::mutex> lock(g_gatherMutex);
            commitMs = g_gather[gLow].commitMs;
        }

        // Harvest when in normal reach, OR when the bot has ARRIVED as close as the
        // navmesh can get it (stopped, after a moment of approaching) and the node
        // is within a larger reach with line of sight. MINING veins commonly sit up
        // on rocks/ledges/walls the navmesh can't path onto, so the bot stops at the
        // base several yards short of GATHER_REACH; without this it just stood there
        // until the 6s approach-timeout abandoned it, then re-picked it 30s later —
        // the "miner stares at the vein for 30-60s, copper/herbs are fine" report
        // (herbs sit on flat ground, always within reach). LoS (M2, matching the
        // spell engine) stops harvesting through a wall.
        bool const inReach = bot->IsWithinDistInMap(target, GATHER_REACH);
        bool const arrivedShort =
            !inReach
            && !bot->isMoving()
            && commitMs && (now - commitMs) > 1500
            && bot->IsWithinDistInMap(target, GATHER_ARRIVED_REACH)
            && bot->IsWithinLOSInMap(target, VMAP::ModelIgnoreFlags::M2);
        if (inReach || arrivedShort)
        {
            if (arrivedShort)
                GatherLog(gLow, "harvest: arrived as close as navmesh allows (elevated node) — gathering from extended reach");
            HarvestTarget(bot, target);
            std::lock_guard<std::mutex> lock(g_gatherMutex);
            auto& st = g_gather[gLow];
            st.node = ObjectGuid::Empty;
            st.commitMs = 0;
        }
        else
        {
            // Walk to the target and keep the 1Hz follow re-asserter off us so
            // it doesn't yank us back to the leader mid-approach. MovePoint
            // paths around geometry (generatePath defaults true).
            HoldFollower(bot->GetGUID(), 2500);
            bot->SetFacingToObject(target);
            bot->GetMotionMaster()->MovePoint(0xA17,
                target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
        }
    }

    // ----- Henchman corpse looting -----------------------------------------
    // Hired henchmen never ran the default playerbot loot AI (managed bots
    // hard-return from UpdateAI), so party-killed corpses just sat there — "they
    // never touch loot at all." This walks an idle henchman to the nearest
    // corpse its OWN WoW group killed and loots it through the ENGINE'S real
    // loot path — exactly like the player right-clicking the corpse:
    //   * MONEY is handled entirely by the built-in HandleLootMoneyOpcode, which
    //     splits it across the normal WoW party. We do NO manual ModifyMoney —
    //     that is party-of-5-only behaviour and doing it here DUPLICATED the gold
    //     on top of WoW's native group share.
    //   * ITEMS go through StoreLootItem, which honours the group's loot rules
    //     (round-robin / threshold / rolls) and bag space just like a real loot.
    // HENCHMEN ONLY: enrolled alts and the human keep their own loot path; the
    // party-of-5 flow is untouched.

    static constexpr float HENCH_LOOT_REACH = 4.5f;   // inside INTERACTION_DISTANCE (5.5)
    static constexpr float HENCH_LOOT_STANDOFF = 2.0f; // walk to ~2y off the corpse, not on top of it
    static constexpr float HENCH_LOOT_STEPOFF  = 3.0f; // after the last corpse, nudge this far clear of it

    static Creature* FindNearestLootableCorpse(Player* bot, float range,
                                               ObjectGuid avoid, Group* grp)
    {
        std::list<Creature*> crs;
        NearbyLootableCorpseCheck check(bot, range, grp, bot->GetGUID());
        Acore::CreatureListSearcher<NearbyLootableCorpseCheck> searcher(bot, crs, check);
        Cell::VisitObjects(bot, searcher, range);

        Creature* best = nullptr;
        float bestDist = range + 1.0f;
        for (Creature* c : crs)
        {
            if (avoid && c->GetGUID() == avoid) continue;
            float const d = bot->GetDistance(c);
            if (d < bestDist) { bestDist = d; best = c; }
        }
        return best;
    }

    // Items a HENCHMAN can't USE — only a player can: dungeon KEYS (ITEM_CLASS_KEY)
    // open doors / gates / cages / event objects the bot can't click, plus a few oddly-
    // classed dungeon "use" keys (the Executioner's Key in Zul'Farrak is a CONSUMABLE,
    // class 0, so the class check alone misses it). A bot hoarding one soft-locked the
    // dungeon (Kevin: "bots pick up keys, can't use them"). These are round-robin loot
    // (not free-for-all), so just SKIPPING the bot's own-corpse key would STRAND it
    // (no one else may loot that corpse) — instead the bot loots it and hands it to the
    // human (GiveLootedItemToLeader). Add more oddly-classed ones by id; every class-13
    // key is already covered. (acore_world.item_template: class=13 OR name LIKE '%Key%'.)
    static bool ItemIsPlayerOnly(uint32 itemid)
    {
        switch (itemid)
        {
            case 8444:   // Executioner's Key — Zul'Farrak (frees the captured prisoners)
                return true;
            default: break;
        }
        ItemTemplate const* t = sObjectMgr->GetItemTemplate(itemid);
        return t && t->Class == ITEM_CLASS_KEY;
    }

    // Hand an item the henchman just looted to the HUMAN party leader (the hirer), so a
    // dungeon key the bot can't use reaches a player. Direct server-side transfer — no
    // trade window. If there's no connected leader or its bags are full, the bot simply
    // KEEPS the item (still not stranded: the player can trade/loot it off the bot).
    static void GiveLootedItemToLeader(Player* hench, uint32 itemid, uint32 count)
    {
        ObjectGuid const leaderGuid = GetLeaderFor(hench->GetGUID());
        Player* leader = leaderGuid ? ObjectAccessor::FindConnectedPlayer(leaderGuid) : nullptr;
        if (!leader || !leader->IsInWorld() || leader == hench) return;
        Item* held = hench->GetItemByEntry(itemid);
        if (!held) return;
        uint32 const move = std::min<uint32>(count, held->GetCount());
        ItemPosCountVec dest;
        if (leader->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemid, move) != EQUIP_ERR_OK)
            return;   // leader's bags full — leave it on the bot (player can trade for it)
        hench->DestroyItemCount(itemid, move, true);
        if (Item* given = leader->StoreNewItem(dest, itemid, true))
        {
            leader->SendNewItem(given, move, true, false);
            if (leader->GetSession())
            {
                ItemTemplate const* t = sObjectMgr->GetItemTemplate(itemid);
                ChatHandler(leader->GetSession()).PSendSysMessage(
                    "|cff66ccff[WowPsParty]|r %s handed you %s — a key for you to use.",
                    hench->GetName().c_str(), t ? t->Name1.c_str() : "an item");
            }
        }
    }

    // Loot a reached corpse via the engine, like a player right-click + auto-loot:
    // open it, let the built-in money handler split the gold across the party,
    // then store each item slot under the group's loot rules, then release.
    static void LootCorpseForHenchman(Player* hench, Creature* c)
    {
        ObjectGuid const guid = c->GetGUID();

        // Open the corpse. SendLoot sets our LootGUID + the group loot permission,
        // and bails internally if we're out of range or not in the recipient
        // group — in which case GetLootGUID stays unset and we stop.
        hench->SendLoot(guid, LOOT_CORPSE);
        if (hench->GetLootGUID() != guid)
            return;

        // Money: built-in group split (HandleLootMoneyOpcode reads our LootGUID).
        // Empty packet — the handler ignores its payload for money loot.
        if (c->loot.gold > 0)
        {
            WorldPacket money(CMSG_LOOT_MONEY, 0);
            hench->GetSession()->HandleLootMoneyOpcode(money);
        }

        // Items: OBEY group-loot round-robin. StoreLootItem does NOT enforce it
        // (round-robin is normally a client-side restriction — the client greys out
        // trash that isn't your turn), so a blind loop let the henchman grab the
        // under-threshold cloth/greys the roll had assigned to the PLAYER. Take ONLY
        // the under-threshold items of a corpse whose round-robin owner is THIS
        // henchman; leave the player's (and every other member's) assigned trash,
        // every roll-pending green (is_blocked), and FFA/quest items untouched. Gold
        // above is unaffected — it's split across the group no matter who loots it.
        if (c->loot.roundRobinPlayer == hench->GetGUID())
        {
            for (size_t i = 0; i < c->loot.items.size(); ++i)
            {
                LootItem const& li = c->loot.items[i];
                if (li.is_looted || li.freeforall || li.is_blocked || !li.is_underthreshold)
                    continue;
                // Capture before looting — StoreLootItem flips li.is_looted.
                uint32 const itemid = li.itemid;
                uint32 const count  = li.count;
                bool const playerOnly = ItemIsPlayerOnly(itemid);
                InventoryResult msg = EQUIP_ERR_OK;
                if (!hench->StoreLootItem(uint8(i), &c->loot, msg))
                    continue;
                // A dungeon KEY the bot can't use: loot it (so round-robin doesn't strand
                // it on the bot's corpse) then hand it straight to the human leader.
                if (playerOnly)
                    GiveLootedItemToLeader(hench, itemid, count);
            }
        }

        // Close the window: clears LootGUID + UNIT_FLAG_LOOTING and, if the corpse
        // is now empty, drops its UNIT_DYNFLAG_LOOTABLE so we stop detecting it.
        hench->GetSession()->DoLootRelease(guid);
    }

    void TickHenchmanLoot(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld() || !bot->GetSession()) return;
        if (bot->HasUnitFlag(UNIT_FLAG_POSSESSED)) return;   // the controlled body

        // HENCHMEN ONLY — the whole point of this feature. Enrolled alts and the
        // human keep their normal loot path. IsHenchman takes g_mutex, so call it
        // once and bail early for everyone else.
        if (!IsHenchman(bot->GetGUID())) return;

        // Must be in a WoW group to loot its kills (the native loot permission
        // and money split both key off the group).
        Group* grp = bot->GetGroup();
        if (!grp) return;

        uint32 const gLow = bot->GetGUID().GetCounter();
        uint32 const now  = getMSTime();

        // COMBAT ALWAYS BEATS LOOTING. Looting only the bot's OWN combat let a
        // henchman whose fight just ended amble off to a corpse while the PLAYER was
        // under attack ("they didn't help"). Check the WHOLE party (the bot, the
        // human leader, every member): if anyone's fighting, abandon any loot-walk
        // and yield so AssistTarget engages. We MUST clear the committed corpse and
        // release the follow-hold the walk set — AssistTarget returns early while
        // held, so otherwise the bot keeps walking to the corpse for ~2.5s.
        bool partyInCombat = bot->IsInCombat();
        if (!partyInCombat)
        {
            std::vector<ObjectGuid> party;
            GetPartyGuidsFor(bot->GetGUID(), party);
            for (ObjectGuid const& g : party)
            {
                Player* m = ObjectAccessor::FindConnectedPlayer(g);
                if (m && m->IsInWorld() && m->IsAlive()
                    && m->GetMapId() == bot->GetMapId() && m->IsInCombat())
                { partyInCombat = true; break; }
            }
        }
        if (partyInCombat)
        {
            bool hadNode = false;
            {
                std::lock_guard<std::mutex> lock(g_gatherMutex);
                auto& st = g_gather[gLow];
                hadNode = (bool)st.node;
                st.node = ObjectGuid::Empty;
                st.commitMs = 0;
            }
            if (hadNode) HoldFollower(bot->GetGUID(), 0);   // release loot-walk hold -> AssistTarget engages
            HenchLootLog(gLow, "skip: party in combat — engaging instead of looting");
            return;
        }
        if (bot->IsNonMeleeSpellCast(false, false, true)) { HenchLootLog(gLow, "skip: casting"); return; }
        if (IsTankLeading(bot->GetGUID()))                { HenchLootLog(gLow, "skip: leading the dungeon"); return; }

        // Leader is the leash anchor only (don't wander off to a far corpse) —
        // NOT involved in money, which the engine distributes natively.
        ObjectGuid const leaderGuid = GetLeaderFor(bot->GetGUID());
        if (!leaderGuid) { HenchLootLog(gLow, "skip: no leader directive"); return; }
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader || !leader->IsInWorld())          { HenchLootLog(gLow, "skip: leader not in world"); return; }
        if (leader->GetMapId() != bot->GetMapId())    { HenchLootLog(gLow, "skip: leader other map"); return; }

        // Reuse the gather approach-state slot: a henchman NEVER gathers (Tick
        // Gathering bails at the henchman gate before touching g_gather), so its
        // entry is free, and reusing it gives us the follow-ticker yield
        // (BotIsApproachingGatherNode) and the >100y teleport clear for free.
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

        // Commit-and-complete: only START toward a new corpse while within leash
        // of the leader, so the henchman never strays off to a far body. A corpse
        // it has ALREADY committed to is finished regardless of leash (the >100y
        // follow teleport, which clears the slot, is the only interrupt).
        if (!committed && bot->GetDistance(leader) > GATHER_LEADER_LEASH)
        { HenchLootLog(gLow, "skip: lagging leader, not starting — catching up"); return; }

        Creature* target = committed ? ObjectAccessor::GetCreature(*bot, committed) : nullptr;
        bool const valid = target && !target->IsAlive()
            && target->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE)
            && target->loot.loot_type != LOOT_SKINNING
            && target->GetLootRecipientGroup() == grp
            && HenchClaimable(target, bot->GetGUID(), grp);  // still something here FOR US?

        if (!valid)
        {
            // Lost / emptied committed corpse — pick the nearest valid one,
            // skipping any we recently gave up reaching / just looted.
            target = FindNearestLootableCorpse(bot, GATHER_SCAN_RANGE,
                (now < avoidUntil) ? avoid : ObjectGuid::Empty, grp);
            std::lock_guard<std::mutex> lock(g_gatherMutex);
            auto& st = g_gather[gLow];
            st.node     = target ? target->GetGUID() : ObjectGuid::Empty;
            st.commitMs = target ? now : 0;
        }
        else if (commitMs && (now - commitMs) > GATHER_APPROACH_TIMEOUT_MS &&
                 !bot->IsWithinDistInMap(target, HENCH_LOOT_REACH))
        {
            // Committed but wedged on geometry — abandon it and avoid re-picking
            // it for a while; next tick re-scans for another.
            std::lock_guard<std::mutex> lock(g_gatherMutex);
            auto& st = g_gather[gLow];
            st.avoid      = target->GetGUID();
            st.avoidUntil = now + GATHER_AVOID_MS;
            st.node       = ObjectGuid::Empty;
            st.commitMs   = 0;
            return;
        }
        if (!target) return;

        if (bot->IsWithinDistInMap(target, HENCH_LOOT_REACH))
        {
            LootCorpseForHenchman(bot, target);
            ObjectGuid const tg = target->GetGUID();
            float const cx = target->GetPositionX();
            float const cy = target->GetPositionY();
            float const cz = target->GetPositionZ();
            {
                std::lock_guard<std::mutex> lock(g_gatherMutex);
                auto& st = g_gather[gLow];
                st.node = ObjectGuid::Empty;
                st.commitMs = 0;
                // Park it: a partially-looted corpse keeps UNIT_DYNFLAG_LOOTABLE (its
                // remaining loot belongs to other members / pending rolls), so without
                // this we'd re-pick it every tick and stop following. We've taken our
                // share; a fully-emptied corpse already lost the flag and won't recur.
                st.avoid = tg; st.avoidUntil = now + GATHER_AVOID_MS;
            }
            // No more corpses queued → step a few yards CLEAR of this one at a random
            // angle so the henchman doesn't sit and drink ON the corpse and block the
            // player from looting it (Kevin). Party isn't in combat here (checked above),
            // so a brief reposition is safe; the follow ticker brings it back after.
            if (!FindNearestLootableCorpse(bot, GATHER_SCAN_RANGE, tg, grp))
            {
                float const ang = float((gLow * 2654435761u + now) % 6283u) / 1000.0f;  // ~0..2pi, varies per bot/time
                float ex = cx + std::cos(ang) * HENCH_LOOT_STEPOFF;
                float ey = cy + std::sin(ang) * HENCH_LOOT_STEPOFF;
                float ez = cz;
                bot->UpdateAllowedPositionZ(ex, ey, ez);
                HoldFollower(bot->GetGUID(), 1500);   // let the step finish before the follow ticker re-asserts
                bot->GetMotionMaster()->MovePoint(0xA18, ex, ey, ez);
            }
        }
        else
        {
            // Walk to a STANDOFF ~2y off the corpse, not on top of it (Kevin: bots
            // stood ON corpses, blocking the player from looting). The loot reach is
            // 4.5y so this still loots fine. Aim for a point HENCH_LOOT_STANDOFF out
            // from the corpse along the corpse->bot line (the side we're approaching
            // from). Keep the 1Hz follow re-asserter and the 250ms humanize tick off
            // us (HoldFollower + the committed-node yield) so neither yanks us back.
            HoldFollower(bot->GetGUID(), 2500);
            bot->SetFacingToObject(target);
            float const sang = target->GetAngle(bot);   // corpse -> bot (our approach side)
            float wx = target->GetPositionX() + std::cos(sang) * HENCH_LOOT_STANDOFF;
            float wy = target->GetPositionY() + std::sin(sang) * HENCH_LOOT_STANDOFF;
            float wz = target->GetPositionZ();
            bot->UpdateAllowedPositionZ(wx, wy, wz);
            bot->GetMotionMaster()->MovePoint(0xA17, wx, wy, wz);
        }
    }

    // Managed party bots (heroes + henchmen) have their default AI gated out, so
    // when the human queues the party for a battleground and the "Enter Battle"
    // invite arrives they would never click it and would be left behind in the
    // queue. Detect a pending BG invite on the bot's own AI tick and accept it
    // (port in) exactly like a player clicking Enter Battle — mirrors mod-
    // playerbots' BgInviteActiveTrigger + AcceptBgInvitationAction, but the port
    // carries the REAL bgTypeId of the invited queue (the playerbot action
    // hardcodes WSG). Random FILL bots are NOT party-of-5 bots, so they accept
    // via their own AI and never reach here. Out-of-combat only — the port opcode
    // refuses in combat.
    void TickAcceptBgInvite(Player* bot)
    {
        if (!bot || !bot->GetSession()) return;
        if (bot->InBattleground() || !bot->InBattlegroundQueue()) return;
        if (bot->IsInCombat()) return;

        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const queueTypeId = bot->GetBattlegroundQueueTypeId(i);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE) continue;

            BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
            GroupQueueInfo ginfo;
            if (!bgQueue.GetPlayerGroupInfoData(bot->GetGUID(), &ginfo)) continue;
            if (!ginfo.IsInvitedToBGInstanceGUID || !ginfo.RemoveInviteTime) continue;

            BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
            uint8 const arenaType = BattlegroundMgr::BGArenaType(queueTypeId);

            // CMSG_BATTLEFIELD_PORT: arenaType, unk2, bgTypeId(u32), unk(0x1F90), action=1.
            WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
            packet << uint8(arenaType) << uint8(0) << uint32(bgTypeId) << uint16(0x1F90) << uint8(1);
            bot->GetSession()->HandleBattleFieldPortOpcode(packet);

            LOG_INFO("module", "[WowPsParty BG] {} auto-accepted BG invite (bgType={})",
                     bot->GetName(), uint32(bgTypeId));
            return;   // one accept per tick
        }
    }

    // The party's main combat target — the lead TANK's victim, else any party
    // member's victim — for a bot that has nothing else to engage. Lets an idle /
    // too-far bot (party-defense is range-capped, the leader isn't attacking) GAP-
    // CLOSE to the fight and use its abilities instead of standing around out of
    // combat while the party fights. Returns nullptr when NOBODY is actually
    // fighting a valid target, so it's dormant out of combat.
    static Unit* PartyMainCombatTarget(Player* bot)
    {
        if (!bot) return nullptr;

        // Members = our directive roster PLUS the WoW group (a second human + their
        // bots), so a bot aids ANY groupmate's fight — its own party, the leader,
        // or another player on another account — not just its own account.
        std::vector<Player*> members;
        auto addMember = [&](Player* m) {
            if (m && m->IsInWorld() && m->IsAlive() && m->GetMapId() == bot->GetMapId()
                && std::find(members.begin(), members.end(), m) == members.end())
                members.push_back(m);
        };
        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(bot->GetGUID(), party);
        for (ObjectGuid const& g : party) addMember(ObjectAccessor::FindConnectedPlayer(g));
        if (Group* grp = bot->GetGroup())
            for (GroupReference* itr = grp->GetFirstMember(); itr; itr = itr->next())
                addMember(itr->GetSource());

        // Is `u` part of OUR fight? True if it's attacking, or simply in combat
        // with, any party member or their (non-totem) pet. IsInCombatWith is the
        // KEY: it catches a RANGED attacker (a dagger-throwing scout/smuggler on
        // the healer) that getAttackers() — melee attackers only — never lists,
        // which is why those mobs were invisible and the casters just stood there.
        auto engagedWithParty = [&](Unit* u) -> bool
        {
            if (!u) return false;
            Unit* const v = u->GetVictim();
            for (Player* m : members)
            {
                if (v == m || u->IsInCombatWith(m)) return true;
                for (Unit* ctrl : m->m_Controlled)
                    if (ctrl && !ctrl->IsTotem() && (v == ctrl || u->IsInCombatWith(ctrl)))
                        return true;
            }
            return false;
        };

        Unit*  tankTgt  = nullptr;   // the tank's victim — the party's main fight
        Unit*  nearest  = nullptr;   // nearest enemy engaged with the party
        float  bestDist = 1e9f;
        auto consider = [&](Unit* u, bool fromTank)
        {
            if (!u || !u->IsAlive() || !u->IsInCombat()) return;
            if (!bot->IsValidAttackTarget(u)) return;
            if (fromTank && !tankTgt) tankTgt = u;
            float const d = bot->GetDistance(u);
            if (d < bestDist) { bestDist = d; nearest = u; }
        };

        // EVERY member's victim — NO range cap, so an idle bot gap-closes to a
        // groupmate fighting OUT of range (the >50y leader leash still bounds how
        // far it strays). The tank's victim is flagged so the party focus-fires
        // the tank's kill when there is one.
        for (Player* m : members)
            consider(m->GetVictim(), RoleForGuid(m->GetGUID()) == "tank");

        // Grid search: every nearby enemy actually engaged with the party —
        // melee OR ranged. Bounded to 45y so we don't grab a fight across the
        // zone; the >50y party leash above caps the wander further.
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, 45.0f);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck>
            searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, 45.0f);
        for (Unit* u : units)
            if (engagedWithParty(u))
                consider(u, false);

        return tankTgt ? tankTgt : nearest;   // focus the tank's kill, else nearest threat
    }

    // The party's lead tank (alive, same map as `bot`), or null. Shared by the
    // pull-pending check and the follow ticker's range-pull anchor.
    static Player* PartyLeadTank(Player* bot)
    {
        if (!bot) return nullptr;
        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(bot->GetGUID(), party);
        for (ObjectGuid const& g : party)
        {
            if (!IsLeadTank(g)) continue;
            Player* p = ObjectAccessor::FindConnectedPlayer(g);
            if (p && p->IsInWorld() && p->IsAlive() && p->GetMapId() == bot->GetMapId())
                return p;
        }
        return nullptr;
    }

    // A feral druid that BRIEFLY drops cat/bear form (cancel form → instant
    // Rejuvenation → re-shift) must keep MELEE positioning for that window — it is
    // NOT a caster just because it's momentarily humanoid. We can't read that off
    // the talent tree (leveling / henchman ferals often don't register feral as
    // the primary tree), so track when the bot was last actually IN a feral form;
    // a resto / balance druid never enters one, so this never false-positives.
    static std::mutex g_feralFormMutex;
    static std::unordered_map<uint32, uint32> g_lastFeralFormMs;   // guidLow -> getMSTime
    static constexpr uint32 FERAL_FORM_GRACE_MS = 5000;

    static void MarkFeralForm(Player* bot)
    {
        // DRUID only: IsInFeralForm() is also true for a shaman's Ghost Wolf, which
        // every shaman spec (incl. resto) uses to reposition — stamping that would
        // wrongly hold a ranged shaman in melee for the grace window. Enhancement
        // shamans are already melee via the PrimaryTalentTree==1 check.
        if (!bot || bot->getClass() != CLASS_DRUID || !bot->IsInFeralForm()) return;
        std::lock_guard<std::mutex> lock(g_feralFormMutex);
        g_lastFeralFormMs[bot->GetGUID().GetCounter()] = getMSTime();
    }

    static bool WasRecentlyFeral(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_feralFormMutex);
        auto it = g_lastFeralFormMs.find(guid.GetCounter());
        return it != g_lastFeralFormMs.end()
            && (getMSTime() - it->second) < FERAL_FORM_GRACE_MS;
    }

    // True if this bot fights in MELEE (anchors closer to the tank during a pull,
    // ready to engage). Mirrors AssistTarget's ranged/melee split: tanks and the
    // physical classes are melee; healers and the caster classes are ranged,
    // EXCEPT an enhancement shaman / feral druid (talent tree 1) or one in — or
    // just out of (grace window) — a feral form, which melee.
    static bool FollowerIsMelee(Player* bot)
    {
        if (!bot) return false;
        std::string const role = RoleForGuid(bot->GetGUID());
        if (role == "tank")   return true;
        if (role == "healer") return false;
        uint8 const acls = bot->getClass();
        bool const ranged =
            acls == CLASS_MAGE   || acls == CLASS_WARLOCK || acls == CLASS_PRIEST ||
            acls == CLASS_HUNTER || acls == CLASS_SHAMAN  || acls == CLASS_DRUID;
        if (!ranged) return true;   // warrior / rogue / death knight
        if ((acls == CLASS_DRUID || acls == CLASS_SHAMAN)
            && WowPsParty::PrimaryTalentTree(bot) == 1) return true;
        if (bot->IsInFeralForm() || WasRecentlyFeral(bot->GetGUID())) return true;
        return false;
    }

    // Does the party LEADER fight from range (caster / healer / ranged dps)? The
    // leader has NO follower directive, so FollowerIsMelee's RoleForGuid lookup is
    // blind to it (a Holy Paladin / Disc Priest healer would be misread by class
    // alone). Read the leader's configured party role from LeaderRole — the same
    // cache HumanTankLeadActive uses — for the tank/healer cases, and fall back to
    // the class + feral-form heuristic only for a "dps" / unset role.
    static bool LeaderFightsAtRange(Player* leader)
    {
        if (!leader) return false;
        if (leader->GetSession())
        {
            std::string const lr = LeaderRole(leader->GetSession()->GetAccountId());
            if (lr == "tank")   return false;   // melee front-liner
            if (lr == "healer") return true;    // heals/stands at range regardless of class
        }
        return !FollowerIsMelee(leader);        // dps / unset -> class & feral-form heuristic
    }

    // True while the party's lead tank is mid ranged-pull and the pack hasn't
    // reached it yet — the signal every other bot reads to hold fire. False once
    // a hostile is in the tank's melee (pull complete → fight) or there's no
    // active tank pull. Dungeon/raid only; a manual leader pull never sets the
    // tank-pull window, so this stays false and normal assist applies.
    static bool IsPartyPullPending(Player* bot)
    {
        if (!bot || !bot->IsInWorld()) return false;
        if (!bot->GetMap() || !bot->GetMap()->IsDungeon()) return false;

        Player* tank = PartyLeadTank(bot);
        if (!tank || tank == bot) return false;

        // BODY-PULL gather active: HOLD the whole time the lead tank is gathering the pack
        // (it runs no rotation / builds no threat during this — see TankIsBodyPulling). The
        // DPS must not engage until the gather CONCLUDES and the tank starts building threat,
        // else they'd rip the not-yet-threatened pack off it. Once the gather ends the tank
        // fights and the threat gate (TankLeadActive / THREAT_CAP) governs the real release.
        if (TankGatherActive(tank->GetGUID().GetCounter())) return true;

        // (Legacy) multi-pull gather mark — held until the tank has a threat lead; self-
        // releases per mob. Kept for the non-gather mark path.
        if (IsTankGathering(tank)) return true;

        if (!IsTankPulling(tank->GetGUID())) return false;

        // Single-mob pull: engaged the instant a hostile reaches the tank's melee
        // (victim or any attacker) — that's "the tank has the pack", so the party is
        // free to go.
        if (Unit* tv = tank->GetVictim())
            if (tank->IsWithinMeleeRange(tv)) return false;
        for (Unit* a : tank->getAttackers())
            if (a && tank->IsWithinMeleeRange(a)) return false;
        return true;
    }

    // Rear-arc bearings for the range-pull tank anchor. MoveFollow's angle is
    // relative to the ANCHOR's facing (0 = in front, M_PI = directly behind), and
    // the tank faces the mob while winding up the pull — so M_PI is "straight back,
    // away from the pack", exactly the safe backpedal direction. These cluster
    // tightly around M_PI (a small spread just to stop bots stacking) and NEVER
    // reach the ±M_PI/2 flanks, so the party backs off BEHIND the tank, not to the
    // sides (the old FORM_ANGLES set included flank bearings — the "backed off to
    // the sides instead" bug).
    static constexpr float PULL_REAR_ANGLES[6] = {
        float(M_PI),            // straight behind
        float(M_PI) * 0.90f,    // behind, slightly left
        float(M_PI) * 1.10f,    // behind, slightly right
        float(M_PI) * 0.85f,    // behind-left
        float(M_PI) * 1.15f,    // behind-right
        float(M_PI) * 0.95f,    // behind, just off-centre (distinct so it can't
                                // stack on index 0; 7+ followers ring out by dist)
    };

    // Rear-arc follow angle (relative to the tank's facing) on the side the bot is
    // ALREADY on, so a ranged bot anchoring during a pull never swings across the
    // tank's FRONT to a fixed formation bearing — the "mage/hunter crosses to the
    // far side of the tank and proximity-pulls the next pack" bug (Kevin). Mirrors
    // PULL_REAR_ANGLES' rear cluster (never the ±M_PI/2 flanks) but reads the side
    // from live geometry; `fi` fans same-side bots a few degrees so they don't stack.
    static float RearArcAngleOnBotSide(Player* bot, Player* tank, int fi)
    {
        // GetRelativeAngle is [0,2pi) in the tank's facing frame: (0,pi)=left (CCW),
        // (pi,2pi)=right. Map to a bearing just inside "straight behind" on that side.
        float const rel = tank->GetRelativeAngle(bot->GetPositionX(), bot->GetPositionY());
        bool const leftSide = rel < float(M_PI);
        float const fan = 0.05f * float(fi % 3);            // 0 / 0.05pi / 0.10pi outward
        return leftSide ? float(M_PI) * (0.90f - fan)
                        : float(M_PI) * (1.10f + fan);
    }

    // True if standing at (x,y,z) would proximity-pull a FRESH pack — an un-aggroed,
    // out-of-combat hostile sits within `aggroR` of the spot. Anything already IN
    // combat is excluded (that's the pull we're part of, not a new pull). `center` is
    // the grid-visit anchor (the tank or the bot itself); the filter is by distance to
    // the candidate point. Only called when a ranged bot is (re)choosing a standoff
    // spot — pull anchor, in-combat back-out, spread step — never every tick, so the
    // grid search stays cheap.
    static bool SpotProximityPulls(Player* center, float x, float y, float z, float aggroR)
    {
        if (!center) return false;
        float const ddx = x - center->GetPositionX();
        float const ddy = y - center->GetPositionY();
        float const scanR = std::sqrt(ddx * ddx + ddy * ddy) + aggroR + 2.0f;
        std::list<Unit*> nearby;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, center, scanR);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(center, nearby, check);
        Cell::VisitObjects(center, searcher, scanR);
        for (Unit* u : nearby)
        {
            if (!u || !u->IsAlive() || u->IsInCombat()) continue;   // fresh packs only
            if (u->IsTotem() || !u->ToCreature()) continue;
            float const dx = u->GetPositionX() - x;
            float const dy = u->GetPositionY() - y;
            float const dz = u->GetPositionZ() - z;
            if (dx * dx + dy * dy + dz * dz <= aggroR * aggroR) return true;
        }
        return false;
    }

    // Pick a firing-range retreat spot for a too-close ranged bot that WON'T
    // proximity-pull a fresh pack. Tries straight back from the mob first (the natural
    // kite line, preserving the bot's side); if that clips a fresh pack, retreats
    // TOWARD the leader instead (the party's safe zone). Returns false when neither is
    // safe — the caller should then hold and fight in place rather than back into the
    // next group (Kevin: "they should be careful during combat too").
    static bool SafeRangedBackout(Player* bot, Unit* mob, Player* leader,
                                  float dist, float& ox, float& oy, float& oz)
    {
        if (!bot || !mob) return false;
        // 1) straight back from the mob.
        mob->GetNearPoint(bot, ox, oy, oz, 0.0f, dist, mob->GetAngle(bot));
        if (!SpotProximityPulls(bot, ox, oy, oz, 10.0f)) return true;
        // 2) toward the leader (their safe standoff) — a point `dist` from the mob on
        //    the leader's bearing, taken only if reachable and itself clear.
        if (leader && leader != bot)
        {
            mob->GetNearPoint(bot, ox, oy, oz, 0.0f, dist, mob->GetAngle(leader));
            if (NavReachable(bot, ox, oy, oz, bot->GetExactDist(ox, oy, oz))
                && !SpotProximityPulls(bot, ox, oy, oz, 10.0f))
                return true;
        }
        return false;   // nowhere safe -> hold in place
    }

    // True when this party is running a dungeon led by a REAL-PLAYER tank — the
    // case where DPS bots must wait for the human to pull + hold aggro instead of
    // pre-pulling or piling onto whatever the tank merely auto-attacks/selects.
    // Scoped exactly to Mill's request: dungeons only, human (not bot) tank only.
    // A bot-tank party (the human leader is DPS) returns false here and keeps the
    // existing lead-tank pull coordination (IsTankPulling / IsPartyPullPending).
    static bool HumanTankLeadActive(Player* bot, Player* leader)
    {
        if (!bot || !leader || !leader->GetSession()) return false;
        if (!bot->GetMap() || !bot->GetMap()->IsDungeon()) return false;
        if (sPlayerbotsMgr.GetPlayerbotAI(leader)) return false;   // leader is a bot, not human
        // Leader's OWN role (it has no follower directive, so RoleForGuid is blind
        // to it) — read the cache captured in SetActiveFollowers.
        return LeaderRole(leader->GetSession()->GetAccountId()) == "tank";
    }

    // The threat-wait gate fires whenever a TANK leads the dungeon — the human leader
    // (HumanTankLeadActive) OR a bot/henchman lead tank while the human plays a DPS.
    // Mill: "the threat throttle isn't respected — when my tank is a henchman the DPS
    // just rip everything off it." The gate's machinery (MobOnTank / BotOverThreatVsTank /
    // TankHasEngageLead) is already tank-agnostic; only this entry condition was scoped
    // to a human tank. A non-tank bot in a bot-tank party now waits for the bot tank's
    // engage lead exactly as it would for a human tank.
    static bool TankLeadActive(Player* bot, Player* leader)
    {
        if (!bot || !bot->GetMap() || !bot->GetMap()->IsDungeon()) return false;
        if (HumanTankLeadActive(bot, leader)) return true;
        Player* const t = PartyLeadTank(bot);
        return t && t != bot && t->IsAlive();
    }

    // The party's tank as a Player*, whoever it is: a bot/henchman lead tank
    // (PartyLeadTank) or the human leader when its party role is tank (the leader
    // has no follower directive, so PartyLeadTank can't see it). nullptr if the
    // party has no live tank. Used to send a non-tank's loose add to the tank.
    static Player* PartyTankPlayer(Player* bot, Player* leader)
    {
        if (Player* t = PartyLeadTank(bot)) return t;
        if (leader && leader->IsAlive() && HumanTankLeadActive(bot, leader)) return leader;
        return nullptr;
    }

    // For the human-tank wait-gate: is `mob` currently attacking a TANK (the human
    // tank-leader or a tank-role bot)? The caller still holds DPS for a threat-
    // build delay even after this is true; a mob on the bot ITSELF / a non-tank
    // ally is handled separately by the caller.
    static bool MobOnTank(Player* bot, Unit* mob, Player* leader)
    {
        if (!mob) return false;
        Unit* const v = mob->GetVictim();
        if (!v) return false;
        if (leader && v == leader) return true;  // the human tank-leader has aggro
        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(bot->GetGUID(), party);
        for (ObjectGuid const& g : party)
            if (v->GetGUID() == g && RoleForGuid(g) == "tank")
                return true;                     // a (bot) tank has aggro
        return false;
    }

    // Threat-cap throttle: true when this DPS bot's threat on `mob` has climbed to
    // within THREAT_CAP_RATIO of the TANK's — i.e. it's about to pull, so it should
    // back off and let the tank rebuild a lead. The tank is the mob's current
    // top-threat unit (the caller only asks once MobOnTank is true, so GetVictim()
    // IS the tank). Holds if there's no tank threat yet (don't pull off ~nothing).
    static bool BotOverThreatVsTank(Player* bot, Unit* mob)
    {
        if (!bot || !mob) return false;
        Unit* const tank = mob->GetVictim();
        if (!tank || tank == bot) return false;             // no tank / we ARE the top -> not capped
        ThreatManager& tm = mob->GetThreatMgr();
        float const tankThreat = tm.GetThreat(tank);
        if (tankThreat <= 0.0f) return true;                // tank has no threat yet -> hold
        return tm.GetThreat(bot) >= tankThreat * THREAT_CAP_RATIO;
    }

    // A dungeon/world boss — exempt from the engage-lead throttle entirely. A boss
    // is tank-and-spank: the tank holds it reliably and DPS uptime matters, so the
    // bots blast/heal from the pull (Kevin: "bosses should be exempt, they can just
    // immediately dps/heal"). Also 7%-of-HP threat on a huge boss pool would be a
    // long wait for no benefit.
    static bool IsBossUnit(Unit* u)
    {
        Creature* const c = u ? u->ToCreature() : nullptr;
        return c && (c->IsDungeonBoss() || c->isWorldBoss());
    }

    // PURE-THREAT engage gate: has the tank built a real lead on THIS mob — enough
    // that it's "properly engaged", not just lightly aggroed (a right-click, a
    // passive patrol, a mob the tank merely auto-selected / that wandered in)? The
    // tank must hold threat >= ENGAGE_THREAT_HEALTH_FRAC of the mob's max health.
    // No timer: it tracks live threat, so deliberately pulling one extra mob onto a
    // locked pack only gates that ONE mob, and an accidental patrol-aggro just sits
    // unengaged instead of freezing the party. Caller has already confirmed the tank
    // is the mob's top-threat (MobOnTank), so GetVictim() IS the tank.
    static bool TankHasEngageLead(Unit* mob, float frac = ENGAGE_THREAT_HEALTH_FRAC)
    {
        if (!mob) return false;
        Unit* const tank = mob->GetVictim();
        if (!tank) return false;
        float const floor = float(mob->GetMaxHealth()) * frac;
        return mob->GetThreatMgr().GetThreat(tank) >= floor;
    }

    // Bot lead-tank pull gather gate (forward-declared up by g_tankGather). Holds the
    // party (IsPartyPullPending) from the moment the tank commits to a pull until it has
    // threat on the pack — covering BOTH a multi-pull cluster and a single-mob barge
    // (TankLeadEngagement marks a 1-mob set for melee/safe_pull-off tanks). The party is
    // held while EITHER:
    //   - the tank is still APPROACHING (tank not in combat) — it's running into the
    //     pack and DPS must not engage ahead of it; OR
    //   - the tank HAS a set member (it's the mob's target) but hasn't built a real
    //     engage lead on it yet (ENGAGE_THREAT_HEALTH_FRAC of max HP) — it's mid-lock.
    // Releases (stops gating) the instant a member is: dead/gone, a boss (tank-and-
    // spank), locked by the tank (lead built), or — once the tank IS in combat — a
    // straggler/ally-peel the tank isn't holding (waiting can't fix it and a held DPS
    // can't peel it). Pure threat once engaged (no fixed DPS-wait timer); the GC
    // backstop (TANK_GATHER_GC_MS) only evicts a pull that got stuck before engaging.
    // Drops the entry once every member is released.
    static bool IsTankGathering(Player* tank)
    {
        if (!tank) return false;
        std::vector<ObjectGuid> set;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_tankGather.find(tank->GetGUID().GetCounter());
            if (it == g_tankGather.end()) return false;
            if (getMSTime() >= it->second.untilMs)   // GC backstop only (abandoned entry)
            {
                g_tankGather.erase(it);
                return false;
            }
            set = it->second.set;
        }
        bool const tankInCombat = tank->IsInCombat();
        for (ObjectGuid const& g : set)
        {
            Creature* m = ObjectAccessor::GetCreature(*tank, g);
            if (!m || !m->IsAlive()) continue;   // dead/gone -> released
            if (IsBossUnit(m))       continue;   // boss -> no gather hold
            if (m->GetVictim() == tank)
            {
                // The tank HAS it — hold only until it's built a real engage lead.
                if (!TankHasEngageLead(m, ENGAGE_THREAT_HEALTH_FRAC)) return true;
                continue;   // locked -> released
            }
            // Not (yet) on the tank:
            //  - APPROACH phase (tank not in combat): it's still running into the pack,
            //    so HOLD — without this DPS engage before the tank ever reaches melee
            //    (Kevin's "DPS instantly engage before the tank is even in melee range";
            //    the old within-9y check released them during the whole run-in).
            //  - tank already in combat: this is a straggler the pull didn't catch, or a
            //    mob that aggroed an ally — neither resolves by waiting and a held DPS
            //    can't peel it, so don't gate the party on it.
            if (!tankInCombat) return true;
            // else: straggler / ally-peel -> released
        }
        // every alive member released -> gather complete; drop the entry so it can't linger.
        std::lock_guard<std::mutex> lock(g_mutex);
        g_tankGather.erase(tank->GetGUID().GetCounter());
        return false;
    }

    // While a DPS/healer is waiting for the human tank, don't keep idling just because
    // the mob it happened to pick isn't tank-held yet — find ANY mob the tank ALREADY
    // holds (top-threat + engage lead) that this bot can hit and isn't over the cap on,
    // and return the NEAREST one so it engages that instead (Kevin: "abort the wait as
    // soon as I have threat on one mob, even if a far mob isn't aggroed"). nullptr if
    // the tank holds nothing engageable yet → the caller keeps waiting.
    static Unit* PickTankEngagedTarget(Player* bot, Player* leader)
    {
        if (!bot || !leader) return nullptr;
        Unit* best = nullptr;
        float bestDist = 1e9f;
        for (auto const& [refGuid, ref] : leader->GetCombatManager().GetPvECombatRefs())
        {
            Unit* const m = ref->GetOther(leader);
            if (!m || !m->IsAlive() || !bot->IsValidAttackTarget(m)) continue;
            if (!MobOnTank(bot, m, leader)) continue;        // tank must be its top-threat
            if (!TankHasEngageLead(m)) continue;             // ...with a real engage lead
            if (BotOverThreatVsTank(bot, m)) continue;       // we're at the cap on it → leave it
            float const d = bot->GetDistance(m);
            if (d < bestDist) { bestDist = d; best = m; }
        }
        return best;
    }

    // Healer threat-hold: should this healer SKIP its heals right now? Heal threat
    // is split across every mob the healer is in combat with and rips any mob the
    // tank hasn't yet locked down. So HOLD while the tank still has a mob it's the
    // top-threat on but hasn't built an engage lead on — i.e. it's still locking the
    // pack. Resumes the instant every such mob is locked (pure threat, no timer).
    // Gated behind the per-bot wait-tank-threat toggle (off -> heal normally);
    // emergency-overridden if anyone drops low. Cleanse/rez are never suppressed
    // (the rotation gate only skips the direct-heal verbs). Consulted from TickRotation.
    bool HealerShouldHoldHeal(Player* bot)
    {
        if (!bot) return false;
        if (RoleForGuid(bot->GetGUID()) != "healer") return false;

        // BODY-PULL gather active (bot lead tank — the gather is bot-tank-only): the
        // gathering tank builds NO threat yet, so a heal lands its threat on the healer and
        // rips the not-yet-tanked pack onto it. Hold heals for the whole gather — with the
        // same emergency override
        // (heal NOW if anyone's in real danger). Clears the instant the gather concludes and
        // the tank starts building threat.
        if (Player* lt = PartyLeadTank(bot))
            if (lt != bot && TankGatherActive(lt->GetGUID().GetCounter()))
            {
                std::vector<ObjectGuid> gp;
                GetPartyGuidsFor(bot->GetGUID(), gp);
                for (ObjectGuid const& g : gp)
                    if (Player* m = ObjectAccessor::FindConnectedPlayer(g))
                        if (m->IsAlive() && m->GetHealthPct() <= TANK_GATHER_LOW_PCT)
                            return false;   // someone in danger -> heal now
                return true;                // hold during the body-pull gather
            }

        ObjectGuid const lg = GetLeaderFor(bot->GetGUID());
        if (!lg) return false;
        Player* leader = ObjectAccessor::FindConnectedPlayer(lg);
        if (!leader || !HumanTankLeadActive(bot, leader)) return false;
        if (!WaitForHumanTank(bot->GetGUID())) return false;   // toggle off -> heal normally
        if (!leader->IsInCombat()) return false;

        // Is the tank still locking the pack? Any mob the tank is the top-threat on
        // that lacks an engage lead = a fresh pull heal-threat would rip off it.
        bool tankStillLocking = false;
        for (auto const& [refGuid, ref] : leader->GetCombatManager().GetPvECombatRefs())
        {
            Unit* const mob = ref->GetOther(leader);
            if (!mob || !mob->IsAlive()) continue;
            if (IsBossUnit(mob)) continue;                   // bosses: heal freely
            if (mob->GetVictim() != leader) continue;        // not the tank's to lock
            if (!TankHasEngageLead(mob)) { tankStillLocking = true; break; }
        }
        if (!tankStillLocking) return false;                 // pack locked -> heal normally

        // Emergency override: heal NOW if the tank or any member is in real danger
        // — the hold is to let the tank build threat, not to let anyone die waiting.
        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(bot->GetGUID(), party);
        for (ObjectGuid const& g : party)
            if (Player* m = ObjectAccessor::FindConnectedPlayer(g))
                if (m->IsAlive() && m->GetHealthPct() <= TANK_GATHER_LOW_PCT)
                    return false;
        return true;
    }

    // True when `bot` is a non-tank under a human tank-lead with the wait toggle on
    // — the regime where it (and its PET) wait for the tank to take threat. Pairs
    // with the bot's victim: AssistTarget only hands it a victim once the tank has
    // the engage lead on a mob, so "regime active AND the bot has no victim" means
    // the bot is HOLDING, and its hunter/warlock pet should heel instead of charging
    // in on the pull. Consulted by MaintainBotPet.
    bool BotWaitsForHumanTank(Player* bot)
    {
        if (!bot) return false;
        if (RoleForGuid(bot->GetGUID()) == "tank") return false;
        ObjectGuid const lg = GetLeaderFor(bot->GetGUID());
        if (!lg) return false;
        Player* leader = ObjectAccessor::FindConnectedPlayer(lg);
        if (!leader || !HumanTankLeadActive(bot, leader)) return false;
        return WaitForHumanTank(bot->GetGUID());
    }

    // Public wrapper over the (file-local) IsPartyPullPending so the rotation engine can
    // apply the SAME pull-hold to a DPS bot's OFFENSE that the follow layer applies to its
    // MOVEMENT: while the lead tank is still body-pulling/locking the pull, every offensive
    // cast holds (else the mage Blizzards/Frostbolts the pack before the tank has threat —
    // Mill). Releases the instant the tank has the pack (IsPartyPullPending -> false).
    bool PartyPullHoldActive(Player* bot) { return IsPartyPullPending(bot); }

    // ========================================================================
    // VEHICLE behaviour. WotLK has many vehicle fights (Oculus drakes, Malygos P3,
    // Wintergrasp, Flame Leviathan…). Without this a bot whose leader mounts a vehicle
    // just MoveFollows on the ground and "walks in the air", and a bot that somehow gets
    // ON a vehicle spams its dead normal spells. EVERYTHING here is gated behind
    // GetVehicleBase() (bot or leader on a vehicle), so normal follow + combat are
    // provably untouched when no vehicle is involved.
    // ========================================================================
    static std::unordered_map<uint32, uint32> g_vehAcquireMs;   // guidLow -> last ride-cast ms
    static std::unordered_map<uint32, uint32> g_vehCastMs;      // guidLow -> last ability-cast ms

    // The Oculus drakes are PER-PLAYER summons, not free-standing vehicles a bot could
    // board — so when the leader is on one, a bot gets its OWN by casting the matching
    // ride spell on itself (the same spell the drake-giver casts on a player). entry->ride.
    static uint32 OculusRideSpellForDrake(uint32 vehEntry)
    {
        switch (vehEntry)
        {
            case 27692: return 49427;   // Emerald Drake
            case 27755: return 49459;   // Amber Drake
            case 27756: return 49463;   // Ruby Drake
            default:    return 0;
        }
    }

    // Most-injured party member that is itself on a vehicle (so a vehicle heal/shield has
    // a sensible target) at or below pctMax. Includes the leader. Returns the VEHICLE base.
    static Unit* MostInjuredVehicleAlly(Player* bot, Player* leader, uint32 pctMax)
    {
        Unit* best = nullptr; uint32 bestPct = pctMax + 1;
        auto consider = [&](Player* p)
        {
            if (!p || !p->IsAlive()) return;
            Unit* veh = p->GetVehicleBase();
            if (!veh || !veh->IsAlive()) return;
            uint32 const pct = veh->GetHealthPct();
            if (pct <= pctMax && pct < bestPct) { bestPct = pct; best = veh; }
        };
        if (leader) consider(leader);
        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(bot->GetGUID(), party);
        for (ObjectGuid const& g : party)
            consider(ObjectAccessor::FindConnectedPlayer(g));
        return best;
    }

    // Fire one of the vehicle creature's own abilities at `target`. The vehicle creature
    // owns the spell + its cooldown, so casting FROM it is how the seat's abilities go off.
    static bool CastVehicleAbility(Creature* vc, uint32 spellId, Unit* target)
    {
        if (!vc || !target || !target->IsAlive()) return false;
        if (!sSpellMgr->GetSpellInfo(spellId)) return false;
        if (vc->HasSpellCooldown(spellId)) return false;
        if (!vc->IsWithinLOSInMap(target)) return false;
        vc->CastSpell(target, spellId, false);
        return true;
    }

    // Movement side, called from the follow ticker ONLY in a vehicle scenario. Returns
    // true when it owns the bot this tick (caller then skips the normal ground follow).
    bool TickBotVehicleMovement(Player* bot, Player* leader)
    {
        if (!bot || !leader) return false;
        Unit* botVeh  = bot->GetVehicleBase();
        Unit* leadVeh = leader->GetVehicleBase();

        if (botVeh)   // bot is riding -> drive its vehicle after the leader's
        {
            if (!leadVeh) { bot->ExitVehicle(); return true; }   // leader got off -> so do we
            Creature* vc = bot->GetVehicleCreatureBase();
            if (vc && vc->IsAlive())
            {
                // Fly after the leader's vehicle in a loose fan. Re-issue only when not
                // already following it — re-issuing every tick would reset the spline.
                if (vc->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                {
                    int const fi = FormationIndexFor(bot->GetGUID(), leader->GetGUID());
                    float const ang = 2.4f + 0.5f * float(fi >= 0 ? fi : 0);
                    vc->GetMotionMaster()->Clear();
                    vc->GetMotionMaster()->MoveFollow(leadVeh, 14.0f, ang);
                }
            }
            return true;
        }

        if (leadVeh)   // leader is riding, bot is not -> get the bot onto a vehicle too
        {
            if (uint32 ride = OculusRideSpellForDrake(leadVeh->GetEntry()))
            {
                uint32 const now = getMSTime();
                uint32& last = g_vehAcquireMs[bot->GetGUID().GetCounter()];
                if (now - last > 3000) { last = now; bot->CastSpell(bot, ride, true); }
                return true;
            }
            // No known way to put the bot on this vehicle type yet — at least DON'T walk
            // in the air after the flying leader. Hold position. (Generic free-vehicle
            // boarding is a follow-up.)
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            return true;
        }
        return false;
    }

    // Ability side, called from TickRotation when the bot is in a vehicle. Returns true
    // (handled) so the normal rotation is skipped — the bot's normal spells don't work in
    // a vehicle; its abilities come from the vehicle's override bar.
    bool TickBotVehicleAbilities(Player* bot)
    {
        if (!bot) return true;
        Creature* vc = bot->GetVehicleCreatureBase();
        if (!vc) return true;
        if (bot->IsNonMeleeSpellCast(false, false, true)) return true;

        uint32 const now = getMSTime();
        uint32& last = g_vehCastMs[bot->GetGUID().GetCounter()];
        if (now - last < 1300) return true;   // ~GCD throttle

        Player* leader = ObjectAccessor::FindConnectedPlayer(GetLeaderFor(bot->GetGUID()));
        Unit* enemy = nullptr;
        if (leader)
        {
            enemy = leader->GetVictim();
            if (!enemy && leader->GetVehicleBase()) enemy = leader->GetVehicleBase()->GetVictim();
        }
        if (!enemy) enemy = vc->GetVictim();
        Unit* injured = MostInjuredVehicleAlly(bot, leader, 65);

        // Pass 0: keep allies up (a positive/heal ability on the injured ally). Pass 1:
        // damage the leader's target. First castable ability wins the tick.
        for (int pass = 0; pass < 2; ++pass)
            for (uint8 i = 0; i < MAX_CREATURE_SPELLS; ++i)
            {
                uint32 const spellId = vc->m_spells[i];
                if (!spellId) continue;
                SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId);
                if (!si) continue;
                bool const positive = si->IsPositive();
                if (pass == 0 && !(positive && injured)) continue;
                if (pass == 1 && positive) continue;
                Unit* const target = positive ? injured : enemy;
                if (CastVehicleAbility(vc, spellId, target)) { last = now; return true; }
            }
        return true;
    }

    static constexpr float FOCUS_SCAN_RANGE = 50.0f;   // how far to look for a "focus:" add

    // Nearest live, attackable enemy whose name is in `names` (the rotation
    // "focus:" list) within `range`. Forces the whole party onto a must-kill add
    // (Chaos Rift / Frost Tomb) the instant it appears.
    static Unit* FindNearestFocusEnemy(Player* bot, std::vector<std::string> const& names, float range)
    {
        std::vector<std::string> low;
        low.reserve(names.size());
        for (std::string s : names)
        {
            for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
            low.push_back(std::move(s));
        }
        std::list<Creature*> crs;
        NearbyFocusEnemyCheck check(bot, low, range);
        Acore::CreatureListSearcher<NearbyFocusEnemyCheck> searcher(bot, crs, check);
        Cell::VisitObjects(bot, searcher, range);
        Creature* best = nullptr;
        float bestDist = range + 1.0f;
        for (Creature* c : crs)
        {
            float const d = bot->GetDistance(c);
            if (d < bestDist) { bestDist = d; best = c; }
        }
        return best;
    }

    // Can the bot actually WALK to (x,y,z)? Asks the navmesh — rejects a spot the
    // path can't reach (NOPATH), only reaches partway (INCOMPLETE — clamped at a
    // wall/ledge), or is off-mesh (FARFROMPOLY). On a map with NO mmaps every query
    // is a SHORTCUT straight line, which we ACCEPT (MovePoint falls back to straight-
    // line there anyway). Used by the no-LoS recovery to tell "behind a corner on my
    // level" (reachable → close in) from "on a platform a level up" (unreachable →
    // climb to the party instead of standing blind). Mirrors PartyRotation's copy.
    static bool NavReachable(Player* bot, float x, float y, float z, float straight)
    {
        PathGenerator gen(bot);
        if (!gen.CalculatePath(x, y, z, false)) return false;
        PathType const t = gen.GetPathType();
        if (t & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY))
            return false;
        return gen.getPathLength() <= straight * 2.5f + 8.0f;
    }

    // Anti-flank tank positioning. A tank pulling a SPREAD pack ends up standing in the
    // middle with mobs hitting it from ALL sides — back hits ignore block/dodge/parry, so
    // a surrounded tank takes far more damage (Kevin: "high-end M+ impossible"). Keep every
    // melee attacker inside the tank's FRONTAL arc:
    //   * find the biggest angular GAP in the ring of attackers (the empty side);
    //   * if the pack spans > 180° (someone is behind no matter how we face), step a few
    //     yards INTO that empty side — the chasing mobs collapse onto the occupied side and
    //     funnel to the front; repeat until they're all within 180°;
    //   * always face the middle of the occupied arc so the frontal cone covers the most
    //     mobs (and the victim, which is one of them, stays meleeable).
    // Returns true when it owns the tank's feet/facing this tick (caller returns), false to
    // fall through to the normal formation chase (0/1 attacker, or boxed against a wall).
    // Anti-flank funnel NUDGE. Returns true ONLY on the brief tick it issues a reposition
    // step; otherwise false so the tank fights NORMALLY (chases + auto-swings) — the v1 that
    // planted/idled when "not flanked" and fired with 2 attackers froze the tank mid-pull and
    // stopped its swings (Kevin: "Nissedaza just stands still, doesn't swing"). Now it only
    // acts on a genuine SURROUND (4+ melee attackers spanning >180°), nudges at most once every
    // couple seconds (lets the mobs collapse + the tank keep swinging between nudges), and never
    // idles the tank.
    static bool TankAntiFlankNudge(Player* tank)
    {
        if (!tank) return false;
        std::vector<float> angs;   // world angle tank -> each melee attacker
        for (Unit* a : tank->getAttackers())
        {
            if (!a || !a->IsAlive()) continue;
            if (tank->GetExactDist2d(a) > 10.0f) continue;   // only the melee ring matters
            angs.push_back(tank->GetAngle(a));
        }
        if (angs.size() < 4) return false;   // not a real surround — fight normally

        std::sort(angs.begin(), angs.end());
        float maxGap   = (2.0f * float(M_PI) - angs.back()) + angs.front();
        float gapStart = angs.back();
        for (size_t i = 1; i < angs.size(); ++i)
        {
            float const g = angs[i] - angs[i - 1];
            if (g > maxGap) { maxGap = g; gapStart = angs[i - 1]; }
        }
        if (maxGap >= float(M_PI)) return false;   // not flanked: all within a half-plane -> fight

        uint32 const gLow = tank->GetGUID().GetCounter();
        uint32 const now  = getMSTime();
        // Let an in-flight nudge finish (and don't re-path / re-navmesh every tick).
        if (tank->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE
            && tank->isMoving())
            return true;
        // Throttle: one nudge, then fight (return false) for ~2s so the chasing pack collapses
        // onto the open side and the tank keeps swinging — instead of shuffling every tick.
        static thread_local std::unordered_map<uint32, uint32> nextNudgeMs;
        uint32& next = nextNudgeMs[gLow];
        if (now < next) return false;

        float const openDir    = Position::NormalizeOrientation(gapStart + maxGap * 0.5f);
        float const clusterDir = Position::NormalizeOrientation(openDir + float(M_PI));
        constexpr float STEP = 4.0f;
        float ox = tank->GetPositionX() + std::cos(openDir) * STEP;
        float oy = tank->GetPositionY() + std::sin(openDir) * STEP;
        float oz = tank->GetPositionZ();
        tank->UpdateAllowedPositionZ(ox, oy, oz);
        if (!NavReachable(tank, ox, oy, oz, STEP))
            return false;   // boxed against a wall — fight in place (the wall is cover anyway)
        next = now + 2000;
        // clusterDir as MovePoint's ARRIVAL facing — a separate SetFacingTo would relaunch a
        // stand-still spline and cancel the step.
        tank->GetMotionMaster()->MovePoint(0, ox, oy, oz, FORCED_MOVEMENT_NONE,
                                           0.0f, clusterDir, /*generatePath=*/true,
                                           /*forceDestination=*/false);
        return true;
    }

    void AssistTarget(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld()) return;
        uint32 const gLow = bot->GetGUID().GetCounter();
        // Stamp feral-form presence every tick so a brief out-of-form cast keeps
        // melee positioning for FERAL_FORM_GRACE_MS (see WasRecentlyFeral).
        MarkFeralForm(bot);

        // User-controlled body: never touch its target/motion.
        if (bot->HasUnitFlag(UNIT_FLAG_POSSESSED)) { AssistLog(gLow, "skip: possessed"); return; }
        // Still MOUNTED while in combat = a transport fly-by (a real fight would have
        // been dismounted by TickRotation's mount guard, which runs first). Don't
        // engage/stand-ground — that's what froze the party on incidental aggro; let
        // the follow ticker keep it riding after the leader.
        if (bot->IsMounted()) { AssistLog(gLow, "skip: mounted (transport fly-by)"); return; }
        // In a vehicle: the vehicle drives movement + targeting (TickBotVehicleMovement /
        // TickBotVehicleAbilities). Ground melee/chase here would fight the vehicle.
        if (bot->GetVehicleBase()) { AssistLog(gLow, "skip: in vehicle"); return; }
        // Don't interrupt a cast in progress.
        if (bot->IsNonMeleeSpellCast(false, false, true)) { AssistLog(gLow, "skip: casting"); return; }
        // Rotation engine has parked this bot (drinking, etc).
        if (IsFollowerHeld(bot->GetGUID())) { AssistLog(gLow, "skip: held by rotation"); return; }
        // Lead tank mid BODY-PULL/gather: do NOTHING here — no target, no attack (so it
        // builds NO threat until the pack is grouped), no chase. TankLeadEngagement /
        // TankGatherStep own the feet and the rotation is suppressed (TickRotation). Resumes
        // the instant the gather concludes -> then it engages, builds threat, DPS follow (Mill).
        if (TankIsBodyPulling(bot)) { AssistLog(gLow, "skip: body-pulling (gather) — move only"); return; }
        // stop_attacking hold (e.g. Mirrored Soul): drop the victim and DON'T engage or
        // chase — heals/movement run elsewhere; this governs offence only.
        if (IsOffensiveHeld(bot->GetGUID()))
        {
            if (bot->GetVictim()) bot->AttackStop();
            AssistLog(gLow, "skip: offensive hold (stop_attacking)");
            return;
        }

        ObjectGuid const leaderGuid = GetLeaderFor(bot->GetGUID());
        if (!leaderGuid)
        {
            AssistLog(gLow, "skip: no leader directive (not a managed bot)");
            return;
        }

        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader || !leader->IsInWorld()) { AssistLog(gLow, "skip: leader not in world"); return; }
        if (leader->GetMapId() != bot->GetMapId()) { AssistLog(gLow, "skip: leader on different map"); return; }

        // BATTLEGROUND leash: keep party bots within 30y of the human so the healer can't
        // wander off (e.g. to cure random raid bots) and leave the human to die when a fight
        // breaks out (Kevin's rule: "in BGs party members should NEVER go more than 30 yards
        // away from you"). Tighter than the 50y open-world leash below, and we actively walk
        // back rather than waiting for the 1Hz follow tick, so the healer is back in heal
        // range fast. Returns to ~8y, so it can range out to 30y again before re-leashing.
        if (bot->InBattleground() && bot->GetDistance(leader) > 30.0f)
        {
            if (bot->GetVictim()) bot->AttackStop();
            if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                bot->GetMotionMaster()->MoveFollow(leader, 8.0f, bot->GetFollowAngle());
            AssistLog(gLow, "skip: BG 30y leash — rejoining leader");
            return;
        }

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

        // Coordinated pull: while the lead tank is range-pulling and the pack
        // hasn't reached it yet, every OTHER bot holds fire — no attacks, pets
        // heel (AttackStop clears the victim the pet keys off) — so nobody chases
        // a feared/running mob into the next pack before the tank has threat.
        // Self-defence is exempt: a mob already swinging at us in melee is part of
        // the pull we're waiting on, not a new pull, so we may fight back rather
        // than stand and die. Lead tank itself is never gated (it IS the puller).
        // guid -> captured tank standoff for the current pull (0 = not captured).
        // map-update thread owns this bot, so thread_local is race-free (matches
        // wasAlive / combatState). Dropped when the pull ends so the next one
        // re-captures from the fresh geometry.
        static thread_local std::unordered_map<uint32, float> g_pullAnchor;
        // Non-tanks currently dragging a loose add back to the tank (see the
        // drag-to-tank block below). thread_local for the same race-free reason.
        static thread_local std::unordered_set<uint32> g_dragToTank;
        if (!IsLeadTank(bot->GetGUID()) && IsPartyPullPending(bot))
        {
            bool meleeThreatOnMe = false;
            for (Unit* a : bot->getAttackers())
                if (a && a->IsAlive() && bot->IsWithinMeleeRange(a)) { meleeThreatOnMe = true; break; }
            if (!meleeThreatOnMe)
            {
                if (bot->GetVictim()) bot->AttackStop();
                // Anchor to the TANK and ride its backward retreat — done HERE (the
                // map-tick assist loop, ~30Hz) not in the 1Hz follow ticker, so it
                // engages the instant the tank commits to the pull instead of a tick
                // late (the bot was trailing the leader, then sprinting back). Hold
                // the SAME standoff it had via the leader (captured once so the
                // leader's roaming can't reel it in); melee anchor close, ready to
                // fight. Backpedal STRAIGHT BEHIND the tank (PULL_REAR_ANGLES), away
                // from the pack — never to the flanks.
                MovementGeneratorType const mg =
                    bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
                Player* const tank = PartyLeadTank(bot);
                if (tank && tank != bot)
                {
                    bool const melee = FollowerIsMelee(bot);
                    // A ranged bot whose LEADER also fights at range holds WITH the
                    // human (their safe standoff side), not behind the tank — human
                    // players don't cross the tank's front to "go to range", and that
                    // crossing was proximity-pulling the next pack (Kevin).
                    bool const leaderRanged = leader && leader != tank && LeaderFightsAtRange(leader);
                    float& held = g_pullAnchor[gLow];
                    bool const firstTick = held <= 0.0f;
                    if (firstTick)
                    {
                        held = melee ? 6.0f
                                     : (leader ? bot->GetDistance(leader) + leader->GetDistance(tank)
                                               : 12.0f);
                        held = std::max(held, melee ? 5.0f : 12.0f);
                        held = std::min(held, 45.0f);
                    }
                    // Force the anchor on the first pull tick even though we were
                    // already FOLLOWing (the leader) — otherwise the stale leader
                    // follow would persist. Re-assert too if the follow was lost.
                    if (firstTick || mg != FOLLOW_MOTION_TYPE)
                    {
                        int const fi = FormationIndexFor(bot->GetGUID(), GetLeaderFor(bot->GetGUID()));
                        if (bot->getStandState() != UNIT_STAND_STATE_STAND)
                            bot->SetStandState(UNIT_STAND_STATE_STAND);
                        bot->GetMotionMaster()->Clear();
                        if (!melee && leaderRanged)
                        {
                            // Cluster a few yards off the human so the ranged bot stays at
                            // range ON THE LEADER'S SIDE — never hugging the tank, never
                            // crossing its front. Fan the bearing across the leader's REAR
                            // hemisphere [0.5pi .. 1.5pi] (behind/beside, away from the pack
                            // the leader faces) so bots arc around the human instead of
                            // stacking on one line (GetFollowAngle is a flat M_PI/2 for all
                            // players), and step the distance out by index too.
                            float const followAngle = float(M_PI) * (0.5f + 0.25f * float(fi % 5));
                            float const followDist  = 6.0f + float(fi % 4) * 2.5f;   // 6..13.5y off the leader
                            bot->GetMotionMaster()->MoveFollow(leader, followDist, followAngle);
                        }
                        else
                        {
                            // Melee (must stay near the tank to engage on release), or a
                            // ranged bot with no ranged leader to cluster on: backpedal
                            // BEHIND the tank. 7+ followers wrap the 6-bearing table; push
                            // each extra ring 2.5y back so they don't stack on the inner ring.
                            float followDist = held + float(fi / 6) * 2.5f;
                            float angle = melee ? PULL_REAR_ANGLES[fi % 6]
                                                : RearArcAngleOnBotSide(bot, tank, fi);   // ranged: keep its own side
                            // Ranged: if the standoff spot would clip a fresh pack, step
                            // IN toward the tank (away from the rear pack) until clear. If
                            // even the floor still clips, tuck in tight behind the tank —
                            // the tank's own spot is the safest (it holds threat there).
                            if (!melee)
                            {
                                bool clear = false;
                                for (int t = 0; t < 4; ++t)
                                {
                                    float px, py, pz;
                                    tank->GetNearPoint(bot, px, py, pz, 0.0f, followDist,
                                                       tank->ToAbsoluteAngle(angle));
                                    if (!SpotProximityPulls(tank, px, py, pz, 11.0f)) { clear = true; break; }
                                    followDist -= 4.0f;
                                    if (followDist < 10.0f) break;
                                }
                                if (!clear) followDist = 6.0f;
                            }
                            bot->GetMotionMaster()->MoveFollow(tank, followDist, angle);
                        }
                    }
                }
                else if (mg == CHASE_MOTION_TYPE)
                    bot->GetMotionMaster()->Clear();   // no tank found — just hold fire
                AssistLog(gLow, "pull pending: anchoring at range (leader side / behind tank), holding fire");
                return;
            }
        }
        else
            g_pullAnchor.erase(gLow);   // not pulling → re-capture next pull

        // Target priority:
        //   1. Leader's explicit victim (you click, everyone follows).
        //   2. Whatever's currently swinging at the bot itself.
        //   3. Whatever's swinging at any party member on the same map.
        // Without #3 we got the "mage solos a mob while the whole party
        // stands around" bug: only the leader's target made anything fire.
        auto pickPartyDefenseTarget = [&]() -> Unit*
        {
            // Ally-peel reach. Mobs ON US (the self-defense loops below) are
            // uncapped — they're already in our melee. But peeling a mob off an
            // ALLY makes AssistTarget engage it; for a MELEE bot that's a chase, and
            // an uncapped chase chain-pulled the room (at 30y a tank sprinted to a
            // far peel and body-pulled every pack on the way, "chain-pulls until we
            // die") — so melee stay tight at 18y. A RANGED bot doesn't path-chase
            // (it stands and casts up to ~36y), so it can safely help an ally being
            // beaten on across the room — the far-body-pull case where casters used
            // to idle behind the healer (Kevin). Doubled to 36y for ranged.
            float const PARTY_DEFEND_RANGE = FollowerIsMelee(bot) ? 18.0f : 36.0f;
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
            // party-defense across our directive roster (leader + all bots +
            // henchmen) AND the WoW group (a second human + their bots) — so
            // heroes, henchmen and grouped players all defend EACH OTHER. Capped to mobs
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
            // Party-defense (peel a mob off an ALLY) applies ONLY when we're a TANK
            // (it wants aggro on everything) or we're OUT OF COMBAT. Its sole purpose
            // was to pull an IDLE dps back into a fight the party was already in
            // ("dps standing out of range, not re-engaging"). An IN-COMBAT dps must
            // NOT peel — that engages mobs the tank hasn't grabbed; it sticks to the
            // tank's target (self-defence above still covers a mob actually on us).
            // Members = our directive roster PLUS the WoW group, so a bot also peels
            // a mob off a SECOND HUMAN — or their bots — grouped with us.
            if (RoleForGuid(bot->GetGUID()) == "tank" || !bot->IsInCombat())
            {
                std::vector<Player*> members;
                auto addMember = [&](Player* m) {
                    if (m && m->IsInWorld() && m != bot && m->GetMapId() == bot->GetMapId()
                        && std::find(members.begin(), members.end(), m) == members.end())
                        members.push_back(m);
                };
                std::vector<ObjectGuid> party;
                GetPartyGuidsFor(bot->GetGUID(), party);
                for (ObjectGuid const& gg : party) addMember(ObjectAccessor::FindConnectedPlayer(gg));
                if (Group* grp = bot->GetGroup())
                    for (GroupReference* itr = grp->GetFirstMember(); itr; itr = itr->next())
                        addMember(itr->GetSource());
                for (Player* m : members)
                {
                    if (Unit* a = scanAttackers(m)) return a;
                    for (Unit* ctrl : m->m_Controlled)
                        if (Unit* a = scanAttackers(ctrl)) return a;
                }
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
            // The nearest enemy the party is ALREADY fighting — never an idle,
            // out-of-combat mob (the old grid search marched bots to their leash
            // to engage a neutral monster). Falls back to party-defense, which
            // is itself combat-only, so this can't initiate a pull.
            desired = PickNearestEngagedTarget(bot);
            if (!desired) desired = pickPartyDefenseTarget();
        }
        else if (mode == "lowest")
        {
            // Focus the weakest enemy already engaged with the party.
            desired = PickLowestHealthTarget(bot);
            if (!desired) desired = pickPartyDefenseTarget();
        }
        else if (mode == "highest")
        {
            // Focus the beefiest enemy already engaged with the party.
            desired = PickHighestHealthTarget(bot);
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
            if (leaderTargetValid)
                desired = leaderTarget;            // the leader is ACTUALLY attacking -> assist
            else
            {
                // The leader isn't attacking a valid target — common when Kevin plays
                // HEALER (mouseover-macro heals, no enemy selected). The bots must NOT
                // idle: assist the TANK's current kill, else the nearest enemy the party
                // is ALREADY fighting, else defend (a mob swinging at us/an ally). All
                // three are COMBAT-ONLY (TankVictim = the tank's live victim;
                // PickNearestEngagedTarget gates on IsInCombat; pickPartyDefenseTarget is
                // combat-only), so this still never engages a mob the leader has merely
                // SELECTED — a left-click is just targeting and pulling on it broke
                // following (Kevin: "the party should not stop following just because the
                // tank selects an enemy"). Only things already in combat are picked up.
                Unit* tv = TankVictim(bot, leader);
                if (tv && tv->IsAlive() && bot->IsValidAttackTarget(tv))
                    desired = tv;
                if (!desired) desired = PickNearestEngagedTarget(bot);
                if (!desired) desired = pickPartyDefenseTarget();
            }
        }
        // Final safety: never hand back a dead/invalid target.
        if (desired && (!desired->IsAlive() || !bot->IsValidAttackTarget(desired)))
            desired = nullptr;

        // ---- FOCUS override -----------------------------------------------------
        // A rotation "focus:<names>" rule marks must-kill adds (Chaos Rift on
        // Anomalus, Frost Tomb in Utgarde Keep). When the bot is already in combat
        // and a matching, attackable enemy is nearby, switch the WHOLE party onto
        // it NOW — computed here so it bypasses the human-tank threat gate, the
        // retarget throttle and the combo-point hold below (all gated on !focusMob).
        // Only while in combat: these adds spawn mid-fight, so this never starts a
        // pull. The tank taunts/holds it and dps pile on, killing it fast.
        Unit* focusMob = nullptr;
        if (bot->IsInCombat())
        {
            std::vector<std::string> focusNames;
            WowPsParty::BotFocusNames(bot->GetGUID(), focusNames);
            if (!focusNames.empty())
                focusMob = FindNearestFocusEnemy(bot, focusNames, FOCUS_SCAN_RANGE);
        }
        if (focusMob)
            desired = focusMob;

        // ---- Pure threat gate: a non-tank only fights what the human TANK holds --
        // In a dungeon led by a human tank, a DPS *or healer* bot attacks a mob ONLY
        // once the tank has a real engage lead on it AND the bot is under
        // THREAT_CAP_RATIO of the tank's threat (so it never rips it off). Anything
        // the tank HASN'T grabbed is simply HELD. No timer: pure live threat. The
        // healer is gated too (its offensive filler — Moonfire/wand — rips just like
        // DPS; its HEALS are gated separately in TickRotation). BOSSES are exempt —
        // a boss is tank-and-spank, the tank holds it reliably, so DPS/heal blast
        // immediately. AoE pulls still can't be perfectly gated (accepted).
        // Skipped entirely for a focus add — a must-kill rift/tomb gets immediate
        // dps from the whole party, threat be damned.
        if (!focusMob)
        {
            std::string const myRole = RoleForGuid(bot->GetGUID());
            bool const gated = desired && myRole != "tank";   // DPS AND healer; tank engages freely
            if (gated && TankLeadActive(bot, leader)
                && WaitForHumanTank(bot->GetGUID())            // default WAIT under a tank (human OR bot); '0' opts out
                && !IsBossUnit(desired))                       // bosses: no throttle, blast on
            {
                // Release ONLY once the tank holds the mob (top-threat) AND has built
                // a real engage lead on it (ENGAGE_THREAT_HEALTH_FRAC of its max HP) —
                // so a bare right-click / a passive patrol-aggro / Retribution-Aura
                // chip threat keeps the bot held. After that THREAT_CAP_RATIO governs.
                // Same 7% floor for ranged and melee: a bigger ranged lead was tried
                // but gutted ranged DPS (Kevin), so we keep it tight and instead trace
                // the release below to pin down the actual rip.
                float const leadFrac = ENGAGE_THREAT_HEALTH_FRAC;
                bool const release = MobOnTank(bot, desired, leader)
                                  && TankHasEngageLead(desired, leadFrac)
                                  && !BotOverThreatVsTank(bot, desired);
                // Release-moment trace (only when we're actually ENGAGING something new,
                // so it's one line per pull, not per tick): the exact threat picture the
                // gate saw. Pins down a rip — if tankThreat is a sliver vs botThreat the
                // lead was too thin (raise the frac); if release fired with tankThreat≈0
                // the gate let it through wrongly. Answers "did I really have zero threat?"
                if (release && bot->GetVictim() != desired)
                {
                    Unit* const tankU = desired->GetVictim();
                    ThreatManager& tm = desired->GetThreatMgr();
                    LOG_INFO("module",
                        "[WowPsParty Assist] guid={} RELEASE mob={} ranged={} leadFrac={:.2f} "
                        "mobMaxHp={} tank={} tankThreat={:.0f} floor={:.0f} botThreat={:.0f} cap={:.0f}",
                        gLow, desired->GetGUID().GetCounter(), FollowerIsMelee(bot) ? 0 : 1,
                        leadFrac, desired->GetMaxHealth(),
                        tankU ? tankU->GetGUID().GetCounter() : 0,
                        tankU ? tm.GetThreat(tankU) : 0.0f,
                        float(desired->GetMaxHealth()) * leadFrac,
                        tm.GetThreat(bot),
                        tankU ? tm.GetThreat(tankU) * THREAT_CAP_RATIO : 0.0f);
                }
                if (!release)
                {
                    // The mob we picked isn't tank-held yet — but if the tank ALREADY
                    // holds OTHER mobs (it's mid-fight on a pack), engage the nearest
                    // of THOSE instead of waiting on a far un-aggroed straggler.
                    if (Unit* const alt = PickTankEngagedTarget(bot, leader))
                    {
                        desired = alt;   // fall through and attack the tank-held mob
                    }
                    else
                    {
                        // Tank holds nothing engageable yet → wait. Loose leash only:
                        // catch up if we've drifted past HOLD_LEASH, otherwise stand
                        // put — no formation spot.
                        if (bot->GetVictim()) bot->AttackStop();
                        MovementGeneratorType const mg2 =
                            bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
                        if (bot->GetDistance(leader) > HOLD_LEASH)
                        {
                            if (mg2 != FOLLOW_MOTION_TYPE)
                                bot->GetMotionMaster()->MoveFollow(leader, HOLD_LEASH - 5.0f, bot->GetFollowAngle());
                        }
                        else if (mg2 != IDLE_MOTION_TYPE)
                        {
                            bot->StopMoving();
                            bot->GetMotionMaster()->Clear();
                            bot->GetMotionMaster()->MoveIdle();
                        }
                        AssistLog(gLow, "human-tank: waiting — tank has no engageable threat yet (loose leash)");
                        return;
                    }
                }
            }
            // Diagnostic for the "bots still rip threat" report: a non-tank in a
            // human-led dungeon whose wait-gate did NOT engage because the human
            // leader's party role isn't "tank" (so HumanTankLeadActive is false).
            // Reveals the need to set the active character's role to tank.
            else if (gated && leader->GetSession() && !sPlayerbotsMgr.GetPlayerbotAI(leader)
                     && bot->GetMap() && bot->GetMap()->IsDungeon()
                     && !HumanTankLeadActive(bot, leader))
                AssistLog(gLow, "no wait-gate: human leader's party role isn't 'tank' (LeaderRole) — bots won't hold");
        }

        // Retarget throttle. If we're already on a live, valid victim, don't
        // abandon it for a DIFFERENT one more than once per cooldown — that's
        // the spinbot fix when several mobs flank the tank. A dead/gone victim
        // (currentValid == false) drops straight through and retargets now.
        uint32 const nowMs = getMSTime();
        Unit* const current = bot->GetVictim();
        bool const currentValid = current && current->IsAlive()
                                  && bot->IsValidAttackTarget(current);
        // A focus add bypasses the throttle AND the combo-point hold below — switch
        // to it immediately even mid-cast / mid-combo; killing it is the priority.
        if (currentValid && !focusMob)
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

        // Still nothing of our own, but the PARTY is fighting — we're idle/too far
        // for party-defense to reach and the leader isn't attacking. GAP-CLOSE to
        // the party's main fight (the tank's victim) instead of standing around out
        // of combat; the engage/position bands below chase us to firing range.
        if (!desired)
            desired = PartyMainCombatTarget(bot);

        if (!desired)
        {
            // Nothing to fight anywhere — drop combat so PartyFollow can
            // resume movement. Clear the drag marker here (this return sits ABOVE
            // the drag block's own erase): otherwise a drag that ends because the
            // add died/evaded leaves the marker set, the follow ticker reasserts
            // MoveFollow(leader), and the NEXT add's first drag tick skips the
            // MoveFollow(tank) re-issue — the bot trails the leader, not the tank.
            g_dragToTank.erase(gLow);
            if (bot->GetVictim())
            {
                AssistLog(gLow, "no-targets: AttackStop");
                bot->AttackStop();
            }
            return;
        }

        // We have a target to engage — get OFF the floor first. A bot that
        // finished drinking is left seated (SustainConsume sits it; nothing stands
        // it back up once topped) and a seated unit can't move or cast, so it sat
        // far from the fight doing nothing while the party got smacked. We're past
        // the held-by-rotation guard above, so the bot isn't mid-drink; stand it up
        // so the positioning below can actually drive it in.
        if (bot->getStandState() != UNIT_STAND_STATE_STAND)
            bot->SetStandState(UNIT_STAND_STATE_STAND);

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

        // ---- Drag a loose add to the tank ---------------------------------------
        // A non-tank with a mob beating on it in melee, while too far from the tank
        // for it to peel (the tank scans ~30y around ITSELF for loose enemies via
        // FindLooseEnemy / its cast_loose_enemy taunt rule), RUNS THE ADD TO THE TANK
        // instead of standing and soloing it to death (Kevin: "they'll tank them for
        // eternity and likely die if they're too far away"). Normal player behaviour —
        // kite the add into the tank's reach; we run to the tank's threat bubble
        // (DRAG_HANDOFF_DIST, inside its melee + Consecration/Thunderclap AoE) so the
        // handoff works whether the tank taunts OR holds threat by AoE. Once back
        // within TANK_GRAB_RANGE the drag stops and the existing "stand & fight, let
        // the tank take it" handoff resumes. Skipped for: the tank itself (it WANTS
        // the add), a focus must-kill add (kill it where it is), and kiters (above).
        // The travel is intentionally UN-clamped (unlike the 18y peel-chase cap in
        // pickPartyDefenseTarget): the run is always TOWARD the tank/party safe-zone
        // and the add is already on us (not a fresh pull), so the geometry — not a
        // distance cap — is what keeps it from chain-pulling. Don't clamp it to 18y.
        static constexpr float TANK_GRAB_RANGE   = 30.0f;   // beyond this the tank can't reach the add
        static constexpr float DRAG_HANDOFF_DIST = 8.0f;    // run the add to here from the tank
        if (!focusMob && RoleForGuid(bot->GetGUID()) != "tank")
        {
            Player* const dragTank = PartyTankPlayer(bot, leader);
            // A mob actually ON us in melee (targeting US, in the dead-zone band) —
            // it FOLLOWS when we run. A ranged caster mob that stands off won't, so
            // dragging wouldn't bring it along; leave those to the normal bands.
            Unit* onMe = nullptr;
            if (dragTank && dragTank != bot && bot->GetDistance(dragTank) > TANK_GRAB_RANGE)
                for (Unit* a : bot->getAttackers())
                    if (a && a->IsAlive() && a->GetVictim() == bot
                        && bot->IsValidAttackTarget(a) && bot->GetDistance(a) < 8.0f)
                    { onMe = a; break; }
            if (onMe)
            {
                if (bot->GetVictim() != onMe)
                {
                    MarkRetarget(gLow, nowMs);
                    bot->Attack(onMe, false);   // keep it engaged; we drive the feet
                }
                MovementGeneratorType const dmg =
                    bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
                // Force the follow on the first drag tick (the bot may have been CHASE/
                // IDLE on the add, or still FOLLOWing the leader) and re-issue only if
                // the follow was lost — re-issuing every tick stutters the spline.
                if (!g_dragToTank.count(gLow) || dmg != FOLLOW_MOTION_TYPE)
                {
                    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
                        bot->SetStandState(UNIT_STAND_STATE_STAND);
                    bot->GetMotionMaster()->Clear();
                    bot->GetMotionMaster()->MoveFollow(dragTank, DRAG_HANDOFF_DIST, bot->GetFollowAngle());
                }
                g_dragToTank.insert(gLow);
                AssistLog(gLow, "drag-to-tank: add on me, too far for the tank to peel — running it in");
                return;
            }
        }
        g_dragToTank.erase(gLow);   // reached here -> not dragging this tick

        // Lead-tank ranged pull, three beats while the mob hasn't reached melee:
        //   1. out of range  -> close to ability/throw range so the pull can fire.
        //   2. in range, not yet in combat -> hold and let the rotation pull.
        //   3. pull connected (in combat) -> back STRAIGHT AWAY once (PULL_RETREAT_
        //      YDS), then hold at that point so the pack is dragged into the open,
        //      separated from neighbours, and the tank locks threat before melee.
        // The window is refreshed each tick so it survives the back-up; the instant
        // the mob reaches melee (or the window lapses) this falls through to engage.
        //
        // Wait cap: a RANGED mob never walks into melee — so at PULL_MAX_WAIT_MS,
        // END the window NOW (don't just stop refreshing, which left a ~4s residual)
        // so the tank stops holding/retreating, engages it normally (closes to the
        // mob) via the bands below, and the party is released. Caps the "stood there
        // taking ranged damage forever" at ~5s.
        if (IsTankPulling(bot->GetGUID())
            && !bot->IsWithinMeleeRange(desired)
            && TankPullElapsedMs(bot->GetGUID().GetCounter()) >= PULL_MAX_WAIT_MS)
            ClearTankPulling(bot->GetGUID().GetCounter());

        if (IsTankPulling(bot->GetGUID()) && !bot->IsWithinMeleeRange(desired))
        {
            if (bot->GetVictim() != desired)
            {
                MarkRetarget(gLow, nowMs);
                bot->Attack(desired, true);
            }

            // Open the pull with the FREE physical ranged weapon if one's equipped
            // (gun/bow/crossbow/thrown). A fresh tank has ~0 rage, so relying on a
            // rage-gated ability (or a cooldown'd Heroic Throw) can fail the pull
            // outright; the ranged weapon never can. Self-guards on weapon/range/
            // ammo and no-ops otherwise, so a paladin/DK/druid (no physical ranged
            // weapon) just falls through to its rotation's ability opener. Runs
            // each tick so it fires the instant we're in range, and the auto-repeat
            // sustains while the pack closes.
            WowPsParty::FireRangedWeaponShot(bot, desired);

            float const d = bot->GetDistance(desired);
            uint32 const tankLow = bot->GetGUID().GetCounter();
            // Rolling refresh (NOT a one-shot extend — each tick re-arms a fresh
            // 4s) so the party stays held through the back-up. BUT only while the
            // pull is genuinely progressing: in combat (mob fighting us) or still
            // within pull range. If the mob EVADES — runs home, drops combat,
            // out of range — stop refreshing so the window drains and the party
            // releases in a couple of seconds instead of freezing until the mob
            // de-leashes.
            // Keep the party held while the pull is genuinely progressing: still
            // closing to/at pull range, OR in combat (mob inbound/fighting us). An
            // EVADE (mob runs home, drops combat, out of range) stops refreshing so
            // the window drains and we engage; the hard wait cap above (which ends
            // the window outright) covers a ranged mob that stays in combat at range.
            float const holdRange = WowPsParty::TankPullHoldRange(bot);
            float const closeThreshold = holdRange + 2.0f;
            bool const stillPulling = d <= closeThreshold || bot->IsInCombat();
            if (stillPulling)
                MarkTankPulling(bot->GetGUID(), 4000);
            MovementGeneratorType const mg =
                bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
            float rx, ry, rz;
            if (GetTankPullRetreat(tankLow, rx, ry, rz))
            {
                // Already backing out / parked at the chosen point — HOLD here.
                // MUST be checked FIRST: the back-out point is intentionally beyond
                // pull range, so if the "d > closeThreshold -> close in" beat ran
                // first it would re-approach and undo the back-up every tick (the
                // "tank never backs up after it shoots" bug). Feet left alone; just
                // face the mob.
            }
            else if (d > closeThreshold)
            {
                if (mg != CHASE_MOTION_TYPE)
                    bot->GetMotionMaster()->MoveChase(desired, holdRange);   // close to pull range
            }
            else if (!bot->IsInCombat())
            {
                // In range but the opener hasn't connected yet. Track the mob at
                // hold range with a CHASE rather than stopping dead: a mob that
                // WALKS AWAY (patrol, fear, repath) would otherwise stroll out of
                // range while we stand frozen at the spot it just left, and since
                // the opener never lands the pull never "connects" — the tank stuck
                // out of range forever. A stationary mob settles the chase at
                // holdRange with no jitter, so an auto-repeat opener still fires;
                // a moving one is followed so an instant opener (Heroic Throw,
                // Avenger's Shield, ...) keeps landing. The instant it connects
                // (IsInCombat) we back up below.
                if (mg != CHASE_MOTION_TYPE)
                    bot->GetMotionMaster()->MoveChase(desired, holdRange);
            }
            else
            {
                // Pull connected — step straight back from the pack immediately and
                // STICK (retreatSet makes the branch above own the feet next tick).
                float bx, by, bz;
                desired->GetNearPoint(bot, bx, by, bz, bot->GetObjectSize(),
                    d + PULL_RETREAT_YDS, desired->GetAngle(bot));
                bot->GetMotionMaster()->MovePoint(0, bx, by, bz);
                SetTankPullRetreat(tankLow, bx, by, bz);
            }
            bot->SetFacingToObject(desired);
            AssistLog(gLow, "tank pull: range-pull, back up, let the pack close");
            return;
        }

        // ===== Engage + position ============================================
        // Single source of truth for combat movement (one mover, no clashes).
        uint8 const acls = bot->getClass();
        std::string const role = RoleForGuid(bot->GetGUID());
        // EVERY healer fights/heals from range — including a holy PALADIN, which
        // isn't in the caster class list below — so it stays back near the group
        // and never melees a mob it pulls.
        bool rangedCaster =
            role == "healer" ||
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
                role == "tank" ||
                ((acls == CLASS_DRUID || acls == CLASS_SHAMAN)
                    && WowPsParty::PrimaryTalentTree(bot) == 1) ||
                bot->IsInFeralForm() || WasRecentlyFeral(bot->GetGUID());
            if (melee) rangedCaster = false;
        }

        // Set the victim (drives auto-attack / has_target — and gives the rotation
        // a target so a healer can filler-DPS / wand at high mana). Melee gets a
        // swing; ranged does NOT (must never run into melee). `desired` is ONLY
        // ever the party's ACTUAL fight here — the mere-SELECTION engage was
        // removed — so a healer's victim means a real fight, never a pull, and a
        // mere left-click never strands a bot (out of combat desired==null and we
        // already returned above, so everyone keeps following).
        bool const newVictim = bot->GetVictim() != desired;
        // A holy PALADIN healer weaves into melee to white-swing its only free,
        // no-mana filler (the healer block below drives it there, and it gets the
        // melee-attack flag here). Restricted to paladins on purpose: they're plate
        // and tanky enough to eat melee-range AoE for the free damage. Resto
        // SHAMAN/DRUID heal from range — their weapon swing is weak and their armor
        // squishy, so the AoE tradeoff isn't worth it; they stay back (and being
        // rangedCaster, they get no melee flag either). Wand healers (priest)
        // free-DPS at range via Shoot.
        bool const meleeHealer = role == "healer" && bot->getClass() == CLASS_PALADIN;
        if (newVictim)
        {
            MarkRetarget(gLow, nowMs);
            bool const meleeAuto = !rangedCaster || meleeHealer;
            bot->Attack(desired, meleeAuto);
            LOG_INFO("module", "[WowPsParty Assist] guid={} ENGAGE victim_guid={} ranged={}",
                     gLow, desired->GetGUID().GetCounter(), rangedCaster ? 1 : 0);
        }

        // The LEAD TANK's FEET are owned by TankLeadEngagement / TankGatherStep while it's
        // pulling or gathering — they run AFTER this assist pass and body-pull the mob/add
        // (deduped via DriveTankChase). If we ALSO issue a chase here, the two fight every
        // tick: this pass re-picks the tank's victim and chases it at a FORMATION ORBIT
        // angle whenever the victim changes, while the gather chases its add DEAD-ON — so
        // the tank spazzes/turns in place and never closes (the recurring "tank won't body-
        // pull, stands looking at the mob" bug). Keep the victim + auto-attack set above;
        // just don't move. Once the pull/gather ends, this pass owns the combat chase again.
        if (IsLeadTank(bot->GetGUID())
            && (TankGatherActive(bot->GetGUID().GetCounter()) || IsTankPulling(bot->GetGUID())))
            return;

        MovementGeneratorType const mg =
            bot->GetMotionMaster()->GetCurrentMovementGeneratorType();

        // HEALER positioning. A RANGED healer — a priest (wands via Shoot) or a
        // resto shaman/druid — heals from RANGE and never chases (the offensive
        // ranged bands below would kite it toward packs); it loose-anchors near the
        // leader. Only a PALADIN healer (meleeHealer) weaves into melee so its
        // white swing — its only free, no-mana filler — lands; heals reach 40y, so
        // it still tops the party from there. All keep the victim above ONLY so the
        // rotation/auto-attack can filler-DPS the party's already-engaged mob;
        // none pull.
        if (role == "healer")
        {
            float const leashDist = bot->GetDistance(leader);
            // No LINE OF SIGHT to the leader = can't actually heal the party, even
            // though we may be within the straight-line leash (a level/platform gap
            // reads as "close" through the floor). This is the recurring "healer
            // stranded a level below while the party fights above" bug: the leash
            // below saw <30y and the block fell through to MoveIdle, so the healer
            // stood blind on the lower deck. Climb to the leader's EXACT spot with a
            // navmesh MovePoint (not the distance-banded MoveFollow, which would
            // settle directly below them and never round the ramp) until LoS clears.
            if (!bot->IsWithinLOSInMap(leader, VMAP::ModelIgnoreFlags::M2) && leashDist > 8.0f)
            {
                if (mg != POINT_MOTION_TYPE)
                {
                    bot->GetMotionMaster()->MovePoint(0, leader->GetPositionX(),
                        leader->GetPositionY(), leader->GetPositionZ(),
                        FORCED_MOVEMENT_NONE, 0.0f, 0.0f, /*generatePath=*/true,
                        /*forceDestination=*/false);
                    AssistLog(gLow, "healer: no LoS to leader — climbing to rejoin/heal the party");
                }
                return;
            }
            constexpr float HEAL_LEASH_FAR  = 30.0f;   // start catching up past this
            constexpr float HEAL_LEASH_NEAR = 18.0f;   // settle once back within this
            bool const following = (mg == FOLLOW_MOTION_TYPE) && bot->isMoving();
            bool const needClose = leashDist > HEAL_LEASH_FAR
                                 || (following && leashDist > HEAL_LEASH_NEAR);
            if (needClose)
            {
                if (mg != FOLLOW_MOTION_TYPE)
                    bot->GetMotionMaster()->MoveFollow(leader, 10.0f, bot->GetFollowAngle());
                AssistLog(gLow, "healer: out of heal range — closing to the leader");
            }
            // Melee-weapon healer: weave into melee to white-swing, but ONLY while
            // the target sits near the party — never chase a loose mob out of the
            // group. The leader-leash above reels it back if the fight wanders off.
            else if (meleeHealer && desired && leader->GetDistance(desired) < 25.0f)
            {
                if (newVictim || mg != CHASE_MOTION_TYPE)
                    bot->GetMotionMaster()->MoveChase(desired);
                else if (!bot->HasInArc(float(M_PI), desired))
                    bot->SetFacingToObject(desired);
                AssistLog(gLow, "melee healer: weaving in to white-swing the party's target");
            }
            else
            {
                if (mg != IDLE_MOTION_TYPE)
                {
                    bot->StopMoving();
                    bot->GetMotionMaster()->Clear();
                    bot->GetMotionMaster()->MoveIdle();
                }
                if (desired && !bot->HasInArc(float(M_PI), desired))
                    bot->SetFacingToObject(desired);
                AssistLog(gLow, "healer: holding near party — heal in place, no enemy chase");
            }
            return;
        }

        if (rangedCaster)
        {
            // RANGED bands (AC's chase never enforces a MIN range, and ChaseAngle
            // ORBITS — it's relative to the mob's facing, so as the mob turns to
            // face the bot the target point moves and the bot chases it forever:
            // the "spazz on the same spot". So: no angle, and DON'T MOVE when
            // already in a safe firing position.
            //   < 8y          too close -> stand & fight if a mob's on us, else
            //                              back straight out to ~13y to shoot
            //   8..hold +LoS  SAFE      -> stand still and shoot (no movement)
            //   > hold / noLoS          -> close in (plain chase, no angle), once
            // `hold` is per-bot: the shortest-range nuke in its rotation (clamped
            // 18..28y) so it positions where its WHOLE kit reaches, not at a flat
            // 30y where only the longest spell is usable.
            float const d   = bot->GetDistance(desired);
            // M2: ignore doodad clutter, matching Spell::CheckCast — the default
            // check counts decorative models, so a caster behind a mine cart/rail
            // read "no LoS" and never settled to fire though the spell would land.
            bool  const los = bot->IsWithinLOSInMap(desired, VMAP::ModelIgnoreFlags::M2);
            float const hold = WowPsParty::BotRangedCastHold(bot);
            if (los) ClearLosSeek(gLow);   // settled with LoS -> reset the give-up clock

            // A mob is in melee on us → STAND and fight/heal, NEVER kite, no matter
            // where `desired` is (could be the tank's far target). Kiting drags the
            // mob off the tank and around the room; standing keeps it where the tank
            // can grab it — and a HEALER that pulls aggro holds its ground instead of
            // running off. The hunter swings in the dead zone (its shots are dead-
            // zoned this close); casters/healers cast point-blank. Once nothing's on
            // us (tank took it / it died) the bands below resume ranged positioning.
            // (Hoisted above the <8y band so it covers aggro from a SECOND mob while
            // we're at firing range of the first — that bot must plant too.)
            // CLOSE gate (<8y): getAttackers() is populated the instant a mob
            // aggros, while it may still be sprinting in from 30y — without this a
            // far add would freeze a bot mid-approach to its target, out of range.
            // 8y matches the dead-zone band, so the bot still holds at firing range
            // for distant attackers and only plants once one is actually on it.
            bool mobOnMe = false;
            for (Unit* a : bot->getAttackers())
                if (a && a->IsAlive() && bot->GetDistance(a) < 8.0f) { mobOnMe = true; break; }
            if (mobOnMe)
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
                AssistLog(gLow, "ranged: mob in melee — stand and fight, don't run off");
                return;
            }

            // No line of sight (target up stairs / behind a pillar / around a
            // corner): MoveChase below is the WRONG tool — it only maintains a
            // DISTANCE band, with no concept of LoS. If the target is already
            // within [15, hold] by straight-line distance the chase generator
            // thinks we're in position and never moves, so the bot stands blind
            // and the party clears the pack without it (Kevin, 2026-06-19: shadow
            // priest froze at the bottom of the stairs). And if we're inside 15y
            // the chase would back us AWAY, worsening LoS. Force a navmesh
            // MovePoint toward the target until LoS clears — the same blind-kiter
            // recovery the rotation's keep_distance_enemy rule uses. Sits ABOVE
            // the <8y back-out so a too-close-but-blind bot closes to see rather
            // than retreating out of LoS.
            if (!los)
            {
                // Where we'd step to regain LoS: ~4y PAST the target's edge (a near-point
                // still ~10y out usually sits on the SAME blind side of the corner, so the
                // bot paths there, STILL has no LoS, and settles — the old "caster stuck
                // behind a corner" bug). 4y is past the edge: LoS clears en route and the
                // fire-bands back it out to firing range the instant it does.
                float lx, ly, lz;
                desired->GetNearPoint(bot, lx, ly, lz, 0.0f, 4.0f, desired->GetAngle(bot));
                bool const reachable = NavReachable(bot, lx, ly, lz, d);

                if (reachable)
                {
                    // Tight corner / tiny room (the SM Cathedral boss room): if we still
                    // can't settle WITH LoS after the give-up window, stop trying to reach
                    // ideal firing range. Arm the range-routine suppression so the back-out
                    // bands below won't yank us out of LoS once we round into the room, then
                    // KEEP CLOSING into LoS and fight from there (point-blank if need be) —
                    // instead of standing blind in the doorway dealing zero damage (Kevin).
                    // Suppressing the retreat is what breaks the round-the-corner ->
                    // fire-band-backs-us-out -> LoS-drops oscillation the old stand-ground
                    // hold was working around.
                    if (LosSeekElapsed(gLow, nowMs) >= LOS_SEEK_GIVEUP_MS)
                        SuppressRangeRoutine(gLow, nowMs);
                    // Corner / staircase on OUR level: path to the walkable near-target
                    // spot (generatePath rounds the corner / climbs stairs;
                    // forceDestination=false so an unreachable spot just isn't taken).
                    if (mg != POINT_MOTION_TYPE)
                    {
                        bot->GetMotionMaster()->MovePoint(0, lx, ly, lz, FORCED_MOVEMENT_NONE,
                                                          0.0f, 0.0f, /*generatePath=*/true,
                                                          /*forceDestination=*/false);
                        AssistLog(gLow, "ranged: no LoS — closing to regain line of sight");
                    }
                }
                else
                {
                    // Target on geometry we CAN'T reach from here — a platform a level up,
                    // the shaman-stranded-on-the-web-below-the-fight bug. Climb to the
                    // LEADER's exact spot (valid ground in the thick of the fight) so the
                    // route brings us up where LoS clears. NOT subject to the stand-ground
                    // give-up above — else a bot would strand itself a level below.
                    if (mg != POINT_MOTION_TYPE)
                    {
                        bot->GetMotionMaster()->MovePoint(0, leader->GetPositionX(),
                            leader->GetPositionY(), leader->GetPositionZ(),
                            FORCED_MOVEMENT_NONE, 0.0f, 0.0f, /*generatePath=*/true,
                            /*forceDestination=*/false);
                        AssistLog(gLow, "ranged: no LoS + target unreachable from here — climbing to the leader to rejoin the fight");
                    }
                }
                bot->SetFacingToObject(desired);
                return;
            }

            if (d < 8.0f)
            {
                // Range routine paused (we just gave up finding a firing spot in a tiny
                // room): do NOT back out — that drops LoS again and we deal zero damage.
                // Stand in LoS and fight point-blank until the suppression lapses (then we
                // retry the back-out, re-arming it if the room's still too tight). Hunter
                // gets dead-zone white swings; casters/healers cast from here.
                if (los && RangeRoutineSuppressed(gLow, nowMs))
                {
                    if (mg != IDLE_MOTION_TYPE)
                    {
                        bot->StopMoving();
                        bot->GetMotionMaster()->Clear();
                        bot->GetMotionMaster()->MoveIdle();
                    }
                    if (bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) && acls != CLASS_HUNTER)
                        bot->Attack(desired, false);
                    if (acls == CLASS_HUNTER)
                        bot->Attack(desired, true);   // white melee swings in the dead zone
                    bot->SetFacingToObject(desired);
                    AssistLog(gLow, "ranged: range routine paused (tight room) — fighting in place, in LoS");
                    return;
                }
                // Nothing on us but we're <8y (walked in, or the mob died / was
                // taken). Back out just PAST the dead zone (13y) so we can shoot
                // again — a ranged special shot's effective min range is ~10y for a
                // normal mob (spell min + melee range), so 13y is just clear of it.
                // The rotation's own too-close check nudges it further for big mobs.
                // Drop any melee.
                if (bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    bot->Attack(desired, false);
                // Re-evaluating the retreat runs a grid scan; throttle it so a boxed-in
                // bot (no safe spot → holding) doesn't scan every tick.
                if (mg != POINT_MOTION_TYPE && BackoutScanReady(gLow, nowMs))
                {
                    MarkBackoutScan(gLow, nowMs);
                    float bx, by, bz;
                    if (SafeRangedBackout(bot, desired, leader, 13.0f, bx, by, bz))
                    {
                        bot->GetMotionMaster()->MovePoint(0, bx, by, bz);
                        AssistLog(gLow, "ranged: too close, backing out to firing range");
                    }
                    else if (mg != IDLE_MOTION_TYPE)
                    {
                        // Backing out either way would clip a fresh pack — hold and fight
                        // in place rather than proximity-pull the next group.
                        bot->StopMoving();
                        bot->GetMotionMaster()->Clear();
                        bot->GetMotionMaster()->MoveIdle();
                        AssistLog(gLow, "ranged: too close but a back-out would pull a fresh pack — holding");
                    }
                }
                bot->SetFacingToObject(desired);
                return;
            }

            if (d <= hold && los)
            {
                // SAFE — the user's "don't move when it can ranged attack". Kill
                // any leftover movement ONCE (so Auto Shot can fire), then leave
                // the feet completely alone; just keep facing the target.
                if (bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    bot->Attack(desired, false);   // back at range — stop meleeing, resume shots
                // On the chase->arrive transition, if we've stacked on another
                // stood-off caster at the LoS chokepoint, take a ONE-SHOT step a
                // few yards into the open (fanned by formation index) so the party
                // doesn't bunch pixel-perfect at a tight corner. The POINT move is
                // left to complete by the back-out guard below; the cooldown stops
                // it re-firing when the chase generator resumes underneath.
                if (mg == CHASE_MOTION_TYPE &&
                    SpreadStepReady(gLow, nowMs) &&
                    IsBunchedAtStandoff(bot, desired, RANGED_SPREAD_RADIUS))
                {
                    float sx, sy, sz;
                    if (ComputeRangedSpreadSpot(bot, desired, hold, sx, sy, sz)
                        && !SpotProximityPulls(bot, sx, sy, sz, 10.0f))   // don't spread INTO a fresh pack
                    {
                        MarkSpreadStep(gLow, nowMs);
                        bot->GetMotionMaster()->MovePoint(0, sx, sy, sz);
                        bot->SetFacingToObject(desired);
                        AssistLog(gLow, "ranged: bunched at the corner — stepping into the open to spread");
                        return;
                    }
                }
                // CRUCIAL: a back-out (POINT motion) carries the bot from the <8y
                // dead zone out to ~13y, and it passes THROUGH this 8..hold band on
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
            // Upper bound = the per-bot hold, so the bot settles INSIDE its kit's
            // reach (not back at 25y where a shorter nuke is still out of range).
            if (bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                bot->Attack(desired, false);   // chasing back to range — stop meleeing, resume shots
            if (mg != CHASE_MOTION_TYPE)
                bot->GetMotionMaster()->MoveChase(desired, ChaseRange(15.0f, hold));
            bot->SetFacingToObject(desired);
            return;
        }

        // MELEE — close to contact, fanned out by formation angle so the melee
        // companions surround the mob (orbiting is fine when you're in contact).
        // TANK anti-flank: when SURROUNDED by 4+ on a spread AoE pull, nudge into the open
        // side so the pack funnels to the front (back hits ignore block/dodge/parry). It
        // returns true ONLY on the brief nudge tick; otherwise the tank fights normally
        // (chases + swings) via the formation chase below. Non-tanks never enter it.
        if (RoleForGuid(bot->GetGUID()) == "tank" && TankAntiFlankNudge(bot))
            return;
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
        // Rear-arc bearings (relative to the leader's facing; M_PI = directly
        // behind) so followers fan out instead of stacking on one point. Indexed
        // by FORMATION ORDINAL. Used by the normal leader-follow formation block
        // below. The range-pull tank anchor (in AssistTarget) uses its OWN tighter
        // PULL_REAR_ANGLES table instead — this fan deliberately includes the
        // +-pi/2 flanks, which are wrong (sideways) for backing off a pull.
        static constexpr float FORM_ANGLES[6] = {
            float(M_PI),            // directly behind
            float(M_PI) * 0.72f,    // behind-left
            float(M_PI) * 1.28f,    // behind-right
            float(M_PI) * 0.5f,     // left flank
            float(M_PI) * 1.5f,     // right flank
            float(M_PI) * 0.9f,     // back-left inner
        };

        // ====================================================================
        //  Movement humanization
        // --------------------------------------------------------------------
        // Followers used to MoveFollow rigid, identical bearings and snap in
        // lockstep the instant the leader moved — unmistakably robotic. The
        // 1 Hz ApplyDirective pass still owns ALL safety (leash, stuck-teleport,
        // combat yield, revive, cross-map); for a bot it judges to be in pure
        // open-follow it now just records an eligibility flag + a jittered slot
        // and DELEGATES the actual follow-install to a fast (250 ms) humanize
        // tick. That fast tick re-checks the cheap critical guards live so it
        // can never fight AssistTarget, and adds the human texture: per-bot
        // formation jitter, a sub-second STAGGERED peel-off when the leader
        // starts moving, and gentle idle WANDER on a short leash. Its worst-
        // case failure is a bot standing a beat or taking one stray step — the
        // 1 Hz net (which measures real position movement, cadence-agnostic)
        // still rescues any genuinely stuck bot.
        struct FollowHumanize
        {
            bool   eligible       = false;  // 1 Hz pass: this bot is in pure open-follow
            float  slotAngle      = float(M_PI);
            float  slotDist       = 2.0f;
            float  apptAngle      = 0.0f;    // last follow angle we asserted (re-issue throttle)
            float  apptDist       = 0.0f;
            bool   followAsserted = false;
            bool   wasLeaderMoving = false;
            uint32 reactAtMs      = 0;       // staggered peel-off: don't move before this
            uint32 driftRerollMs  = 0;       // next slow-drift reroll
            float  driftAngle     = 0.0f;
            float  driftDist      = 0.0f;
            bool   wandering      = false;
            uint32 wanderUntilMs  = 0;
            uint32 nextFidgetMs   = 0;
        };
        // World-thread only (the follow WorldScript OnUpdate drives both the
        // 1 Hz pass and the fast tick), so no mutex is needed.
        static std::unordered_map<uint32, FollowHumanize> g_humanize;

        static constexpr uint32 HUMANIZE_INTERVAL_MS = 250;
        static constexpr uint32 WANDER_POINT_ID       = 0x7A9D;  // distinct MovePoint id

        // Stable per-bot offset in [-amp, amp], constant across ticks (a bot's
        // "personality" — so two of the same class don't sit on one bearing).
        static float StableJitter(uint32 guidLow, uint32 salt, float amp)
        {
            uint32 h = guidLow * 2654435761u + salt * 2246822519u;
            h ^= h >> 15;
            float const u = float(h & 0xFFFFu) / 65535.0f;   // 0..1
            return (u * 2.0f - 1.0f) * amp;
        }

        // Shortest-arc interpolation between two absolute bearings (radians).
        static float LerpAngle(float from, float to, float t)
        {
            float diff = to - from;
            while (diff >  float(M_PI)) diff -= 2.0f * float(M_PI);
            while (diff < -float(M_PI)) diff += 2.0f * float(M_PI);
            return from + diff * t;
        }

        // Per-bot peel-off delay (ms) so the party doesn't lurch as one unit:
        // formation index sequences the wave, a stable hash scatters within it.
        static uint32 StaggerDelayMs(uint32 guidLow, int formationIndex)
        {
            float const base = float(formationIndex % 5) * 110.0f;
            float const jit  = StableJitter(guidLow, 7, 90.0f) + 90.0f;  // 0..180
            return uint32(base + jit);                                   // ~0..620 ms
        }

        // Does any OTHER party member have a victim / is in combat? Mirrors the
        // 1 Hz "don't follow into a pull" yield so the fast tick honours it too.
        static bool AnyPartyMemberEngaged(ObjectGuid self, Player* follower)
        {
            std::vector<ObjectGuid> party;
            GetPartyGuidsFor(self, party);
            for (ObjectGuid const& gg : party)
            {
                if (gg == self) continue;
                Player* m = ObjectAccessor::FindConnectedPlayer(gg);
                if (m && m->IsInWorld() && m->IsAlive()
                    && m->GetMapId() == follower->GetMapId()
                    && (m->IsInCombat() || m->GetVictim()))
                    return true;
            }
            return false;
        }

        // Auto-vote on every pending group-loot roll for this bot. We hard-return
        // out of mod-playerbots' UpdateAI, suppressing its default loot-roll
        // action, so without this our party bots never respond to a roll and the
        // player has to click for each. Heroes (enrolled alts) GREED so loot stays
        // in the party; HENCHMEN PASS so they never WIN a roll and accumulate junk
        // in their bags — henchman bags are kept empty by design (see
        // ClearHenchmanInventory). Either vote answers the roll so it isn't stuck.
        static void AutoGreedRolls(Player* bot)
        {
            if (!bot) return;
            Group* g = bot->GetGroup();
            if (!g) return;
            RollVote const vote = IsHenchman(bot->GetGUID()) ? PASS : GREED;
            for (Roll* roll : g->GetRolls())
            {
                if (!roll) continue;
                auto it = roll->playerVote.find(bot->GetGUID());
                if (it == roll->playerVote.end() || it->second != NOT_EMITED_YET)
                    continue;
                g->CountRollVote(bot->GetGUID(), roll->itemGUID, vote);
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

            // On a taxi flight (escorting the leader's flight path): the flight
            // spline owns the bot's movement. Skip EVERYTHING below — MoveFollow,
            // the leash, the mount-sync, and especially the cross-map teleport
            // (which would otherwise yank the bot off its taxi the moment the
            // flight crosses into the destination's map). Resumes the instant it
            // lands (IsInFlight clears).
            if (follower->IsInFlight()) return true;

            // VEHICLE scenarios (the leader mounted a vehicle, or the bot is on one) are
            // owned by the vehicle driver — board/acquire, fly after the leader, exit.
            // Gated on a vehicle actually being involved, so the normal ground follow
            // below is untouched otherwise. (A dead bot falls through to the revive logic.)
            if (follower->IsAlive()
                && (follower->GetVehicleBase() || leader->GetVehicleBase())
                && TickBotVehicleMovement(follower, leader))
                return true;

            // Re-form the party if an enrolled member got dropped from the leader's group.
            // Exiting Wintergrasp/LFG, OR the human accidentally clicking "Leave Party",
            // disbands or reshuffles the group — the member stays enrolled but ungrouped, and
            // the human ends up out of their OWN roster having to re-invite themselves (Kevin).
            // Re-add it, creating the group with the human as LEADER if needed — so the human
            // is back in their party too. Heroes re-add directly (OnRemoveMember never
            // dismisses them); a henchman needs the regroup guard so the RemoveFromGroup below
            // doesn't trip OnRemoveMember's dismiss. Skip while a BG/battlefield war owns the
            // group (don't fight its battle-group management); heals in the open world.
            // ALSO skip while the FOLLOWER itself is in a BG/arena queue or match: when the
            // human enters a rated arena the heroes are briefly ungrouped while their own
            // invite is pending, and re-grouping them here yanks them out of the arena queue
            // before they can port in — the "heroes stuck in Dalaran, 1v5" bug. Leave a
            // queued/in-match follower alone; the party re-forms once the match is over.
            if (follower->IsAlive() && !leader->InBattleground()
                && !follower->InBattleground() && !follower->InBattlegroundQueue())
            {
                Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(leader->GetZoneId());
                bool const inWar = bf && bf->IsWarTime();
                Group* lg = leader->GetGroup();
                if (!inWar && (!lg || !lg->IsMember(d.followerGuid)))
                {
                    if (!lg)
                    {
                        lg = new Group();
                        if (lg->Create(leader)) sGroupMgr->AddGroup(lg);
                        else { delete lg; lg = nullptr; }
                    }
                    if (lg && !lg->IsMember(d.followerGuid) && !lg->IsFull())
                    {
                        if (follower->GetGroup())
                        {
                            if (d.henchman) SetHenchmanRegrouping(d.followerGuid, true);
                            follower->RemoveFromGroup();
                            if (d.henchman) SetHenchmanRegrouping(d.followerGuid, false);
                        }
                        lg->AddMember(follower);
                        LOG_INFO("module",
                            "[WowPsParty Follow] re-grouped {} {} into {}'s party (was dropped — "
                            "WG/LFG exit or an accidental leave)",
                            d.henchman ? "henchman" : "hero", follower->GetName(), leader->GetName());
                    }
                }
            }

            // Default the humanize tick OFF for this bot. Only the pure open-
            // follow tail below flips it back on; every early-return path
            // (combat, hold, leash, stuck, cross-map, dead, tank-lead) thus
            // leaves the fast tick disabled, so it never fights those systems.
            g_humanize[d.followerGuid.GetCounter()].eligible = false;

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
                ForceMovableState(follower);
                follower->GetMotionMaster()->Clear();
                follower->GetMotionMaster()->MoveIdle();
                follower->StopMoving();
                LOG_INFO("module",
                    "[WowPsParty Follow] {} auto-accepted resurrect",
                    follower->GetName());
                return true;
            }

            // DELAYED party-wipe regroup auto-rez. A dead follower is revived at the
            // leader ONLY after the leader has been alive + OUT OF COMBAT continuously
            // for AUTO_REZ_DELAY_MS (Kevin: don't autorez immediately — delay ~1 min
            // out of combat as a safety net). This keeps the no-stuck-after-a-wipe
            // backstop while removing the instant rez-and-teleport the moment combat
            // drops, and it never battle-rezzes mid-pull. The timer resets the instant
            // eligibility lapses (leader re-enters combat, bot gets rezzed by a healer,
            // map changes), so it only fires after a genuine full minute of calm. A
            // living healer's rez_party still revives the bot at its corpse immediately
            // (auto-accepted above) — this is purely the last-resort whole-party-dead path.
            static constexpr uint32 AUTO_REZ_DELAY_MS = 60000;
            {
                static thread_local std::unordered_map<uint32, uint32> deadCalmSinceMs;
                uint32 const gl = d.followerGuid.GetCounter();
                bool const eligible =
                    !follower->IsAlive() && !follower->isResurrectRequested()
                    && leader->IsAlive() && !leader->IsInCombat()
                    && leader->GetMapId() == follower->GetMapId();
                if (!eligible)
                {
                    deadCalmSinceMs.erase(gl);
                }
                else
                {
                    uint32 const now = getMSTime();
                    uint32& since = deadCalmSinceMs[gl];
                    if (since == 0) since = now;          // first calm tick — start the clock
                    if (now - since >= AUTO_REZ_DELAY_MS)
                    {
                        deadCalmSinceMs.erase(gl);
                        follower->ResurrectPlayer(1.0f, false);   // full HP/mana, no sickness
                        follower->SpawnCorpseBones();
                        follower->TeleportTo(leader->GetMapId(),
                            leader->GetPositionX(), leader->GetPositionY(),
                            leader->GetPositionZ(), leader->GetOrientation());
                        ForceMovableState(follower);
                        follower->GetMotionMaster()->Clear();
                        follower->GetMotionMaster()->MoveIdle();
                        follower->StopMoving();
                        LOG_INFO("module",
                            "[WowPsParty Follow] {} auto-revived at leader (party regroup, "
                            "after {}s out of combat)", follower->GetName(), AUTO_REZ_DELAY_MS / 1000);
                        return true;
                    }
                }
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
                    uint32 const ustate = follower->GetUnitState();
                    ForceMovableState(follower);
                    follower->GetMotionMaster()->Clear();
                    follower->GetMotionMaster()->MoveIdle();
                    follower->StopMoving();
                    if (follower->getStandState() != UNIT_STAND_STATE_STAND)
                        follower->SetStandState(UNIT_STAND_STATE_STAND);
                    LOG_INFO("module",
                        "[WowPsParty Follow] {} revived — scrubbed death-state motion "
                        "(unitState was {:#x}, moveFlags {:#x})",
                        follower->GetName(), ustate, follower->GetUnitMovementFlags());
                    return true;   // one clean settle tick; MoveFollow re-asserts next tick
                }
            }

            // Cross-map: leader has entered a dungeon (or any other instance)
            // and the follower is still on the old map. Yank the follower
            // through with TeleportTo. Skip while the follower is mid-cast or
            // currently being teleported themselves; we'll retry next tick.
            if (follower->GetMapId() != leader->GetMapId())
            {
                // NEVER pull a follower OUT of a battleground/arena to a leader who
                // isn't in one. On an arena pop the hero bots are driven to accept and
                // enter the instance immediately; if the human dawdles ~5s before
                // clicking Enter Battle, this teleport would yank every hero back to the
                // still-in-world leader, emptying the human's side so the bot team wins
                // instantly (Kevin's "wait 5s -> instant loss", 100% repro). The leader
                // joins within the prep window and they converge on the same map; if the
                // leader declines, the BG removes the heroes on match end. (The reverse —
                // follower in the world, leader already in the arena — is NOT guarded, so
                // a lagging hero still gets pulled INTO the leader's arena.)
                if (follower->InBattleground() && !leader->InBattleground())
                    return true;
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

            // ---- Leader-fall snap ------------------------------------------
            // The leader jumped off a ledge / into a hole (detected in OnUpdate,
            // which armed a teleport once the landing grace elapsed). MoveFollow
            // often can't path down, and the >100y leash never fires because the
            // drop is closer than that — so snap any STRANDED follower onto the
            // leader. A 3D distance > FALL_SNAP_MIN_DIST means it didn't fall
            // along (the vertical drop alone exceeds that), so a bot that DID come
            // down with the leader is left to follow normally. Runs before the
            // combat / leash yields so a stranded bot rejoins even mid-fight.
            if (follower->IsAlive() && !follower->IsInFlight()
                && !follower->IsBeingTeleported() && !follower->GetVehicleBase())
            {
                auto fit = g_leaderFall.find(d.leaderGuid.GetCounter());
                if (fit != g_leaderFall.end() && fit->second.teleportDueMs != 0
                    && getMSTime() >= fit->second.teleportDueMs
                    && follower->GetExactDist(leader) > FALL_SNAP_MIN_DIST)
                {
                    if (follower->GetVictim()) follower->AttackStop();
                    if (follower->IsInCombat()) follower->CombatStop();
                    if (follower->getStandState() != UNIT_STAND_STATE_STAND)
                        follower->SetStandState(UNIT_STAND_STATE_STAND);
                    follower->GetMotionMaster()->Clear();
                    follower->StopMoving();
                    ClearGatherNode(d.followerGuid.GetCounter());
                    follower->TeleportTo(leader->GetMapId(),
                        leader->GetPositionX(), leader->GetPositionY(),
                        leader->GetPositionZ(), leader->GetOrientation());
                    LOG_INFO("module",
                        "[WowPsParty Follow] leader-fall snap: {} -> leader {} (dropped out of reach)",
                        follower->GetName(), leader->GetName());
                    return true;
                }
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

            // Catch-up speed: every managed follower (an enrolled hero alt OR a
            // hired henchman — both live in g_directives) speeds up the FURTHER
            // behind the leader it falls, so it matches pace when close but
            // visibly hurries when it lags. Multiplier is applied to the LEADER's
            // current run rate (never the follower's own, which would compound),
            // so one rule covers BOTH states: on foot the leader is 1.0; mounted,
            // the party's matched mount makes the leader's rate the bot's natural
            // mounted speed, so the tiers mean "% of their mount speed" too.
            //   <10y: 100% (no overshoot when already on top of the leader)
            //   10-20y: 103%   20-30y: 105%   >30y: 110%
            // Only act while both share the same mount state, so a mid-transition
            // mount/dismount can't briefly fling an on-foot bot at mounted speed.
            // The +/-0.01 guard avoids re-sending a packet until the tier changes.
            if (follower->IsMounted() == leader->IsMounted())
            {
                float const dist = follower->GetDistance(leader);
                float mult;
                if      (dist < 10.0f) mult = 1.00f;
                else if (dist < 20.0f) mult = 1.03f;
                else if (dist < 30.0f) mult = 1.05f;
                else                   mult = 1.10f;
                float const target = leader->GetSpeedRate(MOVE_RUN) * mult;
                float const cur    = follower->GetSpeedRate(MOVE_RUN);
                if (cur < target - 0.01f || cur > target + 0.01f)
                    follower->SetSpeed(MOVE_RUN, target, true);
            }

            // Mount matching — keep the follower's mounted state synced with the
            // leader's so the party doesn't trail on foot during travel. The bot
            // mounts its OWN level/race-appropriate mount (not a clone of the
            // leader's), matching only the leader's ground-vs-flying type.
            //
            // A bot normally can't mount in combat — but if the LEADER is mounted
            // AND out of combat, the human is travelling, not fighting, so the
            // bot's combat is just incidental road aggro (a mob that tagged it as
            // we rode past). Force-mount anyway (a triggered cast isn't blocked by
            // combat) and let mounted speed carry it off — the mob falls behind
            // and the fly-by guard in TickRotation keeps it mounted. Without this,
            // a hero that catches aggro on the open road never mounts and runs on
            // foot the whole way: the "heroes only mount up in cities" report
            // (cities have no mobs to keep them in combat).
            bool const leaderMounted   = leader->IsMounted();
            bool const botMounted      = follower->IsMounted();
            bool const botCasting      = follower->IsNonMeleeSpellCast(false, false, true);
            bool const leaderTravelling = leaderMounted && !leader->IsInCombat();
            if (leaderMounted && !botMounted && !botCasting
                && (!follower->IsInCombat() || leaderTravelling))
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
                {
                    // Self-heal a stuck state: a lingering SPELL_AURA_MOUNTED while
                    // NOT mounted (a desync from an old Dismount-only path) makes the
                    // cast just refresh the aura without mounting. Strip it first so
                    // the cast applies a FRESH aura and actually mounts.
                    if (follower->HasAuraType(SPELL_AURA_MOUNTED))
                        follower->RemoveAurasByType(SPELL_AURA_MOUNTED);
                    follower->CastSpell(follower, mountSpell, true);
                    // Verbose-gated: once mounted the bot stays mounted (the
                    // TickRotation/AssistTarget mount guards keep mounted bots
                    // from initiating attacks, so AURA_INTERRUPT_FLAG_MOUNT never
                    // fires), so this logs ~once per travel-mount — but gate it
                    // anyway so a pathological flicker can't flood Server.log.
                    if (follower->IsInCombat() && IsLogVerbose())
                        LOG_INFO("module",
                            "[WowPsParty Follow] {} force-mounted through travel "
                            "aggro (leader {} mounted + out of combat)",
                            follower->GetName(), leader->GetName());
                }
            }
            else if (!leaderMounted && botMounted && !follower->IsInCombat() && !botCasting)
            {
                // Remove the mount AURA, not just call Dismount(). Dismount()
                // clears the mounted flag but leaves SPELL_AURA_MOUNTED applied,
                // so the NEXT mount cast only refreshes that lingering aura — its
                // apply handler (which calls Mount()) never re-runs — and the bot
                // reads as un-mounted, spams the cast, and never mounts again
                // (Kevin: "heroes can only mount up once"). RemoveAurasByType runs
                // the aura's OnRemove -> Dismount() and leaves a clean state.
                follower->RemoveAurasByType(SPELL_AURA_MOUNTED);
                follower->Dismount();   // belt-and-braces if a mount set the flag without an aura
            }

            // ---- Phase mirror (twilight realms etc.) ----------------------
            // In an INSTANCE, a HERO alt mirrors the leader's phase so it follows
            // through a phase SHIFT — e.g. the Obsidian Sanctum twilight portals
            // (SPELL_TWILIGHT_SHIFT phases you into the twilight realm to kill the
            // disciple). The follow-teleport below snaps to the leader's XYZ but NOT
            // its phase, so without this the bot stands in the normal realm while the
            // leader fights alone in twilight ("can't solo the mob in there"). Snap the
            // bot onto the leader on the shift so it lands in the realm together; it
            // mirrors BACK to phase 1 the same way when the leader leaves. HENCHMEN are
            // deliberately excluded — they stay behind in the normal phase (the user
            // wants only the 4 heroes in, the 5 henchmen out). Runs before the leash so
            // a phase mismatch always wins.
            if (!IsHenchman(d.followerGuid)
                && follower->FindMap() && follower->FindMap()->Instanceable()
                && follower->GetPhaseMask() != leader->GetPhaseMask()
                && !follower->IsBeingTeleported())
            {
                if (follower->GetVictim()) follower->AttackStop();
                if (follower->IsInCombat()) follower->CombatStop();
                follower->GetMotionMaster()->Clear();
                follower->StopMoving();
                follower->SetPhaseMask(leader->GetPhaseMask(), true);
                follower->TeleportTo(leader->GetMapId(),
                    leader->GetPositionX(), leader->GetPositionY(),
                    leader->GetPositionZ(), leader->GetOrientation());
                LOG_INFO("module",
                    "[WowPsParty Phase] {} mirrored leader phase={} — pulled into the realm",
                    follower->GetName(), leader->GetPhaseMask());
                return true;
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
                    // Drop any committed gather node: after the blink the node is
                    // far behind, and re-walking to it would just re-trip this
                    // teleport. Re-scan fresh at the leader instead.
                    ClearGatherNode(d.followerGuid.GetCounter());
                    follower->TeleportTo(leader->GetMapId(),
                        leader->GetPositionX(), leader->GetPositionY(),
                        leader->GetPositionZ(), leader->GetOrientation());
                    LOG_INFO("module",
                        "[WowPsParty Leash] {} >100y from leader — teleport in",
                        follower->GetName());
                    return true;
                }
                // A bot walking to a gather node may legitimately pass 50y; let it
                // finish rather than reeling it straight back — commit-and-complete
                // keeps the node until it's harvested, so the only thing that ever
                // interrupts the approach is the >100y hard teleport above (which
                // clears the node). A gather can never strand a bot.
                if (leaderDist > 50.0f
                    && !BotIsApproachingGatherNode(d.followerGuid.GetCounter()))
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
            if (follower->IsCharmed())    return true;   // charm is fragile — always yield

            // Mounted transport fly-by: this bot is mounted and the party hasn't
            // dismounted (PartyEngagedDismounted false — see the mount guard), i.e.
            // we're riding past incidental aggro, not committing to a fight. KEEP
            // FOLLOWING the leader rather than yielding to combat AI (which would
            // leave us standing still mounted — the "bots freeze on any aggro" bug).
            // The instant it's a real fight (someone dismounts) the mount guard
            // dismounts us, this flips false, and we yield to AssistTarget as normal.
            bool const mountedFlyby =
                follower->IsMounted()
                && !WowPsParty::PartyEngagedDismounted(follower);
            if (!mountedFlyby)
            {
                if (follower->IsInCombat())   return true;
                if (follower->GetVictim())    return true;

                // (The range-pull TANK ANCHOR lives in AssistTarget now — the
                // map-tick assist loop — so it engages the instant the tank commits
                // to the pull instead of a follow-ticker tick late. During a pull
                // the tank has a victim, so the party-combat guard below yields and
                // AssistTarget owns the anchor; nothing to do here.)

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

            // Actively walking to a mining/herb node — TickGathering owns its
            // motion. Re-asserting MoveFollow here would drag it back to the
            // leader before it reaches the ore.
            if (BotIsApproachingGatherNode(d.followerGuid.GetCounter())) return true;

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

            // Clientless-bot safety net. A party bot that is ALIVE and OUT OF
            // COMBAT must never stay movement-blocked: a human clears a stale
            // root/stun/death-state on revive because the unroot round-trips
            // through their game client, but a bot has none
            // (GetClientControlling() == nullptr), so the block can survive — and
            // the `.revive` GM path (plus mod-playerbots re-touching movement
            // state) RE-APPLIES it for many ticks, which the one-shot dead->alive
            // scrub above misses. A rooted bot can't be driven by MoveFollow, so
            // the catch-up teleport drags it instead — the "healer teleports every
            // few yards after .revive" report. We OWN movement here (past the
            // in-combat yield), so no legitimate CC is active; clear it every tick
            // it's wrong so MoveFollow can walk the bot normally.
            if (!follower->IsInCombat()
                && (follower->HasUnitState(UNIT_STATE_NOT_MOVE)
                    || follower->HasUnitMovementFlag(MOVEMENTFLAG_ROOT)))
                ForceMovableState(follower);

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
            struct StuckSample { float x = 0.0f, y = 0.0f; uint32 idle = 0; uint32 unstickAttempts = 0; };
            static thread_local std::unordered_map<uint32, StuckSample> stuckTracker;
            StuckSample& s = stuckTracker[guidLow];
            float const dxs = follower->GetPositionX() - s.x;
            float const dys = follower->GetPositionY() - s.y;
            float const movedSelf = std::sqrt(dxs * dxs + dys * dys);
            s.x = follower->GetPositionX();
            s.y = follower->GetPositionY();
            // The LEAD TANK is SUPPOSED to be away from the leader — holding its lead spot
            // ahead, or fighting a pull ahead of a hanging-back (e.g. healer) leader. The
            // idle-stuck teleport measures distance to the LEADER, so it kept teleporting a
            // tank that was correctly standing in melee ~18y from a back-line leader ("got
            // teleported to me after the pull", and the formation/oscillation churn). Exempt
            // the lead tank from the idle teleport entirely within a generous 60y; a
            // genuinely lost tank (>60y) still teleports as a backstop. Its real positioning
            // is owned by the isLeadTank MoveFollow + TankLeadEngagement / AssistTarget, and
            // the chase/follow generators re-assert when their generator is lost.
            // Require the LEADER to be stationary for the exemption: a tank holding its
            // lead spot / standing in melee ahead of an idle leader is fine, but if the
            // leader is WALKING and the tank isn't keeping up, that's a genuine stall — let
            // the detector recover it (this closes the "dead follow path within 60y freezes
            // forever" hole: the moment the leader moves, a frozen tank is re-armed).
            bool const leadTankExempt =
                leader->GetMap() && leader->GetMap()->IsDungeon()
                && IsLeadTank(d.followerGuid) && dist < 60.0f && !leader->isMoving();
            if (dist > 8.0f && movedSelf < 1.0f && !leadTankExempt) ++s.idle;
            else { s.idle = 0; s.unstickAttempts = 0; }   // moved / caught up / leading = recovered
            // Genuine long-distance gaps are already handled above (the >50y
            // leash re-walks, >100y snaps), so by here dist <= 50: a "stuck" bot
            // is idle-frozen, NOT far. Two distinct causes, two remedies:
            //   1. A movement-BLOCKING state the bot's missing client never cleared
            //      (post-revive root/stun/death). ForceMovableState + re-asserting
            //      MoveFollow IN PLACE fixes it — the bot walks from where it stands,
            //      no disruptive teleport.
            //   2. The follow generator is already installed (unitState carries
            //      UNIT_STATE_FOLLOW=0x200) but its path computed empty, so it stands
            //      still with NO block at all (moveFlags=0). This is the post-rez
            //      freeze Kevin hit — two bots auto-revived on a boss corpse, then
            //      "refused to move" through the next pull and wiped the party.
            //      Re-asserting the SAME MoveFollow just re-installs the same stalled
            //      generator and it never moves (the logs show unstick-in-place firing
            //      over and over on the same bots, unitState=0x200 moveFlags=0, never
            //      recovering). The only thing that fixes a dead follow PATH is a fresh
            //      pathable position, so escalate to a teleport onto the leader.
            // We can't tell the two apart up front (both look idle), so try the gentle
            // in-place fix ONCE; if the bot is STILL stuck after it, the block wasn't
            // the problem — teleport. A bot whose root the in-place fix cleared starts
            // moving and resets unstickAttempts before ever reaching the teleport.
            bool const stuckClose = s.idle >= 3;   // not moving for 3 ticks while >8y out
            if (stuckClose)
            {
                if (follower->IsBeingTeleported())  return true;
                if (follower->IsNonMeleeSpellCast(false, false, true)) return true;

                uint32 const ustate = follower->GetUnitState();
                ForceMovableState(follower);

                if (s.unstickAttempts == 0)
                {
                    follower->StopMoving();
                    follower->GetMotionMaster()->Clear();
                    follower->GetMotionMaster()->MoveFollow(
                        leader, PET_FOLLOW_DIST, follower->GetFollowAngle());
                    LOG_INFO("module",
                        "[WowPsParty Follow] unstick-in-place: {} dist={:.1f} idle={} "
                        "unitState={:#x} moveFlags={:#x} runSpeed={:.2f}",
                        follower->GetName(), dist, s.idle, ustate,
                        follower->GetUnitMovementFlags(), follower->GetSpeed(MOVE_RUN));
                }
                else
                {
                    follower->GetMotionMaster()->Clear();
                    follower->StopMoving();
                    follower->TeleportTo(leader->GetMapId(),
                        leader->GetPositionX(), leader->GetPositionY(),
                        leader->GetPositionZ(), leader->GetOrientation());
                    LOG_INFO("module",
                        "[WowPsParty Follow] unstick-teleport: {} dist={:.1f} (in-place "
                        "unstick didn't take — follow path stalled, unitState={:#x})",
                        follower->GetName(), dist, ustate);
                }

                // Preserve the escalation counter across the sample reset so the next
                // stuck detection knows the in-place fix already failed.
                uint32 const keepAttempts = s.unstickAttempts + 1;
                s = StuckSample{};
                s.unstickAttempts = keepAttempts;
                s.x = follower->GetPositionX();
                s.y = follower->GetPositionY();
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
            // FORM_ANGLES (rear-arc fan-out bearings, indexed by formation
            // ordinal) is defined at namespace scope above so the pull anchor
            // shares it. Extra companions beyond the table wrap to an outer ring.
            bool const inDungeon = leader->GetMap() && leader->GetMap()->IsDungeon();
            bool const isLeadTank = inDungeon && IsLeadTank(d.followerGuid);

            // If the bot was sitting (post-drink), stand up before moving
            // so the spline doesn't fight the seated stand-state.
            if (follower->getStandState() != UNIT_STAND_STATE_STAND)
                follower->SetStandState(UNIT_STAND_STATE_STAND);

            // The LEAD TANK body-pulls from a precise spot ahead of the leader —
            // never jittered/wandered.
            if (isLeadTank)
            {
                // Mid pull / body-pull gather: TankLeadEngagement / TankGatherStep own the
                // tank's feet (DriveTankChase walks it into the pack by proximity). The lead-
                // ahead MoveFollow below re-Clears that chase every 1 Hz tick and re-pins the
                // tank ~leadDist in front of the (back-line) leader, so it never advances on
                // the pack — the "tank won't body-pull, stands still while the MOB walks to
                // it, engaged stuck at 1/N -> drive-timeout" report (Mill: Amaenna, a wing
                // with no recorded path so the path-follow ticker doesn't cover it either).
                // Yield the feet entirely and clear humanize eligibility so the 250 ms tick
                // can't grab it; the gather drive re-asserts its own chase generator.
                if (TankGatherActive(d.followerGuid.GetCounter())
                    || IsTankPulling(d.followerGuid))
                {
                    g_humanize[d.followerGuid.GetCounter()].eligible = false;
                    return true;
                }

                // The lead tank is driven by TankFollowPath (recorded wing) or the
                // lead-ahead MoveFollow below — NEVER by the humanize formation tick.
                // Explicitly clear any stale eligibility: a tank that was a normal
                // follower (e.g. out in the open world) had eligible=true set, and
                // becoming the lead tank here never reset it. In a wing with NO recorded
                // path, TankFollowPath bails WITHOUT MarkTankLeading, so HumanizeTick's
                // IsTankLeading guard doesn't skip it either — and the 250ms humanize
                // keeps yanking the tank to its REAR formation slot while this 1 Hz block
                // pushes it to the FRONT lead spot. The tank then ping-pongs between the
                // two while the leader stands still (Mill's idle oscillation).
                g_humanize[d.followerGuid.GetCounter()].eligible = false;

                // If THIS WING has a recorded path, the path-follow ticker drives
                // the tank's motion — skip MoveFollow so the two don't fight. Must
                // be leader-WING-aware (not just "any path on the map"): in an
                // un-recorded wing of a multi-wing dungeon the path ticker bails, so
                // MoveFollow has to run or the tank is stranded.
                if (WowPsParty::HasPathForLeader(leader->GetMapId(), leader))
                    return true;

                // MoveFollow's angle is relative to the leader's facing: 0 =
                // directly in front, M_PI = directly behind. The lead tank must
                // be IN FRONT (the old M_PI put it 12y behind — the "tank trails
                // far behind" bug). A few yards ahead so it body-pulls. Install the
                // follow ONCE and let MoveFollow's heartbeat maintain it — re-Clearing
                // + re-MoveFollow every 1 Hz tick resets the spline each second (the
                // jitter the humanize path deliberately avoids); only (re)assert when
                // the generator isn't already FOLLOW or the slider distance changed.
                float const leadDist = float(WowPsParty::BotLeadDistance(d.followerGuid));
                MovementGeneratorType const lmg =
                    follower->GetMotionMaster()->GetCurrentMovementGeneratorType();
                static thread_local std::unordered_map<uint32, std::pair<uint32, float>> leadAssert;
                auto& la = leadAssert[d.followerGuid.GetCounter()];
                bool const leadChanged = la.first != leader->GetGUID().GetCounter()
                                      || std::fabs(la.second - leadDist) > 0.01f;
                // Reassert too when the tank is sitting RIGHT ON the leader (< 4y): another
                // follow path (a catch-up teleport-onto-leader, or a rear pet-distance
                // MoveFollow) can leave it stacked on the leader with a FOLLOW generator the
                // gen+leadChanged dedup wouldn't notice — it'd trail behind instead of
                // leading. An ABSOLUTE small threshold (below the 5y minimum lead distance)
                // so it only catches that stacked case, NOT the normal walk out to a distant
                // lead spot (which would otherwise re-issue every tick and stutter).
                bool const tooClose = follower->GetDistance(leader) < 4.0f;
                if (lmg != FOLLOW_MOTION_TYPE || leadChanged || tooClose)
                {
                    follower->GetMotionMaster()->Clear();
                    follower->GetMotionMaster()->MoveFollow(leader, leadDist, 0.0f);
                    la.first  = leader->GetGUID().GetCounter();
                    la.second = leadDist;
                }
                return true;
            }

            // Normal follower: hand off to the humanize tick. Compute this bot's
            // jittered formation slot (stable personality offset + a slow drift
            // that re-rolls every several seconds), record it, and mark eligible
            // — the 250 ms tick installs/maintains the follow, staggers the
            // peel-off, and adds idle wander. We do NOT install MoveFollow here.
            int const fi = FormationIndexFor(d.followerGuid, d.leaderGuid);
            uint32 const gLow = d.followerGuid.GetCounter();
            uint32 const nowMs = getMSTime();
            FollowHumanize& h = g_humanize[gLow];

            if (h.driftRerollMs == 0)
                h.driftRerollMs = nowMs + urand(3000, 9000);
            if (nowMs >= h.driftRerollMs)
            {
                h.driftAngle    = frand(-0.16f, 0.16f);
                h.driftDist     = frand(-1.0f, 1.0f);
                h.driftRerollMs = nowMs + urand(8000, 15000);
            }

            float const baseAngle = FORM_ANGLES[fi % 6];
            float const baseDist  = PET_FOLLOW_DIST + float(fi / 6) * 2.5f;
            float slot = baseAngle + StableJitter(gLow, 1, 0.30f) + h.driftAngle;
            float sdist = baseDist + StableJitter(gLow, 2, 1.8f) + h.driftDist;
            if (sdist < PET_FOLLOW_DIST) sdist = PET_FOLLOW_DIST;

            h.slotAngle = slot;
            h.slotDist  = sdist;
            h.eligible  = true;

            static thread_local std::unordered_map<uint32, uint32> lastLogMs;
            uint32& last = lastLogMs[gLow];
            if (nowMs - last > 10000)
            {
                last = nowMs;
                LOG_INFO("module",
                    "[WowPsParty Follow] tick: {} -> humanized follow {} dist={:.1f}",
                    follower->GetName(), leader->GetName(), dist);
            }
            return true;
        }

        // Fast (250 ms) humanize tick — see the FollowHumanize comment above.
        // Drives the actual follow-install for bots the 1 Hz pass flagged as
        // pure open-follow, with a per-bot staggered peel-off and short-leash
        // idle wander. Re-checks the cheap critical guards live so it never
        // fights AssistTarget / a hold / a pull.
        void HumanizeTick()
        {
            std::vector<Directive> snapshot;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                snapshot = g_directives;
            }

            uint32 const now = getMSTime();
            for (Directive const& d : snapshot)
            {
                uint32 const gLow = d.followerGuid.GetCounter();
                auto it = g_humanize.find(gLow);
                if (it == g_humanize.end() || !it->second.eligible) continue;
                FollowHumanize& h = it->second;

                Player* follower = ObjectAccessor::FindConnectedPlayer(d.followerGuid);
                Player* leader   = ObjectAccessor::FindConnectedPlayer(d.leaderGuid);
                if (!follower || !leader || !follower->IsInWorld()
                    || !leader->IsInWorld() || follower == leader)
                { h.wandering = false; continue; }

                // Anchor-on-tank: a non-tank with the toggle ON formation-follows the party
                // TANK instead of the leader, so melee reach the front fast when the leader is
                // ranged. PartyTankPlayer returns the leader itself when the leader IS the tank,
                // so tank==leader naturally means "no change". Non-combat positioning only.
                Player* anchor = leader;
                if (BotAnchorOnTank(d.followerGuid) && !IsLeadTank(d.followerGuid))
                    if (Player* tank = PartyTankPlayer(follower, leader))
                        if (tank != leader && tank != follower && tank->IsInWorld()
                            && tank->IsAlive() && tank->GetMapId() == follower->GetMapId())
                            anchor = tank;

                // Live guard re-check — bail (yield to whoever owns it) the instant
                // this bot is anything but a calm open-follower.
                if (!follower->IsAlive()
                    || follower->GetMapId() != leader->GetMapId()
                    || follower->IsInCombat() || follower->GetVictim()
                    || follower->IsNonMeleeSpellCast(false, false, true)
                    || follower->IsBeingTeleported()
                    || IsFollowerHeld(d.followerGuid)
                    || IsTankLeading(d.followerGuid)
                    || BotIsApproachingGatherNode(gLow)   // mining/herbing — don't yank it off the node
                    || AnyPartyMemberEngaged(d.followerGuid, follower))
                { h.wandering = false; continue; }

                MovementGeneratorType const mg =
                    follower->GetMotionMaster()->GetCurrentMovementGeneratorType();
                bool const leaderMoving = anchor->isMoving();

                // Re-assert the formation slot ONLY when it isn't already in
                // effect (gen isn't FOLLOW, or the jittered slot drifted) — so
                // MoveFollow runs continuously and smoothly (with its built-in
                // heartbeat prediction) instead of being re-issued every tick.
                // Re-issuing MoveFollow Clears + reinstalls the generator, which
                // resets the spline — doing that on every steering nudge is what
                // made the bots jitter/glitch.
                auto moveToFormation = [&]()
                {
                    bool const slotChanged =
                        std::fabs(h.slotAngle - h.apptAngle) > 0.03f ||
                        std::fabs(h.slotDist  - h.apptDist)  > 0.20f;
                    if (mg == FOLLOW_MOTION_TYPE && h.followAsserted && !slotChanged)
                        return;
                    follower->GetMotionMaster()->Clear();
                    follower->GetMotionMaster()->MoveFollow(anchor, h.slotDist, h.slotAngle);
                    h.followAsserted = true;
                    h.apptAngle = h.slotAngle;
                    h.apptDist  = h.slotDist;
                };

                // ---- staggered peel-off: arm a per-bot delay on the stop->move edge
                if (leaderMoving && !h.wasLeaderMoving)
                {
                    int const fi = FormationIndexFor(d.followerGuid, d.leaderGuid);
                    h.reactAtMs = now + StaggerDelayMs(gLow, fi);
                }
                h.wasLeaderMoving = leaderMoving;

                if (leaderMoving)
                {
                    h.wandering = false;
                    if (now < h.reactAtMs)
                    {
                        // Not our turn to peel off yet — hold a beat in place so
                        // the party doesn't lurch as one block.
                        if (mg != IDLE_MOTION_TYPE)
                        {
                            follower->StopMoving();
                            follower->GetMotionMaster()->Clear();
                            follower->GetMotionMaster()->MoveIdle();
                            h.followAsserted = false;
                        }
                        continue;
                    }
                    moveToFormation();
                    continue;
                }

                // ---- leader stationary --------------------------------------
                if (h.wandering)
                {
                    if (now >= h.wanderUntilMs) h.wandering = false;  // stroll done -> hold here
                    else continue;                                    // let the stroll finish
                }

                float const wanderCap = std::min(h.slotDist + 6.0f, 7.0f);
                float const dLead     = follower->GetDistance(anchor);

                // Reform to the slot when NOTABLY out of formation — either too FAR (just
                // left combat away from the party; threshold ABOVE the wander cap so a bot
                // that merely strolled out keeps its spot) OR STACKED on the anchor (closer
                // than its slot). Without the too-close case a bot that ended up on the
                // leader's pixel just "settles in place" there and fidgets back and forth on
                // it forever instead of spreading out (Mill: "2 dps walk back and forth on
                // the exact pixel I'm standing on").
                if (dLead > wanderCap + 0.5f || dLead < h.slotDist - 1.0f)
                {
                    moveToFormation();
                    continue;
                }

                // Settle in place. Once the leader stops, drop the follow move
                // so the bot HOLDS its current position rather than being tugged
                // back to its formation slot — this is what lets it stay wherever
                // it last wandered to and pick the next stroll from there. The
                // formation re-forms when the leader moves again (moving branch).
                if (mg != IDLE_MOTION_TYPE || follower->isMoving())
                {
                    follower->StopMoving();
                    follower->GetMotionMaster()->Clear();
                    follower->GetMotionMaster()->MoveIdle();
                    h.followAsserted = false;
                }

                // Occasionally take a short stroll from the CURRENT spot (not a
                // return to formation) so the party keeps shifting its weight
                // instead of freezing. Frequency is ~half the first cut. The
                // direction is weighted back toward the anchor the further out
                // we are, so repeated strolls can't creep into the leash.
                if (now >= h.nextFidgetMs)
                {
                    h.nextFidgetMs = now + urand(3000, 7000);
                    if (urand(0, 99) < 22)
                    {
                        float wBack = (dLead - h.slotDist)
                                    / std::max(1.0f, wanderCap - h.slotDist);
                        wBack = wBack < 0.0f ? 0.0f : (wBack > 1.0f ? 1.0f : wBack);
                        float const randDir = frand(0.0f, 2.0f * float(M_PI));
                        float const dir = LerpAngle(randDir, follower->GetAngle(anchor), wBack);
                        float const r = frand(1.5f, 3.5f);
                        float wx = follower->GetPositionX() + r * std::cos(dir);
                        float wy = follower->GetPositionY() + r * std::sin(dir);
                        float wz = follower->GetPositionZ();
                        // Hard cap: never settle past the wander leash, so a
                        // wandering bot can't sit in the 1 Hz stuck-detector's
                        // >8y window (needless unstick on a big party).
                        if (follower->GetMap()
                            && follower->GetMap()->CanReachPositionAndGetValidCoords(follower, wx, wy, wz)
                            && anchor->GetExactDist2d(wx, wy) < wanderCap)
                        {
                            follower->GetMotionMaster()->Clear();
                            follower->GetMotionMaster()->MovePoint(WANDER_POINT_ID, wx, wy, wz);
                            h.followAsserted = false;
                            h.wandering      = true;
                            h.wanderUntilMs  = now + urand(1500, 3000);
                            continue;
                        }
                    }
                }

                // Otherwise just hold the current spot (already idle) — no
                // re-anchor to the formation slot.
            }
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

            // Fast humanize pass: drives the per-bot jittered follow, staggered
            // peel-off, and idle wander for bots the 1 Hz pass flagged eligible.
            // Runs between the heavy 1 Hz directive ticks so reactions desync at
            // sub-second resolution instead of lockstep.
            _humanizeAccum += diff;
            if (_humanizeAccum >= HUMANIZE_INTERVAL_MS)
            {
                _humanizeAccum = 0;
                HumanizeTick();
            }

            _accum += diff;
            if (_accum < TICK_INTERVAL_MS) return;
            _accum = 0;

            std::vector<Directive> snapshot;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                snapshot = g_directives;
            }

            // Leader-fall detection: sample each distinct leader's Z ONCE this
            // pass. A drop > FALL_DROP_Z (with only a small XY move, so a teleport
            // can't masquerade as a fall) arms a teleport FALL_LAND_GRACE_MS out;
            // re-arming while the leader keeps falling means it fires that long
            // after it actually lands. ApplyDirective consumes the arm below.
            {
                uint32 const now = getMSTime();
                std::unordered_set<uint32> seenLeaders;
                for (auto const& d : snapshot)
                {
                    uint32 const llow = d.leaderGuid.GetCounter();
                    if (!seenLeaders.insert(llow).second) continue;   // once per leader
                    Player* leader = ObjectAccessor::FindConnectedPlayer(d.leaderGuid);
                    if (!leader || !leader->IsInWorld()) continue;
                    LeaderFallState& s = g_leaderFall[llow];
                    float const x = leader->GetPositionX();
                    float const y = leader->GetPositionY();
                    float const z = leader->GetPositionZ();
                    uint32 const mapId = leader->GetMapId();
                    // A map change or an in-flight teleport makes the deltas
                    // meaningless — re-baseline and don't arm.
                    if (s.lastMapId != mapId || leader->IsBeingTeleported())
                    {
                        s.lastX = x; s.lastY = y; s.lastZ = z; s.lastMapId = mapId;
                        continue;
                    }
                    float const drop = s.lastZ - z;   // positive = fell
                    float const dx = x - s.lastX, dy = y - s.lastY;
                    float const horiz = std::sqrt(dx * dx + dy * dy);
                    s.lastX = x; s.lastY = y; s.lastZ = z;
                    // A fall is a big Z loss with only a small horizontal move; a
                    // same-map teleport/blink jumps far in XY too, so the horiz gate
                    // keeps it from masquerading as a fall.
                    if (drop > FALL_DROP_Z && horiz < FALL_MAX_XY)
                        s.teleportDueMs = now + FALL_LAND_GRACE_MS;
                }
                // GC leaders that vanished from the directive set this pass.
                for (auto it = g_leaderFall.begin(); it != g_leaderFall.end(); )
                {
                    if (seenLeaders.count(it->first)) ++it;
                    else it = g_leaderFall.erase(it);
                }
            }

            for (auto const& d : snapshot)
                (void)ApplyDirective(d);

            // Disarm any fall-teleport whose window came due this pass — every
            // follower has now had its chance to snap in ApplyDirective above, so
            // only a NEW drop re-arms it. (Re-running the snap every pass would
            // keep yanking a bot that legitimately walked back to the leader.)
            {
                uint32 const now = getMSTime();
                for (auto& kv : g_leaderFall)
                    if (kv.second.teleportDueMs != 0 && now >= kv.second.teleportDueMs)
                        kv.second.teleportDueMs = 0;
            }
        }

    private:
        uint32 _accum = 0;
        uint32 _humanizeAccum = 0;
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

// Trampoline: is this player AI-controlled (a hero alt OR a henchman)? Core's
// Player::DurabilityPointsLoss calls this to give every bot infinite gear
// durability — only the human-played body (no PlayerbotAI) ever wears down, so
// the party never has to repair the heroes/henchmen, only their own characters.
bool WowPsParty_PlayerHasBotAI_Trampoline(Player* player)
{
    return player && sPlayerbotsMgr.GetPlayerbotAI(player) != nullptr;
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

// Manual pull-more trampoline — must be dispatched LAST in the party-bot AI tick
// so its MoveChase overrides rotation/AssistTarget while the window is armed.
void WowPsParty_TickTankPullMore_Trampoline(Player* bot)
{
    WowPsParty::TickTankPullMore(bot);
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

// Gathering trampoline — out-of-combat mining/herbalism/skinning for the player's alts.
void WowPsParty_TickGathering_Trampoline(Player* bot)
{
    WowPsParty::TickGathering(bot);
}

// Henchman corpse-loot trampoline — out-of-combat looting for hired henchmen
// only (self-gated; no-op for alts and the human).
void WowPsParty_TickHenchmanLoot_Trampoline(Player* bot)
{
    WowPsParty::TickHenchmanLoot(bot);
}

// Battleground-invite auto-accept trampoline — ports a managed party bot into a
// BG the human queued, since the bot's gated AI never clicks "Enter Battle".
void WowPsParty_TickAcceptBgInvite_Trampoline(Player* bot)
{
    WowPsParty::TickAcceptBgInvite(bot);
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

        // The role check must reflect the CURRENT roster, so read the
        // authoritative account_party.role straight from the DB for enrolled
        // alts (the in-memory directive can lag a just-made roster change). A
        // henchman has no account_party row — fall back to its directive role.
        std::string role;
        if (QueryResult q = CharacterDatabase.Query(
                "SELECT `role` FROM `account_party` WHERE `guid` = {}", g.GetCounter()))
            role = q->Fetch()[0].Get<std::string>();
        if (role.empty())
            role = WowPsParty::RoleForGuid(g);
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
