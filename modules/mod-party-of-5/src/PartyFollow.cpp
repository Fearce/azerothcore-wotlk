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
#include "ThreatManager.h"   // threat-cap throttle: bots back off near the tank's threat
#include "CombatManager.h"   // tank gather window: count mobs in combat with the tank
#include "SpellInfo.h"   // SpellInfo::Effects[] for the leader's mount-type check

// Gathering (mining / herbalism) for follower bots.
#include "Bag.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Creature.h"
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
            if (loot.gold > 0) return true;
            if (loot.roundRobinPlayer != self) return false;   // trash assigned to another member
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

        static std::mutex                g_mutex;
        static std::vector<Directive>    g_directives;
        // account -> the ACTIVE leader's own account_party role. The leader isn't a
        // follower, so it has no Directive (RoleForGuid can't see it); captured in
        // SetActiveFollowers so the human-tank wait-gate can read the leader's role.
        static std::unordered_map<uint32, std::string> g_leaderRole;
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
    // until the whole set is gathered — giving the tank time to group them. Only an
    // INITIAL pull triggers it (TankLeadEngagement already requires the whole party out
    // of combat + rested AND the tank to have no live victim), so it never chain-pulls
    // the dungeon: the party still eats/drinks between packs.
    struct TankGatherState
    {
        uint32                  untilMs = 0;   // hard cap on the gather hold
        std::vector<ObjectGuid> set;           // the mobs this opener means to gather
    };
    static std::unordered_map<uint32, TankGatherState> g_tankGather;   // tankLow -> state

    static constexpr uint32 TANK_GATHER_MAX_MS = 6000;   // cap the party hold for a gather
    static constexpr float  PULL_GATHER_RANGE  = 30.0f;  // scan radius for the pull pool
    static constexpr float  MOB_CLUSTER_R      = 10.0f;  // mobs within this aggro/stack as one
    static constexpr float  PULL_Z_TOLERANCE   = 6.0f;   // "similar Z-level" as the tank
    static constexpr float  GATHER_STACK_R     = 9.0f;   // a set mob this near the tank = gathered

    static void MarkTankGathering(uint32 tankLow, std::vector<ObjectGuid> set)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        uint32 const now = getMSTime();
        // Opportunistic prune: a solo tank's gather entry is never read by
        // IsPartyPullPending (no other members), so sweep expired entries here so the
        // map can't slowly accumulate stale tank guids.
        for (auto it = g_tankGather.begin(); it != g_tankGather.end(); )
            it = (it->first != tankLow && now >= it->second.untilMs)
                     ? g_tankGather.erase(it) : std::next(it);
        TankGatherState& s = g_tankGather[tankLow];
        s.untilMs = now + TANK_GATHER_MAX_MS;
        s.set     = std::move(set);
    }

    // True while the tank is still gathering its multi-pull: the cap hasn't lapsed AND
    // at least one alive set member hasn't stacked on the tank yet. Self-clears (returns
    // false) at the cap so a mob that won't path can't freeze the party forever.
    static bool IsTankGathering(Player* tank)
    {
        if (!tank) return false;
        std::vector<ObjectGuid> set;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_tankGather.find(tank->GetGUID().GetCounter());
            if (it == g_tankGather.end()) return false;
            if (getMSTime() >= it->second.untilMs) { g_tankGather.erase(it); return false; }
            set = it->second.set;
        }
        for (ObjectGuid const& g : set)
        {
            Creature* m = ObjectAccessor::GetCreature(*tank, g);
            if (!m || !m->IsAlive()) continue;                        // dead/gone -> gathered
            if (tank->GetDistance(m) > GATHER_STACK_R) return true;   // still inbound -> hold
        }
        return false;   // every alive member is stacked on the tank -> done
    }

    // Build the INITIAL multi-pull set: the connected proximity-cluster (edges within
    // MOB_CLUSTER_R) that contains `anchor`, among UN-AGGROED hostiles near the tank
    // that the tank can SEE (LoS) and that sit on a similar Z-level. Returns the most
    // central member to body-pull (running to it sweeps the cluster's aggro radius) and
    // fills `outSet`, ONLY when the cluster size is in [2, N] — it fills the pull target
    // without overshooting. Returns nullptr (caller does a normal single pull) when the
    // anchor is isolated (<2) OR its cluster is bigger than N (don't body-pull a whole
    // big pack — the careful single pull is safer). This is the cluster-awareness: a mob
    // that would drag the pack past N is simply never made into a multi-pull.
    static Unit* SelectInitialPullCluster(Player* tank, Unit* anchor, uint32 N,
                                          std::vector<ObjectGuid>& outSet)
    {
        outSet.clear();
        if (!tank || !anchor || N < 2) return nullptr;

        std::list<Unit*> nearby;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(tank, tank, PULL_GATHER_RANGE);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck>
            searcher(tank, nearby, check);
        Cell::VisitObjects(tank, searcher, PULL_GATHER_RANGE);

        auto eligible = [&](Unit* u) -> bool
        {
            if (!u || !u->IsAlive() || u->IsInCombat()) return false;   // un-aggroed only
            if (u->IsTotem()) return false;
            if (!tank->IsValidAttackTarget(u)) return false;
            if (std::fabs(u->GetPositionZ() - tank->GetPositionZ()) > PULL_Z_TOLERANCE) return false;
            return tank->IsWithinLOSInMap(u, VMAP::ModelIgnoreFlags::M2);
        };
        // The anchor must itself be eligible to seed a clean cluster; if the tank can't
        // even see it / it's already fighting, fall back to a normal single pull.
        if (!eligible(anchor)) return nullptr;

        std::vector<Unit*> elig{ anchor };
        for (Unit* u : nearby)
            if (u != anchor && eligible(u)) elig.push_back(u);
        if (elig.size() < 2) return nullptr;

        // Connected component of `anchor` (proximity graph, edge <= MOB_CLUSTER_R). BFS
        // capped at N+1 — the moment it grows past N we know it's an overshoot pack.
        std::vector<Unit*> comp{ elig[0] };
        std::vector<bool>  used(elig.size(), false);
        used[0] = true;
        for (size_t head = 0; head < comp.size() && comp.size() <= N; ++head)
            for (size_t i = 0; i < elig.size(); ++i)
                if (!used[i] && comp[head]->GetDistance(elig[i]) <= MOB_CLUSTER_R)
                { used[i] = true; comp.push_back(elig[i]); }
        if (comp.size() < 2 || comp.size() > N) return nullptr;   // isolated or overshoot

        // Body-pull from the most central member (most neighbours within MOB_CLUSTER_R).
        Unit* seed = comp[0];
        uint32 bestDeg = 0;
        for (Unit* a : comp)
        {
            uint32 deg = 0;
            for (Unit* b : comp) if (a != b && a->GetDistance(b) <= MOB_CLUSTER_R) ++deg;
            if (deg > bestDeg) { bestDeg = deg; seed = a; }
        }
        for (Unit* u : comp) outSet.push_back(u->GetGUID());
        return seed;
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
    // pull) or '0' (barge). Unlike wait_tank_threat there's no per-type split:
    // the safe pull is the long-standing default for EVERY tank, so an absent
    // entry means ON; the toggle only lets a tank opt OUT.
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
        std::lock_guard<std::mutex> lock(g_safePullMutex);
        auto it = g_safePull.find(guid.GetCounter());
        if (it != g_safePull.end()) return it->second != 0;
        return true;   // unset -> safe pull ON (the long-standing default)
    }

    void SafePullRefreshFromDB(uint32 guidLow)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `safe_pull` FROM `party_loadout` WHERE `guid` = {}", guidLow);
        std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
        SafePullCacheSet(guidLow, v == "1" ? 1 : (v == "0" ? 0 : -1));
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

        // Find the nearest hostile to the leader within 28y of them. (Was 40y —
        // toned down ~30% so the tank doesn't go for a range-pull on a mob that's
        // still quite far; it waits until the party is closer to the pack.)
        // SelectNearbyTarget(exclude, dist) returns the nearest unit that
        // `this` considers a valid attack target — perfect for "what's
        // about to fight us".
        Unit* nearest = leader->SelectNearbyTarget(nullptr, 28.0f);
        if (!nearest || !nearest->IsAlive()) return;
        if (!bot->IsValidAttackTarget(nearest)) return;

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
                    uint64(manaUnit->GetPower(POWER_MANA)) * 100 < uint64(maxMana) * 99)
                {
                    static thread_local std::unordered_map<uint32, uint32> manaLogMs;
                    uint32 const now = getMSTime();
                    uint32& ml = manaLogMs[bot->GetGUID().GetCounter()];
                    if (now - ml > 5000)
                    {
                        ml = now;
                        LOG_INFO("module",
                            "[WowPsParty TankLead] guid={} holding next pull — {} mana {}/{} (<99%)",
                            bot->GetGUID().GetCounter(),
                            (bot->getClass() == CLASS_PALADIN) ? "own" : "healer",
                            manaUnit->GetPower(POWER_MANA), maxMana);
                    }
                    return;
                }
            }
        }

        bool const ok = bot->Attack(nearest, true);
        if (!ok)
        {
            // We CAN'T actually attack this mob even though it passed
            // IsValidAttackTarget — Unit::Attack rejects an EVADING creature (it
            // aggroed but couldn't path to us across the corner/wall, so it reset),
            // and a few flag/event states. Critically, the old code ignored this
            // `ok` and MoveChased / MarkTankPulling'd anyway, locking the tank onto
            // a mob it can NEVER engage and freezing the whole party holding fire —
            // the "party waited around the corner instead of moving into LoS"
            // report (an Utgarde Dragonflayer Forge Master evading at ~3y). Bail:
            // the follow system walks the tank with the leader, and the instant the
            // mob is attackable again (reset finished / now in reach) the lead
            // resumes and pulls it. Re-armed each tick, so it self-corrects.
            LOG_INFO("module",
                "[WowPsParty TankLead] guid={} CANT-ATTACK mob_guid={} entry={} dist={:.1f} "
                "(evading/immune) — bailing, NOT holding the party",
                bot->GetGUID().GetCounter(), nearest->GetGUID().GetCounter(),
                nearest->GetEntry(), bot->GetDistance(nearest));
            return;
        }
        bot->SetFacingToObject(nearest);

        // Multi-pull (pull_count:N, default 3): on this INITIAL pull, try to open on a
        // whole CLUSTER instead of one mob. If the nearest mob seeds a tight cluster of
        // 2..N eligible (un-aggroed, in the tank's LoS, similar Z) mobs, BODY-PULL it —
        // run into the cluster so the pack aggros and stacks on the tank — mark the
        // gather (the party holds fire until they're stacked, via IsPartyPullPending),
        // and skip the single-mob ranged-isolate flow below. A bigger-than-N pack or an
        // isolated mob returns null and falls through to the normal single pull. Gated
        // to a real initial pull by the no-live-victim + party-rested checks above, so
        // it never chain-pulls; the party eats/drinks between packs as before.
        // INTENTIONAL precedence over safe_pull: a found 2..N cluster is body-pulled even
        // when safe_pull is ON (it's the whole point of pull_count). A LONE mob still
        // falls through to the safe ranged pull below, so safe_pull keeps governing the
        // common single-target case; set pull_count:1 to force single pulls everywhere.
        uint32 const pullN = WowPsParty::BotInitialPullCount(bot->GetGUID());
        if (pullN >= 2)
        {
            std::vector<ObjectGuid> set;
            if (Unit* seed = SelectInitialPullCluster(bot, nearest, pullN, set))
            {
                // The seed may differ from `nearest`; (re)attack it. If it's evading or
                // immune, Unit::Attack rejects it — abandon the multi-pull and fall
                // through to the single pull on `nearest` (already attacked above), so we
                // never charge + hold the party on a mob we can't actually engage
                // (mirrors the single-pull CANT-ATTACK bail).
                if (bot->GetVictim() == seed || bot->Attack(seed, true))
                {
                    MarkTankGathering(bot->GetGUID().GetCounter(), set);
                    bot->SetFacingToObject(seed);
                    bot->GetMotionMaster()->MoveChase(seed);   // body-pull into the cluster
                    LOG_INFO("module",
                        "[WowPsParty TankLead] guid={} MULTI-PULL {}/{} mobs seed_guid={} entry={} dist={:.1f}",
                        bot->GetGUID().GetCounter(), uint32(set.size()), pullN,
                        seed->GetGUID().GetCounter(), seed->GetEntry(), bot->GetDistance(seed));
                    return;
                }
            }
        }

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
            bot->GetMotionMaster()->MoveChase(nearest);              // melee: close in

        LOG_INFO("module", "[WowPsParty TankLead] guid={} {} mob_guid={} entry={} dist={:.1f} ok={}",
                 bot->GetGUID().GetCounter(), canRangedPull ? "RANGE-PULL" : "PULL",
                 nearest->GetGUID().GetCounter(), nearest->GetEntry(), dist, ok);
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
        // Ready if the engine already flagged it, OR it's a genuine party kill we
        // can force ready at harvest time (our bots leave corpse loot unfinished,
        // so the flag is usually never set — see WowPsParty_ForceSkinReady).
        if (c->HasUnitFlag(UNIT_FLAG_SKINNABLE)) return true;
        return c->GetLootRecipient() || c->GetLootRecipientGroup();
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
                InventoryResult msg = EQUIP_ERR_OK;
                hench->StoreLootItem(uint8(i), &c->loot, msg);
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
        else
        {
            // Walk to the corpse; keep the 1Hz follow re-asserter and the 250ms
            // humanize tick off us (HoldFollower + the committed-node yield) so
            // neither yanks us back to the leader mid-approach.
            HoldFollower(bot->GetGUID(), 2500);
            bot->SetFacingToObject(target);
            bot->GetMotionMaster()->MovePoint(0xA17,
                target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
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

        // Multi-pull gather: hold until the whole cluster is stacked on the tank
        // (bounded by TANK_GATHER_MAX_MS), so the tank can group the pack before DPS
        // threat splits it across the not-yet-gathered mobs. Self-clears at the cap.
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
                        // 7+ followers (party-of-5 + henchmen) wrap the 6-bearing
                        // table; push each extra ring 2.5y further back so they
                        // don't stack on the inner ring (mirrors the leader-follow).
                        float const followDist = held + float(fi / 6) * 2.5f;
                        if (bot->getStandState() != UNIT_STAND_STATE_STAND)
                            bot->SetStandState(UNIT_STAND_STATE_STAND);
                        bot->GetMotionMaster()->Clear();
                        bot->GetMotionMaster()->MoveFollow(tank, followDist, PULL_REAR_ANGLES[fi % 6]);
                    }
                }
                else if (mg == CHASE_MOTION_TYPE)
                    bot->GetMotionMaster()->Clear();   // no tank found — just hold fire
                AssistLog(gLow, "pull pending: anchoring behind the tank, holding fire");
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
            // ALLY makes AssistTarget MELEE-CHASE it, and that chase is uncapped:
            // at 30y a tank sprinted across the room to a far peel and body-pulled
            // every pack on the way ("chain-pulls until we die"). 18y still covers
            // adds on a healer/caster positioned behind the tank; farther adds are
            // grabbed with a ranged taunt rule (cast_loose_enemy:Growl), not a run.
            static constexpr float PARTY_DEFEND_RANGE = 18.0f;
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
                // The leader isn't attacking — only DEFEND (a mob already swinging
                // at the bot or an ally). We deliberately do NOT engage the mob the
                // leader has merely SELECTED: a left-click is just targeting, and
                // pulling on it broke following / dragged the party into combat
                // (Kevin: "the party should not stop following just because the
                // tank selects an enemy"). pickPartyDefenseTarget is combat-only,
                // so a mere selection now produces NO target -> the bot keeps
                // following until the leader actually engages (attacks/casts, which
                // puts the mob in combat and the assist/defense paths pick it up).
                desired = pickPartyDefenseTarget();
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
            if (gated && HumanTankLeadActive(bot, leader)
                && WaitForHumanTank(bot->GetGUID())            // default WAIT under a human tank; '0' opts out
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
            // resume movement.
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
                if (mg != POINT_MOTION_TYPE)
                {
                    float lx, ly, lz;
                    desired->GetNearPoint(bot, lx, ly, lz, 0.0f,
                                          std::min(hold, 10.0f), desired->GetAngle(bot));
                    // generatePath rounds the corner / climbs the stairs;
                    // forceDestination=false so an unreachable spot just isn't
                    // taken (no straight-line dive through geometry).
                    bot->GetMotionMaster()->MovePoint(0, lx, ly, lz, FORCED_MOVEMENT_NONE,
                                                      0.0f, 0.0f, /*generatePath=*/true,
                                                      /*forceDestination=*/false);
                    AssistLog(gLow, "ranged: no LoS — closing to regain line of sight");
                }
                bot->SetFacingToObject(desired);
                return;
            }

            if (d < 8.0f)
            {
                // Nothing on us but we're <8y (walked in, or the mob died / was
                // taken). Back out just PAST the dead zone (13y) so we can shoot
                // again — a ranged special shot's effective min range is ~10y for a
                // normal mob (spell min + melee range), so 13y is just clear of it.
                // The rotation's own too-close check nudges it further for big mobs.
                // Drop any melee.
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
                    if (ComputeRangedSpreadSpot(bot, desired, hold, sx, sy, sz))
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

            // Party-wipe regroup: no healer offered a res (the whole party can be
            // dead, including the healer), so a dead follower would lie there
            // forever. Once the LEADER is alive and OUT OF COMBAT — the fight is
            // over — revive the follower at full health beside the leader. Gated on
            // leader-not-in-combat so we never battle-rez mid-pull (which would
            // just feed the same fight); the player kills what's on them, then the
            // party stands back up and reforms.
            if (!follower->IsAlive() && !follower->isResurrectRequested()
                && leader->IsAlive() && !leader->IsInCombat()
                && leader->GetMapId() == follower->GetMapId())
            {
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
                    "[WowPsParty Follow] {} auto-revived at leader (party regroup)",
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
            if (dist > 8.0f && movedSelf < 1.0f) ++s.idle;
            else { s.idle = 0; s.unstickAttempts = 0; }   // moved or caught up = recovered
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
                        "unitState={:#x} moveFlags={:#x}",
                        follower->GetName(), dist, s.idle, ustate,
                        follower->GetUnitMovementFlags());
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
            // never jittered/wandered. Install it immediately here and leave the
            // humanize tick disabled for it (eligible already false).
            if (isLeadTank)
            {
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
                // far behind" bug). A few yards ahead so it body-pulls.
                follower->GetMotionMaster()->Clear();
                follower->GetMotionMaster()->MoveFollow(leader, 8.0f, 0.0f);
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
                bool const leaderMoving = leader->isMoving();

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
                    follower->GetMotionMaster()->MoveFollow(leader, h.slotDist, h.slotAngle);
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
                float const dLead     = follower->GetDistance(leader);

                // Notably out of formation — e.g. just left combat away from the
                // party — walk back toward the slot. The threshold sits ABOVE the
                // wander cap, so a bot that merely strolled out is never yanked
                // back (the user wants it to keep its wandered spot); only a real
                // post-combat drift reforms.
                if (dLead > wanderCap + 0.5f)
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
                // direction is weighted back toward the leader the further out
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
                        float const dir = LerpAngle(randDir, follower->GetAngle(leader), wBack);
                        float const r = frand(1.5f, 3.5f);
                        float wx = follower->GetPositionX() + r * std::cos(dir);
                        float wy = follower->GetPositionY() + r * std::sin(dir);
                        float wz = follower->GetPositionZ();
                        // Hard cap: never settle past the wander leash, so a
                        // wandering bot can't sit in the 1 Hz stuck-detector's
                        // >8y window (needless unstick on a big party).
                        if (follower->GetMap()
                            && follower->GetMap()->CanReachPositionAndGetValidCoords(follower, wx, wy, wz)
                            && leader->GetExactDist2d(wx, wy) < wanderCap)
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

            for (auto const& d : snapshot)
                (void)ApplyDirective(d);
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
