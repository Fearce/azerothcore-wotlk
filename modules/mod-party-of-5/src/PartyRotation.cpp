/*
 * WowPs Party-of-5 mod — rotation DSL implementation
 */

#include "PartyRotation.h"
#include "PartyFollow.h"
#include "PartyMgr.h"   // MaintainBotConsumables

#include "Bag.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "WorldSession.h"

#include <functional>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace WowPsParty
{
    static std::unordered_map<uint32, std::vector<RotationRule>> g_rotationCache;
    static std::mutex g_rotationCacheMutex;

    // Cache for BotRangedCastHold (guidLow -> (yards, ms)). Invalidated whenever
    // the rotation cache is written/cleared (below) so a loadout change re-derives
    // the hold immediately; that eviction also keeps it from growing unbounded.
    // Lock order if both are held: g_rotationCacheMutex OUTER, g_rangedHoldMutex
    // INNER (BotRangedCastHold only ever takes them one at a time, never nested).
    static std::unordered_map<uint32, std::pair<float, uint32>> g_rangedHoldCache;
    static std::mutex g_rangedHoldMutex;

    // ----- string helpers -----------------------------------------------------

    static std::string Trim(std::string s)
    {
        auto isspace = [](unsigned char c) { return std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c){ return !isspace(c); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char c){ return !isspace(c); }).base(), s.end());
        return s;
    }

    static std::vector<std::string> Split(std::string const& s, char sep)
    {
        std::vector<std::string> out;
        std::string buf;
        std::istringstream is(s);
        while (std::getline(is, buf, sep))
            out.push_back(Trim(buf));
        return out;
    }

    // ----- DSL parse/serialise ------------------------------------------------

    std::vector<RotationRule> ParseRotationString(std::string const& dsl)
    {
        std::vector<RotationRule> rules;
        for (std::string const& raw : Split(dsl, ';'))
        {
            if (raw.empty()) continue;
            auto fields = Split(raw, '|');
            if (fields.size() < 2) continue;
            RotationRule r;
            r.condition = fields[0];
            r.action    = fields[1];
            r.priority  = fields.size() >= 3 ? std::atoi(fields[2].c_str()) : 0;
            r.flags     = fields.size() >= 4 ? fields[3] : "";
            if (r.condition.empty() || r.action.empty()) continue;
            rules.push_back(std::move(r));
        }
        std::stable_sort(rules.begin(), rules.end(),
            [](RotationRule const& a, RotationRule const& b) { return a.priority > b.priority; });
        return rules;
    }

    std::string SerialiseRotationRules(std::vector<RotationRule> const& rules)
    {
        std::ostringstream out;
        bool first = true;
        for (auto const& r : rules)
        {
            if (!first) out << ';';
            first = false;
            out << r.condition << '|' << r.action << '|' << r.priority;
            // Only emit the 4th field when set, so rotations that use no
            // flags round-trip to the exact same string they had before.
            if (!r.flags.empty())
                out << '|' << r.flags;
        }
        return out.str();
    }

    // ----- cache --------------------------------------------------------------

    // Drop the derived ranged-hold for a bot whose rotation just changed (or who
    // logged out). Nested INSIDE g_rotationCacheMutex by the callers below — the
    // documented lock order.
    static void EvictRangedHold(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(g_rangedHoldMutex);
        g_rangedHoldCache.erase(guid);
    }

    void RotationCacheSet(uint32 guid, std::vector<RotationRule> rules)
    {
        std::lock_guard<std::mutex> lock(g_rotationCacheMutex);
        if (rules.empty())
            g_rotationCache.erase(guid);
        else
            g_rotationCache[guid] = std::move(rules);
        EvictRangedHold(guid);
    }

    void RotationCacheClear(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(g_rotationCacheMutex);
        g_rotationCache.erase(guid);
        EvictRangedHold(guid);
    }

    // Defined further down; forward-declared so BotIsKiting uses the SAME
    // disabled-flag predicate as the rule loop (lowercased, comma-tokenised) and
    // the two can't disagree — a mismatch would freeze a bot whose kite rule is
    // disabled (AssistTarget yields to a rotation that never hops).
    static std::string Lower(std::string s);
    static bool CsvContains(std::string const& csv, std::string const& kw);

    // True if the bot's rotation opts into rotation-driven positioning — a
    // keep_distance_enemy (kite) or keep_distance_healer rule. AssistTarget reads
    // this to STOP installing its chase/dead-zone movement, handing the feet to
    // the rotation so the two don't fight (only one mover at a time).
    bool BotIsKiting(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_rotationCacheMutex);
        auto it = g_rotationCache.find(guid.GetCounter());
        if (it == g_rotationCache.end()) return false;
        for (RotationRule const& r : it->second)
            if ((r.action.rfind("keep_distance", 0) == 0
                 || r.action.rfind("close_to_enemy", 0) == 0)
                && !CsvContains(Lower(r.flags), "disabled"))
                return true;
        return false;
    }

    void RotationCacheRefreshFromDB(uint32 guid)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `priority_actions_json` FROM `party_loadout` WHERE `guid` = {}", guid);
        if (!q)
        {
            RotationCacheClear(guid);
            return;
        }
        std::string const dsl = q->Fetch()[0].Get<std::string>();
        auto rules = ParseRotationString(dsl);
        RotationCacheSet(guid, std::move(rules));
    }

    bool HasRotation(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(g_rotationCacheMutex);
        return g_rotationCache.find(guid) != g_rotationCache.end();
    }

    // ----- time-to-die estimator ----------------------------------------------
    //
    // The Kevkili rotations gate big cooldowns / DoTs / Sunder refreshes on
    // "will this thing live another N seconds?" There's no server API for it,
    // so we keep a short health-history per target GUID and fit a damage rate.
    // Modelled on Kevkili's TargetTTD addon (12 s sample window, linear slope),
    // but server-side so every bot sharing a target reuses the same samples.
    struct TtdSample { uint32 timeMs; uint64 health; };
    static std::unordered_map<uint64, std::deque<TtdSample>> g_ttdSamples;
    static std::mutex g_ttdMutex;
    static uint32 g_ttdLastSweepMs = 0;

    // Guards the per-bot consumable/use-item throttle timestamp maps below.
    // Map updates can run on multiple threads, so these shared statics need
    // the same lock discipline as g_ttdSamples / g_rotationCache.
    static std::mutex g_useThrottleMutex;

    // Per-bot "approaching a cast target" state: guid -> (targetGuidLow,
    // lastMoveChaseMs). Throttles MoveChase re-issue so the LoS/range
    // approach doesn't recompute the navmesh path every UpdateAI tick.
    // Guarded by g_useThrottleMutex.
    static std::unordered_map<uint32, std::pair<uint32, uint32>> g_approachState;

    static constexpr uint32 TTD_WINDOW_MS  = 12000; // keep ~12 s of history
    static constexpr uint32 TTD_SAMPLE_MS  = 500;   // at most one sample / 0.5 s
    static constexpr int    TTD_UNKNOWN    = 999;   // "effectively never dies"

    // Record one health sample for `target` (throttled). Called once per tick
    // for each bot's current victim. Resets the history if the target healed
    // (health went up) so a heal mid-fight doesn't produce a negative slope.
    static void TtdRecord(Unit* target)
    {
        if (!target || !target->IsAlive()) return;
        uint64 const key = target->GetGUID().GetRawValue();
        uint64 const hp  = target->GetHealth();
        uint32 const now = getMSTime();

        std::lock_guard<std::mutex> lock(g_ttdMutex);

        // Periodic sweep so GUIDs of dead/despawned mobs don't accumulate.
        if (now - g_ttdLastSweepMs > 60000)
        {
            g_ttdLastSweepMs = now;
            for (auto it = g_ttdSamples.begin(); it != g_ttdSamples.end(); )
            {
                if (it->second.empty() ||
                    now - it->second.back().timeMs > TTD_WINDOW_MS * 2)
                    it = g_ttdSamples.erase(it);
                else
                    ++it;
            }
        }

        auto& dq = g_ttdSamples[key];
        if (!dq.empty())
        {
            if (now - dq.back().timeMs < TTD_SAMPLE_MS) return; // throttle
            if (hp > dq.back().health) dq.clear();              // healed → reset
        }
        dq.push_back({ now, hp });
        while (!dq.empty() && now - dq.front().timeMs > TTD_WINDOW_MS)
            dq.pop_front();
    }

    // Estimate seconds until `target` dies from the recorded samples. Returns
    // TTD_UNKNOWN when we lack data or the target isn't losing health.
    static int TtdSeconds(Unit* target)
    {
        if (!target || !target->IsAlive()) return TTD_UNKNOWN;
        uint64 const key = target->GetGUID().GetRawValue();

        std::lock_guard<std::mutex> lock(g_ttdMutex);
        auto it = g_ttdSamples.find(key);
        if (it == g_ttdSamples.end()) return TTD_UNKNOWN;
        auto const& dq = it->second;
        if (dq.size() < 2) return TTD_UNKNOWN;

        TtdSample const& first = dq.front();
        TtdSample const& last  = dq.back();
        uint32 const dtMs = last.timeMs - first.timeMs;
        if (dtMs < 1000) return TTD_UNKNOWN;            // need ~1 s of spread
        if (last.health >= first.health) return TTD_UNKNOWN; // not dropping

        double const lostPerMs =
            double(first.health - last.health) / double(dtMs);
        if (lostPerMs <= 0.0) return TTD_UNKNOWN;
        double const secs = double(last.health) / (lostPerMs * 1000.0);
        if (secs >= double(TTD_UNKNOWN)) return TTD_UNKNOWN;
        return int(secs);
    }

    // ----- party helpers ------------------------------------------------------

    // Walks the bot's group and returns the lowest-HP alive party member
    // (including the bot itself). Returns nullptr if the bot isn't in a group
    // or no member is alive in range.
    // Authoritative party roster for target-selection. Enumerates from our
    // own follow directives (leader + all bots) rather than bot->GetGroup(),
    // which can form incompletely from bot-spawn timing and leave a bot blind
    // to the leader's health. Only in-world, same-map members are returned;
    // pass includeDead=true for resurrection targeting. Falls back to the WoW
    // group, then to the bot alone, if directives aren't populated yet.
    static void GatherPartyPlayers(Player* bot, std::vector<Player*>& out,
                                   bool includeDead)
    {
        if (!bot) return;
        auto consider = [&](Player* m) {
            if (!m || !m->IsInWorld()) return;
            if (m->GetMapId() != bot->GetMapId()) return;
            if (!includeDead && !m->IsAlive()) return;
            if (std::find(out.begin(), out.end(), m) == out.end())
                out.push_back(m);
        };

        std::vector<ObjectGuid> guids;
        WowPsParty::GetPartyGuidsFor(bot->GetGUID(), guids);
        for (ObjectGuid const& g : guids)
            consider(ObjectAccessor::FindConnectedPlayer(g));

        if (out.empty())
        {
            if (Group* grp = bot->GetGroup())
                for (GroupReference* itr = grp->GetFirstMember(); itr; itr = itr->next())
                    consider(itr->GetSource());
            else
                consider(bot);
        }
    }

    static Player* GetLowestHpPartyMember(Player* bot)
    {
        std::vector<Player*> party;
        GatherPartyPlayers(bot, party, /*includeDead=*/false);
        Player* best = nullptr;
        float bestPct = 200.0f;
        for (Player* m : party)
        {
            float const maxHp = float(m->GetMaxHealth());
            if (maxHp <= 0) continue;
            float const pct = (float(m->GetHealth()) / maxHp) * 100.0f;
            if (pct < bestPct)
            {
                bestPct = pct;
                best    = m;
            }
        }
        return best;
    }

    // Returns the lowest-HP%% across the bot's party (0-100). 200 if no
    // members are evaluable.
    static int GetLowestPartyHpPercent(Player* bot)
    {
        Player* p = GetLowestHpPartyMember(bot);
        if (!p) return 200;
        float const maxHp = float(p->GetMaxHealth());
        if (maxHp <= 0) return 200;
        return int((float(p->GetHealth()) / maxHp) * 100.0f);
    }

    // Defined further down; forward-declared so the rank-insensitive buff check
    // below (matched by spell name) can reuse it.
    static Aura const* FindNamedAura(Unit* unit, std::string const& name);

    // True if target already has the buff `spellId` represents — matched by NAME
    // (any RANK counts), not the exact rank's id. Rank-sensitive matching caused
    // the "two mages spam Arcane Intellect on one member forever" bug: mage A
    // (rank 5) and mage B (rank 6) each saw the OTHER's rank as "missing mine" and
    // re-cast, overwriting each other every GCD. All callers are group-buff / HoT
    // presence checks ("skip if they already have it"), so any rank should count.
    // Falls back to the exact-id check if the spell or its name can't be resolved.
    static bool HasAuraFromSpell(Unit* target, uint32 spellId)
    {
        if (!target || !spellId) return false;
        if (SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId))
            if (char const* nm = si->SpellName[0])
                if (*nm)
                    return FindNamedAura(target, nm) != nullptr;
        return target->HasAura(spellId);
    }

    // ----- AoE / aggro helpers -----------------------------------------------

    // Gather every hostile alive Unit within `radius` of the bot. Uses AC's
    // cell visitor + AnyUnfriendlyUnitInObjectRangeCheck — the same pattern
    // creature AI uses for "find nearest enemy" searches.
    static void GatherHostilesAround(Player* bot, float radius,
                                     std::list<Unit*>& out)
    {
        if (!bot || !bot->IsInWorld()) return;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, radius);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck>
            searcher(bot, out, check);
        Cell::VisitObjects(bot, searcher, radius);
    }

    static uint32 CountHostilesWithin(Player* bot, float radius)
    {
        std::list<Unit*> targets;
        GatherHostilesAround(bot, radius, targets);
        return uint32(targets.size());
    }

    // 2D distance from point (px,py) to the segment (ax,ay)->(bx,by). Used to
    // test whether a hostile sits near a bot's retreat LANE, not just its
    // endpoint — clipping past a mob mid-kite pulls it just as surely as
    // stopping next to one.
    static float DistPointToSeg2D(float px, float py, float ax, float ay,
                                  float bx, float by)
    {
        float const abx = bx - ax, aby = by - ay;
        float const len2 = abx * abx + aby * aby;
        float t = 0.0f;
        if (len2 > 0.0001f)
            t = ((px - ax) * abx + (py - ay) * aby) / len2;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
        float const cx = ax + t * abx, cy = ay + t * aby;
        float const dx = px - cx, dy = py - cy;
        return std::sqrt(dx * dx + dy * dy);
    }

    // Choose a kite destination at `dist` yards from `enemy` that the bot can
    // retreat to WITHOUT backing into — or pathing past — UN-AGGROED hostile
    // mobs, so kiting doesn't pull a fresh pack. Mobs already in combat are
    // fine to walk past (they add no new pull), so only out-of-combat hostiles
    // block a lane — otherwise the kiter gets boxed in by the very pack it's
    // fighting. Fans out from the straight-away direction (enemy -> bot)
    // through progressively wider angles; the first candidate whose retreat
    // LANE stays DANGER yards clear of every un-aggroed hostile wins. Returns
    // false when every lane is crowded, so the caller skips the kite and casts
    // in place. (Reachability/lava is still handled by MovePoint's
    // forceDestination=false at the call site.)
    static bool PickSafeKitePoint(Player* bot, Unit* enemy, float dist,
                                  float& ox, float& oy, float& oz)
    {
        if (!bot || !enemy) return false;

        std::list<Unit*> hostiles;
        GatherHostilesAround(bot, 40.0f, hostiles);

        float const DANGER = 12.0f;                 // keep the whole lane this clear
        float const baseAngle = enemy->GetAngle(bot);  // directly away from enemy
        float const bx = bot->GetPositionX();
        float const by = bot->GetPositionY();

        // Direct retreat first, then symmetric fan-out (~20deg steps each side).
        static float const offsets[] =
            { 0.0f, 0.35f, -0.35f, 0.7f, -0.7f, 1.05f, -1.05f, 1.4f, -1.4f };

        for (float off : offsets)
        {
            float x, y, z;
            enemy->GetNearPoint(bot, x, y, z, 0.0f, dist, baseAngle + off);

            bool crowded = false;
            for (Unit* h : hostiles)
            {
                if (!h || h == enemy || !h->IsAlive()) continue;
                // Only UN-AGGROED mobs are a danger — walking past something
                // already fighting the party pulls nothing new, and treating it
                // as a wall is what corners the kiter. Skip in-combat hostiles.
                if (h->IsInCombat()) continue;
                // Reject if an un-aggroed mob is near the retreat lane (bot ->
                // spot); this subsumes the endpoint test (spot = lane's end).
                if (DistPointToSeg2D(h->GetPositionX(), h->GetPositionY(),
                                     bx, by, x, y) < DANGER)
                {
                    crowded = true;
                    break;
                }
            }
            if (crowded) continue;

            // Must still be able to SEE the target from the kite spot, or the
            // caster kites around a corner and stands there unable to cast
            // (Flamestrike/Fireball both need line of sight). Reject blind spots.
            if (!enemy->IsWithinLOS(x, y, z))
                continue;

            ox = x; oy = y; oz = z;
            return true;
        }
        return false;   // no safe + in-LoS spot — caller skips the kite
    }

    // Size of the densest cluster of hostiles that are all within `radius` of
    // ONE of them — i.e. "how many enemies are within R of each other". This
    // is the metric AoE placement wants (Blizzard / Flamestrike): a high value
    // means an AoE dropped on that knot of mobs would hit them all. O(n^2) over
    // the nearby hostile set, which is tiny.
    static uint32 MaxEnemyCluster(Player* bot, float radius)
    {
        std::list<Unit*> hostiles;
        GatherHostilesAround(bot, 45.0f, hostiles);   // candidate pool near bot
        uint32 best = 0;
        for (Unit* a : hostiles)
        {
            if (!a || !a->IsAlive() || !bot->IsValidAttackTarget(a)) continue;
            uint32 c = 0;
            for (Unit* b : hostiles)
            {
                if (!b || !b->IsAlive() || !bot->IsValidAttackTarget(b)) continue;
                if (a->GetDistance(b) <= radius) ++c;   // counts a itself too
            }
            if (c > best) best = c;
        }
        return best;
    }

    // The enemy that anchors the densest cluster (most neighbours within
    // `radius`). Cast a ground-targeted AoE at its position to catch the most
    // mobs. nullptr if no valid hostile nearby.
    static Unit* BestClusterAnchor(Player* bot, float radius)
    {
        std::list<Unit*> hostiles;
        GatherHostilesAround(bot, 45.0f, hostiles);
        Unit* best = nullptr;
        uint32 bestCount = 0;
        for (Unit* a : hostiles)
        {
            if (!a || !a->IsAlive() || !bot->IsValidAttackTarget(a)) continue;
            uint32 c = 0;
            for (Unit* b : hostiles)
                if (b && b->IsAlive() && bot->IsValidAttackTarget(b)
                    && a->GetDistance(b) <= radius)
                    ++c;
            if (c > bestCount) { bestCount = c; best = a; }
        }
        return best;
    }

    // Dominant talent tree (tabpage 0/1/2) of a LIVE bot, by points spent.
    // Mirrors PartyMgr's offline InferHenchmanRole but reads learned talent
    // ranks straight from the player in memory (HasSpell), so it tracks the
    // bot's CURRENT spec. Cached per-bot with a short TTL — the walk over the
    // talent DBC is cheap but pointless to repeat every tick, and a spec rarely
    // changes mid-pull. Returns 0 (the benign default) when the bot has no
    // talents yet. Mage trees: 0=Arcane, 1=Fire, 2=Frost.
    uint8 PrimaryTalentTree(Player* bot)   // declared in PartyRotation.h (used by the follow layer too)
    {
        static std::mutex mtx;
        static std::unordered_map<uint32, std::pair<uint32, uint8>> cache;  // guid -> (computedMs, tree)
        uint32 const guid = bot->GetGUID().GetCounter();
        uint32 const now  = getMSTime();
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = cache.find(guid);
            if (it != cache.end() && now - it->second.first < 10000)
                return it->second.second;
        }

        uint32 points[3] = { 0, 0, 0 };
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* tal = sTalentStore.LookupEntry(i);
            if (!tal) continue;
            TalentTabEntry const* tab = sTalentTabStore.LookupEntry(tal->TalentTab);
            if (!tab || tab->tabpage > 2) continue;
            for (int rank = int(tal->RankID.size()) - 1; rank >= 0; --rank)
                if (tal->RankID[rank] && bot->HasSpell(tal->RankID[rank]))
                {
                    points[tab->tabpage] += uint32(rank + 1);
                    break;
                }
        }
        uint8 tree = 0;
        if (points[1] > points[tree]) tree = 1;
        if (points[2] > points[tree]) tree = 2;

        {
            std::lock_guard<std::mutex> lock(mtx);
            cache[guid] = { now, tree };
        }
        return tree;
    }

    // Find an enemy within `radius` of the bot whose current victim is a
    // party member OTHER than the bot itself. Used by the tank's
    // `cast_loose_enemy:Taunt` rule to pull aggro off the healer / casters.
    static Unit* FindLooseEnemy(Player* bot, float radius)
    {
        if (!bot) return nullptr;
        std::list<Unit*> targets;
        GatherHostilesAround(bot, radius, targets);

        // Party membership from our own directives (leader + bots), NOT the
        // WoW group — the group can form incompletely and the tank would then
        // ignore a mob beating on the leader.
        std::vector<ObjectGuid> partyGuids;
        WowPsParty::GetPartyGuidsFor(bot->GetGUID(), partyGuids);

        for (Unit* enemy : targets)
        {
            if (!enemy || !enemy->IsAlive()) continue;
            Unit* victim = enemy->GetVictim();
            if (!victim) continue;
            if (victim == bot) continue;          // already on us
            if (!victim->IsPlayer()) continue;     // only care about party
            if (std::find(partyGuids.begin(), partyGuids.end(),
                          victim->GetGUID()) == partyGuids.end())
                continue;                          // victim isn't a party member
            if (!bot->IsValidAttackTarget(enemy)) continue;
            return enemy;
        }
        return nullptr;
    }

    // ----- shared-inventory food/drink helpers -------------------------------

    // Visit every Item in the bags of every member of `bot`'s account_party.
    // Visitor returns true to stop iteration. Used by the food/drink helpers
    // below — counts inventory across the *whole party*, not just the bot.
    static void ForEachSharedItem(Player* bot,
        std::function<bool(Player* owner, Item* item)> const& visit)
    {
        if (!bot || !bot->GetSession()) return;
        uint32 const account = bot->GetSession()->GetAccountId();
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid` FROM `account_party` WHERE `account` = {}", account);
        if (!q) return;
        do
        {
            uint32 const g = q->Fetch()[0].Get<uint32>();
            Player* owner = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(g));
            if (!owner) continue;

            // Backpack
            for (uint8 s = INVENTORY_SLOT_ITEM_START; s < INVENTORY_SLOT_ITEM_END; ++s)
            {
                Item* it = owner->GetItemByPos(INVENTORY_SLOT_BAG_0, s);
                if (it && visit(owner, it)) return;
            }
            // Equipped bags
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            {
                Bag* bag = owner->GetBagByPos(b);
                if (!bag) continue;
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                {
                    Item* it = owner->GetItemByPos(b, i);
                    if (it && visit(owner, it)) return;
                }
            }
        } while (q->NextRow());
    }

    // A consumable item is treated as a "drink" if any of its on-use spells
    // applies a MOD_POWER_REGEN aura, and as "food" if it applies MOD_REGEN.
    // This catches every standard food/drink (Refreshing Spring Water,
    // Conjured Mana Biscuit, Tough Hunk of Bread, etc.) without us having to
    // maintain a hardcoded item-id list.
    static bool ItemSpellAppliesAura(ItemTemplate const* tmpl, uint32 auraName,
                                     uint32* outSpellId)
    {
        if (!tmpl) return false;
        if (tmpl->Class != ITEM_CLASS_CONSUMABLE) return false;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            int32 const sid = tmpl->Spells[i].SpellId;
            if (sid <= 0) continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(sid));
            if (!info) continue;
            for (uint8 e = 0; e < 3; ++e)
            {
                if (info->Effects[e].ApplyAuraName == auraName)
                {
                    if (outSpellId) *outSpellId = uint32(sid);
                    return true;
                }
            }
        }
        return false;
    }

    static bool ItemIsDrink(ItemTemplate const* t, uint32* outSpellId = nullptr)
    {
        return ItemSpellAppliesAura(t, SPELL_AURA_MOD_POWER_REGEN, outSpellId);
    }

    static bool ItemIsFood(ItemTemplate const* t, uint32* outSpellId = nullptr)
    {
        return ItemSpellAppliesAura(t, SPELL_AURA_MOD_REGEN, outSpellId);
    }

    // Sum item count across all party bags for items that match the drink-
    // or food-classifier predicate.
    static uint32 CountSharedConsumables(Player* bot, bool drink)
    {
        uint32 total = 0;
        ForEachSharedItem(bot, [drink, &total](Player*, Item* it) -> bool {
            // Conditional function-pointer call loses the default arg, so
            // pass nullptr for outSpellId explicitly.
            ItemTemplate const* tmpl = it->GetTemplate();
            bool const ok = drink ? ItemIsDrink(tmpl, nullptr)
                                  : ItemIsFood (tmpl, nullptr);
            if (ok) total += it->GetCount();
            return false;
        });
        return total;
    }

    // Find a food/drink item anywhere in the shared inventory, decrement
    // one charge (destroy if last), and return the use-spell to apply.
    // Returns 0 if nothing was found.
    [[maybe_unused]] static uint32 ConsumeSharedConsumable(Player* bot, bool drink)
    {
        uint32 castSpell = 0;
        ForEachSharedItem(bot, [drink, &castSpell](Player* owner, Item* it) -> bool {
            uint32 sid = 0;
            bool const ok = drink ? ItemIsDrink(it->GetTemplate(), &sid)
                                  : ItemIsFood (it->GetTemplate(), &sid);
            if (!ok || !sid) return false;
            castSpell = sid;
            if (it->GetCount() > 1)
            {
                it->SetCount(it->GetCount() - 1);
                it->SetState(ITEM_CHANGED, owner);
            }
            else
            {
                owner->DestroyItem(it->GetBagSlot(), it->GetSlot(), true);
            }
            return true;  // stop iteration
        });
        return castSpell;
    }

    // Keep a seated bot FAST-restoring HP/mana until topped: (re)apply the food
    // (433) / drink (430) auras whenever they lapse, and add a fraction of the max
    // pool every 1.5 s so recovery to full is ~7.5 s at any level. Shared by the
    // drink/eat verb AND the "commit to consuming" hold below — the bug was that
    // the hold suppressed the WHOLE rotation (including the verb that does this),
    // so a bot restored one slice then sat there with a lapsed aura crawling on
    // natural regen ("consuming but mana stuck, takes forever"). Out-of-combat
    // only is the caller's responsibility (the regen auras break in combat anyway).
    static void SustainConsume(Player* bot)
    {
        if (!bot) return;
        if (bot->getStandState() != UNIT_STAND_STATE_SIT)
            bot->SetStandState(UNIT_STAND_STATE_SIT);

        uint32 const mxMana = bot->GetMaxPower(POWER_MANA);   // 0 for non-mana classes
        uint32 const mxHp   = bot->GetMaxHealth();
        bool const needMana = mxMana > 0 && bot->GetPower(POWER_MANA) < mxMana;
        bool const needHp   = bot->GetHealth() < mxHp;
        if (needMana && !bot->HasAura(430)) bot->CastSpell(bot, 430, true);   // Drink
        if (needHp   && !bot->HasAura(433)) bot->CastSpell(bot, 433, true);   // Food

        static std::unordered_map<uint32, uint32> lastRegenMs;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> lock(g_useThrottleMutex);
            uint32& lr = lastRegenMs[bot->GetGUID().GetCounter()];
            if (now - lr >= 1500)
            {
                lr = now;
                if (needMana)
                    bot->SetPower(POWER_MANA, std::min(mxMana,
                        bot->GetPower(POWER_MANA) + std::max<uint32>(1, mxMana / 5)));
                if (needHp)
                    bot->SetHealth(std::min(mxHp,
                        bot->GetHealth() + std::max<uint32>(1, mxHp / 5)));
            }
        }
        WowPsParty::HoldFollower(bot->GetGUID(), 1500);
    }

    // True if `target` carries any debuff with the given dispel type
    // (DISPEL_DISEASE / DISPEL_POISON / DISPEL_MAGIC / DISPEL_CURSE).
    // Used by the "cure" condition + action pair so a priest with Cure
    // Disease can react to a diseased party member.
    static bool HasDebuffOfType(Unit* u, DispelType type)
    {
        if (!u) return false;
        for (auto const& kv : u->GetAppliedAuras())
        {
            Aura const* aura = kv.second ? kv.second->GetBase() : nullptr;
            if (!aura) continue;
            SpellInfo const* si = aura->GetSpellInfo();
            if (!si) continue;
            if (si->Dispel == uint32(type) && !si->IsPositive())
                return true;
        }
        return false;
    }

    // Walk the bot's group and return the first DEAD member. Used by the
    // resurrect rule path.
    static Player* FindDeadPartyMember(Player* bot)
    {
        if (!bot) return nullptr;
        std::vector<Player*> party;
        GatherPartyPlayers(bot, party, /*includeDead=*/true);
        for (Player* m : party)
        {
            if (m == bot || m->IsAlive()) continue;
            return m;
        }
        return nullptr;
    }

    // Walk the bot's group and return the first member carrying a debuff
    // of the requested dispel type. Returns nullptr if no member is
    // afflicted (or no group).
    static Player* FindPartyMemberWithDispelType(Player* bot, DispelType type)
    {
        if (!bot) return nullptr;
        std::vector<Player*> party;
        GatherPartyPlayers(bot, party, /*includeDead=*/false);
        for (Player* m : party)
            if (HasDebuffOfType(m, type)) return m;
        return nullptr;
    }

    // Find a connected party member on the bot's account with the given role.
    // role ∈ {"tank","healer","dps"}. Returns nullptr if no such member is
    // currently in world on the same map. Walks account_party directly so it
    // works regardless of group composition.
    static Player* FindPartyMemberByRole(Player* bot, char const* role)
    {
        if (!bot || !bot->GetSession()) return nullptr;
        uint32 const account = bot->GetSession()->GetAccountId();
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid` FROM `account_party` "
            "WHERE `account` = {} AND `role` = '{}'", account, role);
        if (!q) return nullptr;
        do
        {
            uint32 const g = q->Fetch()[0].Get<uint32>();
            Player* p = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(g));
            if (p && p->IsAlive() && p->IsInWorld() && p->GetMapId() == bot->GetMapId())
                return p;
        } while (q->NextRow());
        return nullptr;
    }

    // ----- condition evaluator ------------------------------------------------

    // Returns true on parse + match. Conditions of the form name<N or name>N
    // (no spaces) are parsed as comparison; otherwise treated as a flag check.
    // True if `target` carries an aura whose Spell.dbc name matches the
    // given string case-insensitively. Walks the applied-aura map and
    // compares names rather than spell IDs because Kevkili-style rules
    // reference specific debuffs (Hamstring, Thunder Clap, Mortal Strike)
    // by name and we want the rule to match any rank.
    // Return the first aura on `unit` whose spell name matches `name`
    // case-insensitively, or nullptr. Callers that only need presence use
    // TargetHasNamedAura; the timing/stack conditions need the Aura* to read
    // GetDuration()/GetStackAmount().
    static Aura const* FindNamedAura(Unit* unit, std::string const& name)
    {
        if (!unit || name.empty()) return nullptr;
        std::string needle;
        needle.reserve(name.size());
        for (char c : name) needle.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        for (auto const& kv : unit->GetAppliedAuras())
        {
            Aura const* a = kv.second ? kv.second->GetBase() : nullptr;
            if (!a) continue;
            SpellInfo const* si = a->GetSpellInfo();
            if (!si) continue;
            char const* sname = si->SpellName[0];
            if (!sname) continue;
            std::string lower;
            for (char const* p = sname; *p; ++p)
                lower.push_back(char(std::tolower(static_cast<unsigned char>(*p))));
            if (lower == needle) return a;
        }
        return nullptr;
    }

    static bool TargetHasNamedAura(Unit* target, std::string const& name)
    {
        return FindNamedAura(target, name) != nullptr;
    }

    // Remaining duration of a named aura, in milliseconds. 0 if absent.
    // Permanent auras (maxDuration == -1) report a very large value so
    // "remaining > N" gates treat them as never-expiring.
    static int32 NamedAuraRemainingMs(Unit* unit, std::string const& name)
    {
        Aura const* a = FindNamedAura(unit, name);
        if (!a) return 0;
        if (a->IsPermanent()) return 0x7FFFFFFF;
        return a->GetDuration();
    }

    static uint32 NamedAuraStacks(Unit* unit, std::string const& name)
    {
        Aura const* a = FindNamedAura(unit, name);
        if (!a) return 0;
        uint8 const s = a->GetStackAmount();
        return s ? s : 1;  // a present non-stacking aura counts as 1
    }

    // Creature rank (elite tier) of a unit, or -1 if it isn't a creature
    // (players, pets without a template, etc.). Maps to CreatureEliteType.
    static int UnitCreatureRank(Unit* u)
    {
        if (!u) return -1;
        Creature* c = u->ToCreature();
        if (!c) return -1;
        CreatureTemplate const* t = c->GetCreatureTemplate();
        return t ? int(t->rank) : -1;
    }

    // Return the unit the bot is currently fighting, accounting for "no
    // victim yet" — used by the target_* conditions.
    static Unit* BotTarget(Player* bot) { return bot ? bot->GetVictim() : nullptr; }

    // The spell `target` is currently casting (generic) or channeling, with a
    // flag for whether a kick (silence-prevention) would interrupt it. Returns
    // nullptr if the target isn't casting anything.
    static SpellInfo const* CurrentCastSpell(Unit* target, bool* outChanneled,
                                             bool* outInterruptible)
    {
        if (outChanneled) *outChanneled = false;
        if (outInterruptible) *outInterruptible = false;
        if (!target) return nullptr;
        // Channeled first (Drain Life, Mind Flay, Hurricane …), then the
        // generic cast bar (Frostbolt, Heal, …).
        Spell* s = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        bool channeled = true;
        if (!s)
        {
            s = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            channeled = false;
        }
        if (!s) return nullptr;
        SpellInfo const* info = s->GetSpellInfo();
        if (!info) return nullptr;
        if (outChanneled) *outChanneled = channeled;
        if (outInterruptible)
            *outInterruptible =
                (info->PreventionType == SPELL_PREVENTION_TYPE_SILENCE);
        return info;
    }

    // Forward decls: EvalCondition recurses through AND-chains, and the
    // aura/cooldown conditions resolve spells by name (defined further down).
    static bool EvalSingleCondition(std::string const& cond, Player* bot);
    static uint32 FindKnownSpellByName(Player* bot, std::string const& name);

    static bool EvalCondition(std::string const& cond, Player* bot)
    {
        // AND chain: any number of conditions separated by `&` — every
        // clause must evaluate true. Lets Kevkili-style warrior rules
        // compose stance + rage + missing-aura gates in one rule.
        size_t p = 0;
        while (p <= cond.size())
        {
            size_t amp = cond.find('&', p);
            std::string clause = (amp == std::string::npos)
                ? cond.substr(p) : cond.substr(p, amp - p);
            if (!clause.empty())
            {
                // Leading '!' negates the clause — lets the editor express
                // "NOT <anything>" generically (NOT elite, NOT casting, …)
                // without a dedicated opposite condition for each one.
                bool negate = false;
                if (clause[0] == '!') { negate = true; clause.erase(0, 1); }
                bool r = clause.empty() ? true : EvalSingleCondition(clause, bot);
                if (negate) r = !r;
                if (!r) return false;
            }
            if (amp == std::string::npos) break;
            p = amp + 1;
        }
        return true;
    }

    static bool EvalSingleCondition(std::string const& cond, Player* bot)
    {
        if (cond == "always") return true;
        // Conditions with a string arg: `<name>:<spell-name>`.
        // target_has_aura:Hamstring / target_missing_aura:Mortal Strike /
        // self_has_aura:Battle Shout / self_missing_aura:Bloodrage.
        auto colon = cond.find(':');
        if (colon != std::string::npos)
        {
            std::string const cname = cond.substr(0, colon);
            std::string const arg   = cond.substr(colon + 1);
            if (cname == "target_has_aura" || cname == "target_missing_aura")
            {
                Unit* victim = bot->GetVictim();
                if (!victim) return cname == "target_missing_aura"; // no target = no aura
                bool const has = TargetHasNamedAura(victim, arg);
                return cname == "target_has_aura" ? has : !has;
            }
            if (cname == "self_has_aura")     return TargetHasNamedAura(bot, arg);
            if (cname == "self_missing_aura") return !TargetHasNamedAura(bot, arg);

            // --- Aura timing / stacks / cooldown (the min-max primitives) ---
            // These all share an arg of the form "<spell name><op><number>",
            // where <op> is the first '<' or '>' in the arg. Spell names have
            // no angle brackets so the split is unambiguous. Helper unpacks
            // (name, op, value); returns false if malformed.
            auto unpackNameOpVal = [&arg](std::string& outName, char& outOp,
                                          int& outVal) -> bool
            {
                auto p = arg.find_first_of("<>");
                if (p == std::string::npos || p == 0) return false;
                outName = arg.substr(0, p);
                outOp   = arg[p];
                outVal  = std::atoi(arg.substr(p + 1).c_str());
                return true;
            };

            // Refresh window: "target_aura_remain:Rend<2" is TRUE when Rend
            // has under 2s left OR is absent (remaining 0) — exactly the
            // "reapply 2s before it drops" pattern. ">N" requires the aura
            // present with more than N seconds left.
            if (cname == "target_aura_remain" || cname == "self_aura_remain")
            {
                std::string n; char op; int sec;
                if (!unpackNameOpVal(n, op, sec)) return false;
                Unit* u = (cname == "self_aura_remain") ? bot : bot->GetVictim();
                if (!u) return op == '<';   // no target → "expiring/absent" true
                int const remSec = NamedAuraRemainingMs(u, n) / 1000;
                return op == '<' ? (remSec < sec) : (remSec > sec);
            }
            if (cname == "target_aura_stacks" || cname == "self_aura_stacks")
            {
                std::string n; char op; int want;
                if (!unpackNameOpVal(n, op, want)) return false;
                Unit* u = (cname == "self_aura_stacks") ? bot : bot->GetVictim();
                int const stacks = u ? int(NamedAuraStacks(u, n)) : 0;
                return op == '<' ? (stacks < want) : (stacks > want);
            }
            // "spell_cd_remain:Mortal Strike<2" — TRUE when MS will be off
            // cooldown within 2s (or already is). ">N" = still has more than
            // N seconds to go. Unknown spell → treated as "not ready" (never
            // <, never >) so a typo'd rule fails closed instead of spamming.
            if (cname == "spell_cd_remain")
            {
                std::string n; char op; int sec;
                if (!unpackNameOpVal(n, op, sec)) return false;
                uint32 const sid = FindKnownSpellByName(bot, n);
                if (!sid) return false;
                int const cdSec = int(bot->GetSpellCooldownDelay(sid)) / 1000;
                return op == '<' ? (cdSec < sec) : (cdSec > sec);
            }
            // "spell_ready:Overpower" — known AND off cooldown. Pair with the
            // proc/reactive abilities (Overpower after a dodge, Revenge after
            // a block/dodge/parry) whose usability the server tracks for us.
            if (cname == "spell_ready")
            {
                uint32 const sid = FindKnownSpellByName(bot, arg);
                if (!sid) return false;
                return bot->GetSpellCooldownDelay(sid) == 0;
            }

            // Arbitrary-radius enemy count: enemies_within:<R><op><N>
            // e.g. "enemies_within:10<2" (fewer than 2 hostiles in 10y)
            // or  "enemies_within:12>1" (more than 1 hostile in 12y).
            // Lets the user gate AoE spells on the actual spell radius
            // instead of the hardcoded 8y melee / 30y range buckets.
            if (cname == "enemies_within")
            {
                auto opPosA = arg.find_first_of("<>");
                if (opPosA == std::string::npos) return false;
                float const radius = float(std::atof(arg.substr(0, opPosA).c_str()));
                if (radius <= 0.0f) return false;
                char const opA = arg[opPosA];
                int const countN = std::atoi(arg.substr(opPosA + 1).c_str());
                int const found  = int(CountHostilesWithin(bot, radius));
                return opA == '<' ? (found < countN) : (found > countN);
            }
            // Clustering gate for placed AoE (Blizzard / Flamestrike):
            // "enemies_clustered:10>2" → more than 2 enemies are within 10y of
            // EACH OTHER (the densest knot). Same R<op>N grammar as above.
            if (cname == "enemies_clustered")
            {
                auto opPosA = arg.find_first_of("<>");
                if (opPosA == std::string::npos) return false;
                float const radius = float(std::atof(arg.substr(0, opPosA).c_str()));
                if (radius <= 0.0f) return false;
                char const opA = arg[opPosA];
                int const countN = std::atoi(arg.substr(opPosA + 1).c_str());
                int const found  = int(MaxEnemyCluster(bot, radius));
                return opA == '<' ? (found < countN) : (found > countN);
            }
            // "primary_tree:N" — TRUE when the bot's dominant talent tree is
            // tabpage N (0/1/2). Lets a rotation prefer a spec's signature
            // ability, e.g. mage "primary_tree:1" = Fire (so a fire mage leads
            // its AoE with Flamestrike instead of Blizzard). Combine with `!`
            // for the inverse.
            if (cname == "primary_tree")
                return int(PrimaryTalentTree(bot)) == std::atoi(arg.c_str());
        }
        if (cond == "in_combat")     return bot->IsInCombat();
        if (cond == "out_of_combat") return !bot->IsInCombat();
        // Party-wide combat state: true if ANY party member (leader + bots)
        // is fighting. Use these for pull / hold-position / between-pull rules
        // so the tank doesn't "pre-pull" or stand idle while the rest of the
        // party is already mid-fight but the tank personally hasn't taken
        // aggro yet (the per-bot out_of_combat is true for it then).
        if (cond == "party_in_combat" || cond == "party_out_of_combat")
        {
            std::vector<Player*> party;
            GatherPartyPlayers(bot, party, /*includeDead=*/true);
            bool any = false;
            for (Player* m : party)
                if (m->IsInCombat()) { any = true; break; }
            return cond == "party_in_combat" ? any : !any;
        }
        // Gate on the VICTIM, not the selection field. Party bots engage via
        // AssistTarget's bot->Attack(victim), which sets m_attacking (and drives
        // auto-attack) but never touches UNIT_FIELD_TARGET — so GetTarget() stays
        // empty even while the bot is swinging. Every cast:X action resolves its
        // target from GetVictim(), so has_target must agree or the whole rotation
        // NO_MATCHes for a bot that's clearly in melee ("only auto-attacks").
        if (cond == "has_target")    return bot->GetVictim() != nullptr;
        if (cond == "no_target")     return bot->GetVictim() == nullptr;
        // "target_attacking_me" — the bot's victim is targeting the bot BACK,
        // i.e. the bot has aggro on it. Gate a ranged bot's MELEE abilities on
        // this: without it a hunter whose victim is its far ranged target would
        // fire Raptor Strike (a melee ability), fail out-of-range, and the cast
        // path would walk it INTO melee — then back out — forever. With aggro,
        // the mob is already in melee on the bot, so the strike lands in place.
        if (cond == "target_attacking_me")
        {
            Unit* const v = bot->GetVictim();
            return v && v->GetVictim() == bot;
        }
        // Movement gate — pair "is_moving" with instant-only rules, or
        // "is_not_moving" so a cast-time spell only queues when planted.
        if (cond == "is_moving")     return bot->isMoving();
        if (cond == "is_not_moving") return !bot->isMoving();

        // Party-debuff checks — boolean, no <N/>N suffix.
        if (cond == "party_has_disease")
            return FindPartyMemberWithDispelType(bot, DISPEL_DISEASE) != nullptr;
        if (cond == "party_has_poison")
            return FindPartyMemberWithDispelType(bot, DISPEL_POISON) != nullptr;
        if (cond == "party_has_magic")
            return FindPartyMemberWithDispelType(bot, DISPEL_MAGIC) != nullptr;
        if (cond == "party_has_curse")
            return FindPartyMemberWithDispelType(bot, DISPEL_CURSE) != nullptr;
        if (cond == "party_has_dead")
            return FindDeadPartyMember(bot) != nullptr;

        // Warrior stance detection by aura presence. Each stance is a
        // self-applied buff that stays until you swap. Pair with
        // `buff_self:<stance>` actions to build stance-dance rotations.
        if (cond == "stance_is_battle")     return bot->HasAura(2457);
        if (cond == "stance_is_defensive")  return bot->HasAura(71);
        if (cond == "stance_is_berserker")  return bot->HasAura(2458);
        // "no stance at all" — our follow bots spawn with mod-playerbots AI
        // suppressed, so nothing puts them in a stance and every stance-locked
        // ability silently fails the shapeshift gate. Pair a top-priority
        // `stance_is_none | buff_self:Battle Stance` rule to self-correct,
        // without an `always` rule fighting a deliberate Defensive/Berserker.
        if (cond == "stance_is_none")
            return bot->getClass() == CLASS_WARRIOR
                && !bot->HasAura(2457) && !bot->HasAura(71) && !bot->HasAura(2458);
        // Same idea for druid forms — useful for feral / boomkin rules.
        if (cond == "form_is_bear")         return bot->HasAura(5487) || bot->HasAura(9634);
        if (cond == "form_is_cat")          return bot->HasAura(768);
        if (cond == "form_is_moonkin")      return bot->HasAura(24858);
        if (cond == "form_is_caster")       return bot->getClass() == CLASS_DRUID
            && !bot->HasAura(5487) && !bot->HasAura(9634) && !bot->HasAura(768)
            && !bot->HasAura(24858);

        // Boolean: is there any nearby enemy that's currently attacking an
        // ally OTHER than the bot? Used by the tank's taunt rule.
        if (cond == "enemy_loose_in_melee")
            return FindLooseEnemy(bot, 12.0f) != nullptr;
        if (cond == "enemy_loose_in_range")
            return FindLooseEnemy(bot, 30.0f) != nullptr;

        // --- Target classification / type --------------------------------
        // Gate cooldowns, DoTs, CC and snares on what the target actually is,
        // exactly like the Kevkili UnitClassification / UnitIsPlayer checks.
        // "no target" → false for every flavour (nothing to classify).
        if (cond == "target_is_player")
        {
            Unit* t = BotTarget(bot);
            return t && t->IsPlayer();
        }
        if (cond == "target_is_npc")
        {
            Unit* t = BotTarget(bot);
            return t && !t->IsPlayer();
        }
        if (cond == "target_is_boss")
        {
            Unit* t = BotTarget(bot);
            if (!t) return false;
            if (Creature* c = t->ToCreature())
                if (c->isWorldBoss()) return true;
            return UnitCreatureRank(t) == CREATURE_ELITE_WORLDBOSS;
        }
        if (cond == "target_is_elite")
        {
            int const r = UnitCreatureRank(BotTarget(bot));
            return r == CREATURE_ELITE_ELITE || r == CREATURE_ELITE_RAREELITE
                || r == CREATURE_ELITE_WORLDBOSS;
        }
        if (cond == "target_is_rare")
        {
            int const r = UnitCreatureRank(BotTarget(bot));
            return r == CREATURE_ELITE_RARE || r == CREATURE_ELITE_RAREELITE;
        }
        if (cond == "target_is_normal")
            return UnitCreatureRank(BotTarget(bot)) == CREATURE_ELITE_NORMAL;

        // Creature type — drives Banish (Demon/Elemental), Turn Undead,
        // Hibernate (Beast/Dragonkin), Polymorph (Beast/Humanoid), etc.
        if (cond.rfind("target_type_", 0) == 0)
        {
            Unit* t = BotTarget(bot);
            Creature* c = t ? t->ToCreature() : nullptr;
            if (!c) return false;
            uint32 const ct = c->GetCreatureType();
            std::string const which = cond.substr(std::strlen("target_type_"));
            if (which == "beast")     return ct == CREATURE_TYPE_BEAST;
            if (which == "dragonkin") return ct == CREATURE_TYPE_DRAGONKIN;
            if (which == "demon")     return ct == CREATURE_TYPE_DEMON;
            if (which == "elemental") return ct == CREATURE_TYPE_ELEMENTAL;
            if (which == "giant")     return ct == CREATURE_TYPE_GIANT;
            if (which == "undead")    return ct == CREATURE_TYPE_UNDEAD;
            if (which == "humanoid")  return ct == CREATURE_TYPE_HUMANOID;
            return false;
        }

        // --- Target cast / interrupt -------------------------------------
        // Pair `target_interruptible` with a `cast:<kick>` rule (Pummel,
        // Counterspell, Kick, Earth Shock, Shield Bash) so the bot only
        // spends the interrupt when there's actually a kickable cast.
        if (cond == "target_casting")
        {
            bool ch = false, ir = false;
            return CurrentCastSpell(BotTarget(bot), &ch, &ir) != nullptr && !ch;
        }
        if (cond == "target_channeling")
        {
            bool ch = false, ir = false;
            return CurrentCastSpell(BotTarget(bot), &ch, &ir) != nullptr && ch;
        }
        if (cond == "target_interruptible")
        {
            bool ch = false, ir = false;
            return CurrentCastSpell(BotTarget(bot), &ch, &ir) != nullptr && ir;
        }

        // --- Target movement ---------------------------------------------
        // For snares (Hamstring / Wing Clip / Concussive Shot) you only want
        // to spend the GCD when the target is actually running.
        if (cond == "target_moving")
        {
            Unit* t = BotTarget(bot);
            return t && t->isMoving();
        }
        if (cond == "target_not_moving")
        {
            Unit* t = BotTarget(bot);
            return t && !t->isMoving();
        }

        // --- Pet status --------------------------------------------------
        // Hunter / Warlock: keep the pet alive and summoned. `pet_health`
        // (the <N/>N form) is handled in the numeric section below.
        if (cond == "pet_exists")
        {
            Pet* p = bot->GetPet();
            return p && p->IsAlive();
        }
        if (cond == "pet_missing")
        {
            Pet* p = bot->GetPet();
            return !p;
        }
        if (cond == "pet_dead")
        {
            Pet* p = bot->GetPet();
            return p && !p->IsAlive();
        }

        // <name><op><N> where op is < or >
        auto opPos = cond.find_first_of("<>");
        if (opPos == std::string::npos) return false;
        std::string const name = cond.substr(0, opPos);
        char const op = cond[opPos];
        int const threshold = std::atoi(cond.substr(opPos + 1).c_str());

        auto pct = [](float cur, float max) -> int
        {
            if (max <= 0) return 0;
            return int((cur / max) * 100.0f);
        };

        auto cmp = [op, threshold](int v) -> bool
        {
            return op == '<' ? (v < threshold) : (v > threshold);
        };

        if (name == "self_health")
            return cmp(pct(float(bot->GetHealth()), float(bot->GetMaxHealth())));
        // Class-specific power pools — warriors don't have mana, so
        // self_mana on a warrior is a no-op. self_rage, self_energy, etc.
        // let you write proper rules per class. self_power is the generic
        // "whatever power this class actually uses".
        if (name == "self_rage")
            return cmp(pct(float(bot->GetPower(POWER_RAGE)), float(bot->GetMaxPower(POWER_RAGE))));
        if (name == "self_energy")
            return cmp(pct(float(bot->GetPower(POWER_ENERGY)), float(bot->GetMaxPower(POWER_ENERGY))));
        if (name == "self_focus")
            return cmp(pct(float(bot->GetPower(POWER_FOCUS)), float(bot->GetMaxPower(POWER_FOCUS))));
        if (name == "self_runic")
            return cmp(pct(float(bot->GetPower(POWER_RUNIC_POWER)), float(bot->GetMaxPower(POWER_RUNIC_POWER))));
        if (name == "self_power")
        {
            Powers const p = bot->getPowerType();
            uint32 const mx = bot->GetMaxPower(p);
            if (mx == 0) return false;
            return cmp(int((float(bot->GetPower(p)) / float(mx)) * 100.0f));
        }
        if (name == "self_mana")
        {
            // Diagnostic: throttle to once per 5 s per bot so we can see
            // why the rule isn't matching when the user expects it to.
            static thread_local std::unordered_map<uint32, uint32> lastLog;
            uint32 const nowMs = getMSTime();
            uint32& last = lastLog[bot->GetGUID().GetCounter()];
            int const manaPct = (bot->GetMaxPower(POWER_MANA) > 0)
                ? int((float(bot->GetPower(POWER_MANA))
                       / float(bot->GetMaxPower(POWER_MANA))) * 100.0f)
                : -1;
            if (nowMs - last > 5000)
            {
                last = nowMs;
                LOG_INFO("module",
                    "[WowPsParty Rotation] {} self_mana eval: powerType={} mana={}/{} pct={} cmp_op={} threshold={}",
                    bot->GetName(), uint32(bot->getPowerType()),
                    bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA),
                    manaPct, op, threshold);
            }
            // Original behaviour: only paladins/priests/mages/etc. with a
            // primary mana pool. Warriors/rogues should never match this.
            if (bot->getPowerType() != POWER_MANA) return false;
            if (manaPct < 0) return false;
            return cmp(manaPct);
        }
        if (name == "target_health")
        {
            Unit* tgt = bot->GetVictim();
            if (!tgt) return false;
            return cmp(pct(float(tgt->GetHealth()), float(tgt->GetMaxHealth())));
        }
        if (name == "party_lowest_health")
            return cmp(GetLowestPartyHpPercent(bot));

        // Role-tagged member mana — usually used as "wait for the healer".
        // Tank rule example:  healer_mana<75 | hold_position | 100
        auto pctOfMember = [&pct](Player* m) -> int
        {
            if (!m || m->getPowerType() != POWER_MANA) return -1;
            return pct(float(m->GetPower(POWER_MANA)), float(m->GetMaxPower(POWER_MANA)));
        };
        if (name == "healer_mana")
        {
            int const p = pctOfMember(FindPartyMemberByRole(bot, "healer"));
            if (p < 0) return false;
            return cmp(p);
        }
        if (name == "tank_health")
        {
            Player* m = FindPartyMemberByRole(bot, "tank");
            if (!m) return false;
            return cmp(pct(float(m->GetHealth()), float(m->GetMaxHealth())));
        }
        if (name == "shared_drink")
            return cmp(int(CountSharedConsumables(bot, /*drink=*/true)));
        if (name == "shared_food")
            return cmp(int(CountSharedConsumables(bot, /*drink=*/false)));

        // AoE / aggro counts. Two pre-canned distance bands so the user
        // doesn't have to spell a radius into the condition name.
        if (name == "enemies_in_melee")
            return cmp(int(CountHostilesWithin(bot, 8.0f)));
        if (name == "enemies_in_range")
            return cmp(int(CountHostilesWithin(bot, 30.0f)));

        // Time-to-die (seconds), estimated from the target's recent health
        // history. RAW seconds, not a percentage. No target / no data →
        // TTD_UNKNOWN (≈never), so `target_ttd>20` fires on an unmeasured
        // mob (assume a long fight) and `target_ttd<8` stays false — matching
        // Kevkili's `TargetTTD(...) or 999` default.
        if (name == "target_ttd")
        {
            Unit* t = bot->GetVictim();
            if (!t) return false;
            return cmp(TtdSeconds(t));
        }
        // Combo points are 0-5, compared RAW (not as a percent of anything).
        if (name == "self_combo")
            return cmp(int(bot->GetComboPoints()));
        // Pet health %, for Mend Pet gating.
        if (name == "pet_health")
        {
            Pet* p = bot->GetPet();
            if (!p || !p->IsAlive()) return false;
            return cmp(pct(float(p->GetHealth()), float(p->GetMaxHealth())));
        }
        // Distance (yards) from the bot to the controlled char it follows.
        // Gate maintenance casts on proximity, e.g. only conjure food/water
        // when "master_dist<15". If the leader can't be found / is off-map,
        // report a huge distance so "<" gates fail (don't conjure blindly).
        if (name == "master_dist")
        {
            ObjectGuid const lg = GetLeaderFor(bot->GetGUID());
            Player* leader = lg ? ObjectAccessor::FindConnectedPlayer(lg) : nullptr;
            if (!leader || !leader->IsInWorld() || leader->GetMapId() != bot->GetMapId())
                return op == '>';   // unknown → "far": '>' true, '<' false
            return cmp(int(bot->GetDistance(leader)));
        }

        return false;
    }

    // ----- action executor ----------------------------------------------------

    // Find a spell in the bot's spellbook by case-insensitive name, returning
    // the HIGHEST rank the bot actually knows. Returns 0 if not known.
    //
    // GetSpellMap() iterates in ascending spell-id order, and lower ranks have
    // lower ids, so naively returning the first name match handed back rank 1
    // every time — a level-20 mage was casting Rank 1 Conjure Water / Frostbolt
    // and the healer was casting Rank 1 heals. We now keep scanning and prefer
    // the entry with the greatest spell rank (ties → greater spell id, which is
    // the newer version for chainless spells).
    static uint32 FindKnownSpellByName(Player* bot, std::string const& name)
    {
        std::string needle;
        needle.reserve(name.size());
        for (char c : name) needle.push_back(std::tolower(static_cast<unsigned char>(c)));

        uint32 bestId   = 0;
        uint8  bestRank = 0;
        for (auto const& kv : bot->GetSpellMap())
        {
            if (kv.second->State == PLAYERSPELL_REMOVED) continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(kv.first);
            if (!info) continue;
            char const* spellName = info->SpellName[0];  // enUS slot
            if (!spellName) continue;
            std::string lower;
            for (char const* p = spellName; *p; ++p)
                lower.push_back(std::tolower(static_cast<unsigned char>(*p)));
            if (lower != needle) continue;

            uint8 const rank = sSpellMgr->GetSpellRank(kv.first);
            if (bestId == 0 || rank > bestRank ||
                (rank == bestRank && kv.first > bestId))
            {
                bestId   = kv.first;
                bestRank = rank;
            }
        }
        return bestId;
    }

    // The lead tank's RANGED pull ability, or 0 if it has none. Lets a tank with
    // no thrown/gun/bow weapon (a paladin carries a libram in the ranged slot, a
    // DK/druid nothing) still OPEN from range instead of charging the pack: the
    // engagement layer holds it at range and the rotation's matching `cast:` rule
    // fires the actual pull. IMPORTANT: each list is the spell the rotation
    // actually opens with, highest-known-rank first — because TankPullHoldRange
    // derives the hold stand-off from THIS spell's range, so it must be the one
    // that really fires (e.g. the DK opener is Icy Touch via its Frost Fever rule;
    // Death Grip / Dark Command are taunt/utility, not its has_target opener).
    uint32 TankRangedPullSpell(Player* bot)
    {
        if (!bot) return 0;
        std::vector<char const*> names;
        switch (bot->getClass())
        {
            case CLASS_WARRIOR:      names = { "Heroic Throw" }; break;
            case CLASS_PALADIN:      names = { "Avenger's Shield", "Hand of Reckoning", "Exorcism" }; break;
            case CLASS_DEATH_KNIGHT: names = { "Icy Touch" }; break;
            case CLASS_DRUID:        names = { "Faerie Fire (Feral)", "Faerie Fire" }; break;
            default: return 0;
        }
        for (char const* n : names)
            if (uint32 id = FindKnownSpellByName(bot, n))
                return id;
        return 0;
    }

    // The distance a ranged bot should hold at so its WHOLE single-target damage
    // kit is in range — the shortest-range offensive `cast:` in its rotation, sat
    // a couple yards inside, clamped to a sane ranged band. Without this the
    // engagement layer parked the bot anywhere up to 30y, so a caster idling at
    // ~28y could fire only its 30y filler and never closed to use a shorter nuke
    // ("ranged attackers don't get closer to cast an out-of-range ability"). 18y
    // floor so it never hugs the mob; 28y ceiling so a pure-30y kit still stands
    // back. Default 28 when the rotation has no ranged cast (or isn't cached yet).
    float BotRangedCastHold(Player* bot)
    {
        if (!bot) return 28.0f;
        uint32 const low = bot->GetGUID().GetCounter();
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> lock(g_rangedHoldMutex);
            auto it = g_rangedHoldCache.find(low);
            if (it != g_rangedHoldCache.end() && now - it->second.second < 10000)
                return it->second.first;
        }

        // Pull the offensive cast spell names out under the rotation lock, then do
        // the (lock-free) spell lookups so we don't hold g_rotationCacheMutex long.
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lock(g_rotationCacheMutex);
            auto it = g_rotationCache.find(low);
            if (it != g_rotationCache.end())
                for (RotationRule const& r : it->second)
                    if (r.action.rfind("cast:", 0) == 0)   // plain offensive cast, NOT cast_self/_party/_pet
                        names.push_back(r.action.substr(5));
        }

        float minRange = 0.0f;
        for (std::string const& name : names)
        {
            uint32 const id = FindKnownSpellByName(bot, name);
            if (!id) continue;
            SpellInfo const* si = sSpellMgr->GetSpellInfo(id);
            if (!si) continue;
            float const mr = si->GetMaxRange(false, bot);
            if (mr <= 6.0f) continue;   // a melee ability in the list — irrelevant to ranged hold
            if (minRange == 0.0f || mr < minRange) minRange = mr;
        }

        float hold = (minRange <= 0.0f) ? 28.0f : (minRange - 2.0f);
        if (hold < 18.0f) hold = 18.0f;
        if (hold > 28.0f) hold = 28.0f;
        {
            std::lock_guard<std::mutex> lock(g_rangedHoldMutex);
            g_rangedHoldCache[low] = { hold, now };
        }
        return hold;
    }

    // Stand-off the tank holds at to fire its opener: a few yards inside the pull
    // ability's actual reach so the cast doesn't fail at the very edge. A long
    // opener (Heroic Throw / Avenger's Shield / Faerie Fire, 30y) pulls from ~26y;
    // a DK on Icy Touch (20y) naturally holds closer (~16y) — no class special-
    // case, it just falls out of the range. The post-pull back-up is what creates
    // separation, so a closer stand-off for a short opener is fine. A ranged-
    // WEAPON puller (no ability id) falls back to the long hold.
    float TankPullHoldRange(Player* bot)
    {
        float maxRange = 30.0f;   // ranged weapon / unknown -> assume a long reach
        if (uint32 id = TankRangedPullSpell(bot))
            if (SpellInfo const* si = sSpellMgr->GetSpellInfo(id))
            {
                float const r = si->GetMaxRange(si->IsPositive(), bot);
                if (r > 0.0f) maxRange = r;
            }
        float hold = maxRange - 4.0f;
        if (hold < 12.0f) hold = 12.0f;
        if (hold > 26.0f) hold = 26.0f;
        return hold;
    }

    // Fire the bot's equipped PHYSICAL ranged weapon at `target`: gun/bow/crossbow
    // → "Shoot" (3018, auto-repeat); thrown → "Throw" (2764, single). Free (no
    // rage/mana), so it's a reliable pull opener for a fresh tank with ~0 rage.
    // Returns false (caller falls back to an ability) when there's no physical
    // ranged weapon, it's out of range/LoS, or the cast is rejected (e.g. no ammo
    // for a gun/bow). Shared by the `shoot` rotation verb and the lead-tank pull.
    bool FireRangedWeaponShot(Player* bot, Unit* target)
    {
        if (!bot || !target || !target->IsAlive()) return false;
        Item* const ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!ranged) return false;
        uint32 shootSpell = 0;
        bool   needsAmmo  = false;
        switch (ranged->GetTemplate()->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_GUN:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW: shootSpell = 3018; needsAmmo = true; break;
            case ITEM_SUBCLASS_WEAPON_THROWN:   shootSpell = 2764; break;
            default: return false;   // wand (use the `wand` verb) or nothing
        }
        // Guns/bows/crossbows need ammo loaded; bail BEFORE the cast (matches the
        // hunter Auto Shot helper) so a no-ammo tank falls back to its ability
        // instead of attempting — and failing — the shot every tick. Thrown
        // weapons are their own ammo, so they skip this.
        if (needsAmmo && bot->GetUInt32Value(PLAYER_AMMO_ID) == 0) return false;
        if (bot->GetDistance(target) > 30.0f) return false;
        if (!bot->IsWithinLOSInMap(target)) return false;
        // Already mid-shot? DON'T re-cast — re-issuing the shot every tick restarts
        // its wind-up/swing timer so it never lands ("interrupts itself forever,
        // animation restarts very fast"). Shoot (3018) and Throw (2764) live in the
        // CURRENT_AUTOREPEAT_SPELL slot once the auto-attack is established, but the
        // FIRST cast spends its wind-up in CURRENT_GENERIC_SPELL — which the old
        // autorepeat-only guard missed, so it kept re-casting the wind-up forever.
        // Check BOTH slots: if our shot is already in flight at this target, leave
        // it alone; if it's aimed at the wrong target, stop so next tick re-acquires.
        for (CurrentSpellTypes slot : { CURRENT_AUTOREPEAT_SPELL, CURRENT_GENERIC_SPELL })
            if (Spell* cur = bot->GetCurrentSpell(slot))
                if (cur->GetSpellInfo() && cur->GetSpellInfo()->Id == shootSpell)
                {
                    if (cur->m_targets.GetUnitTarget() == target)
                        return true;                      // already shooting this mob
                    bot->InterruptSpell(slot);
                    return false;                         // wrong target — restart next tick
                }
        bot->SetFacingToObject(target);   // ranged attack fails NOT_INFRONT otherwise
        return bot->CastSpell(target, shootSpell, false) == SPELL_CAST_OK;
    }

    // Lowercase a class name for case-insensitive matching in the
    // class-filter list.
    static std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Map a player class id to the lower-case keyword used in the
    // class-filter syntax ("warrior", "paladin", ...).
    static char const* ClassKeyword(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return "warrior";
            case CLASS_PALADIN:      return "paladin";
            case CLASS_HUNTER:       return "hunter";
            case CLASS_ROGUE:        return "rogue";
            case CLASS_PRIEST:       return "priest";
            case CLASS_DEATH_KNIGHT: return "deathknight";
            case CLASS_SHAMAN:       return "shaman";
            case CLASS_MAGE:         return "mage";
            case CLASS_WARLOCK:      return "warlock";
            case CLASS_DRUID:        return "druid";
            default:                 return "";
        }
    }

    // True if `kw` appears in the comma-separated, lowercase list `csv`.
    static bool CsvContains(std::string const& csv, std::string const& kw)
    {
        size_t p = 0;
        while (p <= csv.size())
        {
            size_t c = csv.find(',', p);
            std::string tok = (c == std::string::npos)
                ? csv.substr(p) : csv.substr(p, c - p);
            // trim leading whitespace
            while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
            while (!tok.empty() && tok.back()  == ' ') tok.pop_back();
            if (tok == kw) return true;
            if (c == std::string::npos) break;
            p = c + 1;
        }
        return false;
    }

    // Find first party member matching a class-list filter and missing the
    // named spell's aura. classCsv is comma-separated lowercase class
    // keywords ("priest,mage,warlock,druid").
    static Player* FindClassFilteredMissing(Player* bot,
        std::string const& classCsv, uint32 spellId)
    {
        if (!bot || !spellId) return nullptr;
        std::string const csv = Lower(classCsv);
        std::vector<Player*> party;
        GatherPartyPlayers(bot, party, /*includeDead=*/false);
        for (Player* m : party)
        {
            char const* kw = ClassKeyword(m->getClass());
            if (!*kw) continue;
            if (!CsvContains(csv, kw)) continue;
            if (HasAuraFromSpell(m, spellId)) continue;
            return m;
        }
        return nullptr;
    }

    // Find first party member matching a role filter and missing the named
    // spell's aura. roleFilter is "tank" / "healer" / "dps" / "!tank" etc.
    static Player* FindRoleFilteredMissing(Player* bot,
        std::string const& roleFilter, uint32 spellId)
    {
        if (!bot || !bot->GetSession() || !spellId) return nullptr;
        bool negate = !roleFilter.empty() && roleFilter[0] == '!';
        std::string wantedRole = negate ? roleFilter.substr(1) : roleFilter;
        wantedRole = Lower(wantedRole);

        uint32 const account = bot->GetSession()->GetAccountId();
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid`, COALESCE(`role`, 'dps') FROM `account_party` "
            "WHERE `account` = {}", account);
        if (!q) return nullptr;
        do
        {
            uint32 const memberGuid = q->Fetch()[0].Get<uint32>();
            std::string memberRole = q->Fetch()[1].Get<std::string>();
            memberRole = Lower(memberRole);
            bool const matches = (memberRole == wantedRole);
            bool const include = negate ? !matches : matches;
            if (!include) continue;
            Player* m = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(memberGuid));
            if (!m || !m->IsAlive() || !m->IsInWorld()) continue;
            if (m->GetMapId() != bot->GetMapId()) continue;
            if (HasAuraFromSpell(m, spellId)) continue;
            return m;
        } while (q->NextRow());
        return nullptr;
    }

    // Walk the bot's group looking for the first member missing the named
    // spell's aura. Returns nullptr if every member already has it.
    static Player* FindPartyMemberMissingAura(Player* bot, uint32 spellId)
    {
        if (!bot || !spellId) return nullptr;
        std::vector<Player*> party;
        GatherPartyPlayers(bot, party, /*includeDead=*/false);
        for (Player* m : party)
            if (!HasAuraFromSpell(m, spellId)) return m;
        return nullptr;
    }

    static bool ExecAction(std::string const& act, Player* bot,
                           std::string const& flags)
    {
        // action format: <verb>:<arg>
        auto colon = act.find(':');
        std::string verb = act.substr(0, colon);
        std::string arg  = colon == std::string::npos ? "" : act.substr(colon + 1);

        // "clip" flag: let this cast interrupt the bot's own in-progress
        // cast/channel. Without it, a rule is skipped while the bot is
        // mid-cast (the default — don't clip your own Frostbolt). With it,
        // a higher-priority reactive rule (Counterspell, an emergency heal)
        // cancels the current cast and fires. `channelClipOk` is the single
        // gate every cast verb runs through instead of the bare
        // IsNonMeleeSpellCast check.
        bool const allowClip = CsvContains(Lower(flags), "clip");
        auto channelClipOk = [bot, allowClip]() -> bool
        {
            // skipAutorepeat=true: a hunter's Auto Shot auto-repeat is NOT a
            // "cast in progress" you must wait for — special shots are woven
            // BETWEEN auto-shots. Without skipping it, IsNonMeleeSpellCast was
            // true the whole time the bot auto-shot, so every non-clip special
            // shot (Steady/Arcane/Serpent Sting...) was skipped and the hunter
            // "only auto-attacked, everything exec_failed" (cast never even
            // attempted → lastCastResult stayed 0). Still blocks clipping a real
            // generic/channeled cast (those aren't auto-repeat, so not skipped).
            if (!bot->IsNonMeleeSpellCast(false, false, /*skipAutorepeat=*/true))
                return true;
            if (!allowClip) return false;
            bot->InterruptNonMeleeSpells(false);
            return true;
        };

        // A cast is only "really fireable" when EVERYTHING checks out:
        // cooldown clear, enough mana/power, within range, line of sight,
        // target alive + valid. Any failure → rule returns false and the
        // rotation drops to the next lower-priority rule. Without this,
        // an `always | cast: Holy Light` on a low-mana paladin would
        // halt the loop every tick (rule "fired" but actual cast failed
        // silently), starving every lower-priority rule.
        // Why the last canFireSpellOn() rejected, so the caller can decide
        // whether to reposition. POSITION = the cast would succeed if only
        // the bot were closer / had line of sight (walk toward the target);
        // HARD = cooldown / power / stance / GCD (no point moving, fall
        // through to a lower-priority rule).
        enum class CastBlock { None, Hard, Position };
        CastBlock castBlock = CastBlock::None;
        // Result of the most recent faceAndCast(), so castOrApproach can tell a
        // positional server-side rejection (walk in and retry) from a real one.
        SpellCastResult lastCastResult = SPELL_CAST_OK;

        auto canFireSpellOn = [bot, &castBlock](uint32 spellId, Unit* target) -> bool
        {
            castBlock = CastBlock::Hard;
            // Rate-limited rejection logger. When a rule's condition
            // matches but the cast never fires, the per-tick trace only
            // says "exec_failed_falling_through" — it can't see WHICH of
            // the gates below bailed. This pins the exact reason (stance,
            // GCD, cooldown, power, range, LoS) without spamming: at most
            // one line per (bot,spell) every 3 s.
            auto reject = [bot, spellId](char const* why) -> bool
            {
                static thread_local std::unordered_map<uint64, uint32> lastMs;
                uint64 const key = (uint64(bot->GetGUID().GetCounter()) << 32) | spellId;
                uint32 const now = getMSTime();
                uint32& last = lastMs[key];
                if (now - last > 3000)
                {
                    last = now;
                    LOG_INFO("module",
                        "[WowPsParty Rotation]     canFire spell={} on guid={}: REJECT ({})",
                        spellId, bot->GetGUID().GetCounter(), why);
                }
                return false;
            };

            if (!spellId || !target) return reject("no spell or target");
            if (bot->HasSpellCooldown(spellId)) return reject("on cooldown");

            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info) return reject("no SpellInfo");

            // GCD: AC stores it in a per-player GlobalCooldownMgr, NOT in
            // the regular cooldown map. StartRecoveryCategory alone misses
            // it. Without this check the mage would happily try to cast
            // Frostbolt during a GCD, fail with SPELL_FAILED_NOT_READY
            // (result=67), and the rotation would fall through to Shoot.
            if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
                return reject("global cooldown");
            if (info->StartRecoveryCategory > 0 &&
                bot->HasSpellCooldown(info->StartRecoveryCategory))
                return reject("category cooldown");

            // Shapeshift / stance gate. Without this, a battle-stance
            // warrior with a Taunt rule (requires Defensive Stance)
            // fires CastSpell every tick, server replies with
            // SPELL_FAILED_ONLY_SHAPESHIFT (94), and the rule loops
            // forever instead of falling through to Thunder Clap /
            // Heroic Strike. SpellInfo::CheckShapeshift handles the
            // full mask (Stances + StancesNot + cancellable-form bits).
            if (info->CheckShapeshift(uint32(bot->GetShapeshiftForm())) != SPELL_CAST_OK)
            {
                // Spell out the mismatch so we know which stance to dance
                // into: current form vs the spell's required/forbidden masks.
                static thread_local std::unordered_map<uint64, uint32> lastFormMs;
                uint64 const fkey = (uint64(bot->GetGUID().GetCounter()) << 32) | spellId;
                uint32 const fnow = getMSTime();
                uint32& flast = lastFormMs[fkey];
                if (fnow - flast > 3000)
                {
                    flast = fnow;
                    LOG_INFO("module",
                        "[WowPsParty Rotation]     spell={} stance-block: curForm={} Stances={:#x} StancesNot={:#x}",
                        spellId, uint32(bot->GetShapeshiftForm()),
                        info->Stances, info->StancesNot);
                }
                return reject("wrong stance/form");
            }

            // Power cost (mana / rage / energy / etc).
            int32 const cost = info->CalcPowerCost(bot, info->GetSchoolMask());
            if (cost > 0)
            {
                Powers const powerType = Powers(info->PowerType);
                if (int32(bot->GetPower(powerType)) < cost)
                    return reject("not enough power");
            }

            // Self-casts don't need range / LoS checks.
            if (target != bot)
            {
                float const maxRange = info->GetMaxRange(info->IsPositive(), bot);
                if (maxRange > 0 && !bot->IsWithinDistInMap(target, maxRange))
                {
                    castBlock = CastBlock::Position;
                    return reject("out of range");
                }
                // Ranged dead-zone: a hunter shot etc. has a minimum range and
                // fails SPELL_FAILED_TOO_CLOSE in melee. Treat as a positioning
                // block so the bot backs OUT to range (repositionToCast handles
                // the direction) instead of firing it every tick and failing.
                // CRITICAL: match the server (Spell::CheckRange) — the EFFECTIVE
                // minimum is the spell's min range PLUS melee range, so a special
                // shot fails ~10y out for a normal mob (5y spell + ~5y melee), not
                // 5y. The old raw-min check passed at ~9.6y, the shot then failed
                // server-side, and the bot stood there auto-attacking forever
                // ("abilities exec_fail"). Auto Shot uses a different range path,
                // which is why only the SPECIAL shots were affected.
                float const minRange = info->GetMinRange(info->IsPositive());
                if (minRange > 0.0f
                    && bot->IsWithinRange(target, minRange + bot->GetMeleeRange(target)))
                {
                    castBlock = CastBlock::Position;
                    return reject("too close");
                }
                if (!bot->IsWithinLOSInMap(target))
                {
                    castBlock = CastBlock::Position;
                    return reject("no line of sight");
                }
            }

            castBlock = CastBlock::None;
            return true;
        };

        // Backwards-compat shim for self-cast / no-target validations.
        auto canFireSpell = [&canFireSpellOn, bot](uint32 spellId) -> bool
        {
            return canFireSpellOn(spellId, bot);
        };

        // Most directed spells require the caster to face the target. The
        // bot is server-driven and doesn't physically rotate on its own
        // unless MoveChase happens to be steering it, so make the cast
        // path responsible for facing every time.
        //
        // Bots are Player units, so the server doesn't auto-halt their
        // motion when a cast starts (that's normally driven by the
        // client). If we leave MoveChase/MoveFollow running while a
        // cast-time spell is in flight, the motion update clears
        // UNIT_STATE_CASTING and the spell is interrupted. So for any
        // spell with a cast time, freeze the bot here. PartyFollow's
        // per-tick re-asserter already skips while IsNonMeleeSpellCast,
        // so once the cast finishes, formation movement resumes
        // naturally on the next tick.
        // Returns true if the cast was actually issued and accepted by the
        // server. False means the spell failed validation (bad target,
        // immune, out of LoS at cast time, etc.) — the caller treats that
        // as "rule didn't fire" so the rotation falls through to the next
        // lower-priority rule.
        auto faceAndCast = [bot, &lastCastResult](Unit* target, uint32 spellId) -> bool
        {
            int32 castMs = 0;
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                castMs = info->CalcCastTime();
            // Plant ONLY for cast-time spells: a moving Player's motion update
            // clears UNIT_STATE_CASTING and interrupts the cast, so freeze for
            // the cast bar. Instant shots are NOT planted — doing so on every
            // tick fought AssistTarget's chase (StopMoving+Clear, then the chase
            // re-installs) and made the hunter stutter in place. A ranged bot
            // settles at its standoff and shoots while stationary; the only cost
            // is a transient UNIT_NOT_INFRONT on a shot fired mid-reposition,
            // which simply retries next tick once planted at range.
            if (target && target != bot && castMs > 0)
            {
                bot->StopMoving();
                bot->GetMotionMaster()->Clear();
                WowPsParty::HoldFollower(bot->GetGUID(),
                    uint32(castMs) + 500);
            }
            if (target && target != bot)
                bot->SetFacingToObject(target);
            SpellCastResult const r = bot->CastSpell(target, spellId, false);
            lastCastResult = r;
            if (r != SPELL_CAST_OK)
            {
                // Throttle per (bot, spell) — a cast that keeps failing must not
                // flood the log every tick.
                static thread_local std::unordered_map<uint64, uint32> failLogMs;
                uint64 const fk = (uint64(bot->GetGUID().GetCounter()) << 32) | spellId;
                uint32 const fn = getMSTime();
                uint32& fl = failLogMs[fk];
                if (fn - fl > 5000)
                {
                    fl = fn;
                    LOG_INFO("module",
                        "[WowPsParty Rotation] cast {} failed on guid={} result={}",
                        spellId, bot->GetGUID().GetCounter(), uint32(r));
                }
                return false;
            }
            return true;
        };

        // Ground-targeted AoE (Blizzard / Flamestrike / Rain of Fire / Volley):
        // cast at a WORLD POSITION rather than a unit, so it lands on the mob
        // cluster instead of just the current victim. Same cast-time freeze as
        // faceAndCast.
        auto faceAndCastAt = [bot](Unit* aimAt, uint32 spellId) -> bool
        {
            if (aimAt) bot->SetFacingToObject(aimAt);
            int32 castMs = 0;
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                castMs = info->CalcCastTime();
            if (castMs > 0)
            {
                bot->StopMoving();
                bot->GetMotionMaster()->Clear();
                WowPsParty::HoldFollower(bot->GetGUID(), uint32(castMs) + 500);
            }
            float x, y, z;
            aimAt->GetPosition(x, y, z);
            // Predictive placement: a mob group charging the casters walks out of a
            // Flamestrike dropped on its CURRENT spot before the ~2s cast lands. Aim
            // where the cluster WILL be when the cast ends — `lead` yards ahead along
            // the anchor's facing (= its move direction), like a real player leading
            // the throw. Capped so a sudden turn can't fling the AoE across the map;
            // GetNearPoint settles z onto the ground at the lead point.
            if (castMs > 0 && aimAt->isMoving())
            {
                float lead = aimAt->GetSpeed(MOVE_RUN) * (float(castMs) / 1000.0f);
                if (lead > 1.0f)
                {
                    lead = std::min(lead, 18.0f);
                    aimAt->GetNearPoint(nullptr, x, y, z, 0.0f, lead, aimAt->GetOrientation());
                }
            }
            SpellCastResult const r = bot->CastSpell(x, y, z, spellId, false);
            if (r != SPELL_CAST_OK)
            {
                static thread_local std::unordered_map<uint64, uint32> gFailLogMs;
                uint64 const fk = (uint64(bot->GetGUID().GetCounter()) << 32) | spellId;
                uint32 const fn = getMSTime();
                uint32& fl = gFailLogMs[fk];
                if (fn - fl > 5000)
                {
                    fl = fn;
                    LOG_INFO("module",
                        "[WowPsParty Rotation] ground-cast {} failed on guid={} result={}",
                        spellId, bot->GetGUID().GetCounter(), uint32(r));
                }
                return false;
            }
            return true;
        };

        // Walk toward a cast target the bot can't yet reach. MoveChase routes
        // around corners via the navmesh, so a healer whose target is behind a
        // wall rounds the corner until line of sight clears instead of standing
        // still spamming a blocked cast. Throttled so we don't recompute the
        // path every UpdateAI tick (that stutters the bot in place). Returns
        // true — committing to the approach counts as the rule "firing", which
        // also stops the rotation from dropping to a worse lower-priority rule.
        auto repositionToCast = [bot](Unit* target, uint32 spellId) -> bool
        {
            if (!target || target == bot) return false;

            // Mid ranged-pull: never chase a MELEE ability into the pack — let the
            // rule fall through (to Heroic Throw etc.) and AssistTarget's pull-hold
            // keep the tank at throwing range. Without this the rotation's Shield
            // Slam would drag the tank straight in, defeating the pull.
            if (WowPsParty::IsTankPulling(bot->GetGUID()))
            {
                float maxR = 0.0f;
                if (SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId))
                    maxR = si->GetMaxRange(si->IsPositive(), bot);
                if (maxR > 0.0f && maxR <= 6.0f) return false;   // melee spell: hold, don't approach
            }

            // Suppress the follow/assist ticker for the approach window.
            WowPsParty::HoldFollower(bot->GetGUID(), 1200);

            // Too close for a min-range (ranged) spell? Back STRAIGHT OUT to
            // just past min range rather than chasing inward. Without this a
            // hunter shoved into melee chases the mob forever and every shot
            // fails TOO_CLOSE.
            float minRange = 0.0f;
            if (SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId))
                minRange = si->GetMinRange(si->IsPositive());
            // EFFECTIVE min = spell min + melee range (matches the server, see
            // canFireSpellOn). Standoff = just past it, so the bot ends up only a
            // little outside shooting range (~13y for a normal mob) instead of a
            // fixed 16y, and scales correctly for big mobs.
            float const effMinRange = minRange > 0.0f ? minRange + bot->GetMeleeRange(target) : 0.0f;
            bool const tooClose = effMinRange > 0.0f && bot->IsWithinRange(target, effMinRange);

            uint32 const gLow = bot->GetGUID().GetCounter();
            uint32 const tLow = target->GetGUID().GetCounter();
            uint32 const now  = getMSTime();
            // MoveChase only drives the bot while the chased unit is its VICTIM:
            // ChaseMovementGenerator::HasLostTarget is `GetVictim() != target`,
            // so it StopMoving()s the instant the target isn't who we're fighting.
            // A FRIENDLY buff/rez target is never the victim, so chasing it froze
            // the bot just outside range ("walks to 49.9y and stops, won't close").
            // FollowMovementGenerator has no such check — use MoveFollow for any
            // non-victim target; keep MoveChase for the offensive case.
            bool const targetIsVictim = (bot->GetVictim() == target);

            bool reissue = true;
            {
                std::lock_guard<std::mutex> lock(g_useThrottleMutex);
                auto& e = g_approachState[gLow];   // (targetGuidLow, lastMoveMs)
                MovementGeneratorType const mg =
                    bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
                // Already advancing on the SAME target? POINT for a back-out,
                // CHASE for an offensive approach, FOLLOW for a friendly one.
                bool const moving = tooClose      ? (mg == POINT_MOTION_TYPE)
                                  : targetIsVictim ? (mg == CHASE_MOTION_TYPE)
                                                   : (mg == FOLLOW_MOTION_TYPE);
                if (e.first == tLow && (now - e.second) < 700 && moving)
                    reissue = false;
                else { e.first = tLow; e.second = now; }
            }

            if (reissue)
            {
                if (tooClose)
                {
                    // Back STRAIGHT OUT to just past the effective min range
                    // (+3y), so the bot clears the dead zone with a small margin
                    // and settles close rather than at a fixed 16y.
                    float bx, by, bz;
                    target->GetNearPoint(bot, bx, by, bz, 0.0f,
                        effMinRange + 3.0f, target->GetAngle(bot));
                    bot->GetMotionMaster()->MovePoint(0, bx, by, bz);
                }
                else
                {
                    float maxRange = 0.0f;
                    if (SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId))
                        maxRange = si->GetMaxRange(si->IsPositive(), bot);
                    bool const meleeSpell = maxRange > 0.0f && maxRange <= 6.0f;
                    // How close to settle: contact for a melee spell, right in to
                    // clear a corner when LoS is blocked, else just inside max range.
                    float const settleDist =
                        meleeSpell                       ? 0.0f
                        : !bot->IsWithinLOSInMap(target) ? 2.0f
                                                         : maxRange - 3.0f;
                    if (!targetIsVictim)
                        bot->GetMotionMaster()->MoveFollow(target, settleDist, 0.0f);
                    else if (meleeSpell)
                        bot->GetMotionMaster()->MoveChase(target);   // chase to contact
                    else
                        bot->GetMotionMaster()->MoveChase(target, settleDist);
                }
            }

            static thread_local std::unordered_map<uint64, uint32> lastLog;
            uint64 const lkey = (uint64(gLow) << 32) | spellId;
            uint32& llast = lastLog[lkey];
            if (now - llast > 3000)
            {
                llast = now;
                LOG_INFO("module",
                    "[WowPsParty Rotation] {} approaching target guid={} for spell={} "
                    "(dist={:.1f} los={})",
                    bot->GetName(), tLow, spellId, bot->GetDistance(target),
                    bot->IsWithinLOSInMap(target) ? 1 : 0);
            }
            return true;
        };

        // Validate + cast a directed spell; if the ONLY blocker is range or
        // line of sight, walk toward the target instead of bailing.
        //
        // `friendlyApproach`: only for casts on an ALLY (buffs/rez/cure). When a
        // pre-approved cast is rejected by the server for a POSITIONAL reason,
        // walk in and retry. We do NOT do this for offensive casts on the bot's
        // victim — AssistTarget owns ranged/melee positioning there, and a shot
        // that briefly fails NOT_INFRONT/LoS mid-move would otherwise call
        // repositionToCast → HoldFollower, which makes AssistTarget YIELD, so the
        // rotation drags the bot into melee and it never settles to shoot ("only
        // auto-attacks, everything NO_MATCHes"). Offensive casts just fall through
        // and let AssistTarget reposition.
        // Throttled diagnostic for the friendly buff/rez approach path: names
        // which branch decided NOT to walk in, so a "bots don't move to
        // buff/rez" report can be pinned without guessing. At most one line per
        // (bot,spell) every 3 s.
        auto logFriendly = [bot](uint32 spellId, Unit* target, char const* what,
                                 uint32 detail = 0) {
            static thread_local std::unordered_map<uint64, uint32> lastMs;
            uint64 const key = (uint64(bot->GetGUID().GetCounter()) << 32) | spellId;
            uint32 const now = getMSTime();
            uint32& last = lastMs[key];
            if (now - last <= 3000) return;
            last = now;
            LOG_INFO("module",
                "[WowPsParty Rotation] {} friendly-cast spell={} target={} dist={:.1f}: {} (result={})",
                bot->GetName(), spellId,
                target ? target->GetGUID().GetCounter() : 0u,
                target ? bot->GetDistance(target) : -1.0f, what, detail);
        };

        auto castOrApproach = [&](Unit* target, uint32 spellId,
                                  bool friendlyApproach = false) -> bool
        {
            if (canFireSpellOn(spellId, target))
            {
                if (!channelClipOk()) return false;
                if (faceAndCast(target, spellId)) return true;
                // Pre-check OK'd it but the server rejected it. For a FRIENDLY
                // target whose GetMaxRange/IsPositive under-reports the real range
                // (so the range pre-check was skipped), a positional failure means
                // "walk in and retry". Non-positional (immune, already has a better
                // aura, ...) falls through so we don't chase pointlessly.
                if (friendlyApproach)
                {
                    switch (lastCastResult)
                    {
                        case SPELL_FAILED_OUT_OF_RANGE:
                        case SPELL_FAILED_LINE_OF_SIGHT:
                        case SPELL_FAILED_UNIT_NOT_INFRONT:
                        case SPELL_FAILED_TOO_CLOSE:
                            logFriendly(spellId, target, "server reject -> approaching",
                                        uint32(lastCastResult));
                            return repositionToCast(target, spellId);
                        default:
                            logFriendly(spellId, target,
                                        "cast failed, not positional -> giving up",
                                        uint32(lastCastResult));
                            break;
                    }
                }
                return false;
            }
            if (castBlock == CastBlock::Position)
            {
                // Only a FRIENDLY cast self-repositions here. For an OFFENSIVE
                // cast on the victim, fall through (return false) so the rule
                // fails and AssistTarget owns positioning — its melee chase for a
                // tank, its ranged firing band for a caster. Calling
                // repositionToCast here would set HoldFollower, making AssistTarget
                // YIELD ("skip: held by rotation"); the bot is then held but never
                // actually closes, standing out of range while every rule reports
                // exec_failed_falling_through (the "tank stuck at 21y" bug).
                if (friendlyApproach)
                {
                    logFriendly(spellId, target, "pre-check out of range/LoS -> approaching");
                    return repositionToCast(target, spellId);
                }
                return false;
            }
            if (friendlyApproach)
                logFriendly(spellId, target, "hard block (cooldown/power/stance) -> not casting");
            return false;
        };

        if (verb == "cast" || verb == "cast_self")
        {
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;

            // Ground-targeted AoE: aim at the densest cluster within the
            // spell's own radius (not the victim). Detected via the spell's
            // explicit DEST_LOCATION target flag.
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (verb == "cast" && info
                && (info->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION))
            {
                float radius = 0.0f;
                for (uint8 i = 0; i < 3; ++i)   // MAX_SPELL_EFFECTS
                {
                    float const er = info->Effects[i].CalcRadius(bot);
                    if (er > radius) radius = er;
                }
                if (radius <= 0.0f) radius = 8.0f;
                Unit* anchor = BestClusterAnchor(bot, radius);
                if (!anchor) anchor = bot->GetVictim();   // no cluster → victim spot
                if (!anchor) return false;
                if (!canFireSpellOn(spellId, anchor)) return false;
                if (!channelClipOk()) return false;
                return faceAndCastAt(anchor, spellId);
            }

            Unit* target = (verb == "cast_self") ? bot : bot->GetVictim();
            if (!target) return false;
            // Offensive cast on our victim: AssistTarget owns the positioning, so
            // DON'T approach on a positional server reject (default friendlyApproach
            // = false) — repositioning here would fight AssistTarget and pull the
            // ranged bot into melee.
            return castOrApproach(target, spellId);
        }

        if (verb == "buff_self")
        {
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            // Self-skip by NAME, not by the cast spell id. Seals (and some other
            // buffs) apply an aura whose spell id differs from the spell you cast,
            // so HasAura(castId) stayed false and the paladin re-cast Seal of
            // Righteousness every tick, never falling through to Crusader Strike.
            if (TargetHasNamedAura(bot, arg)) return false;
            if (!canFireSpellOn(spellId, bot)) return false;
            if (!channelClipOk()) return false;
            return faceAndCast(bot, spellId);
        }

        // "cast_pet:<spell>" — cast a spell that targets the bot's own pet
        // (Mend Pet). Call Pet / Revive Pet are self-cast and go through the
        // ordinary cast_self verb gated by pet_missing / pet_dead.
        if (verb == "cast_pet")
        {
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            Pet* pet = bot->GetPet();
            if (!pet || !pet->IsAlive()) return false;
            if (!canFireSpellOn(spellId, pet)) return false;
            if (!channelClipOk()) return false;
            return faceAndCast(pet, spellId);
        }

        if (verb == "cast_party_lowest" || verb == "cast_party_lowest_hot")
        {
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            Player* target = GetLowestHpPartyMember(bot);
            if (!target) return false;
            if (verb == "cast_party_lowest_hot" && HasAuraFromSpell(target, spellId))
                return false;
            return castOrApproach(target, spellId, /*friendlyApproach=*/true);
        }

        // "cast_class_missing:<classes>:<spell>" — cast spell on the first
        // party member of one of the listed classes who's missing the
        // spell's aura. Used for class-targeted buffs (BoW on casters,
        // BoM on melee, etc.). `arg` is "<classes>:<spell>".
        if (verb == "cast_class_missing")
        {
            auto inner = arg.find(':');
            if (inner == std::string::npos) return false;
            std::string const classes  = arg.substr(0, inner);
            std::string const spellNm  = arg.substr(inner + 1);
            uint32 const spellId = FindKnownSpellByName(bot, spellNm);
            if (!spellId) return false;
            Player* target = FindClassFilteredMissing(bot, classes, spellId);
            if (!target) return false;
            return castOrApproach(target, spellId, /*friendlyApproach=*/true);
        }

        // "cast_role_missing:<role>:<spell>" — same idea but filter by the
        // role you assigned in the Party Roster (tank / healer / dps). The
        // `!` prefix negates: "!tank" matches everyone EXCEPT tanks.
        if (verb == "cast_role_missing")
        {
            auto inner = arg.find(':');
            if (inner == std::string::npos) return false;
            std::string const roleFilter = arg.substr(0, inner);
            std::string const spellNm    = arg.substr(inner + 1);
            uint32 const spellId = FindKnownSpellByName(bot, spellNm);
            if (!spellId) return false;
            Player* target = FindRoleFilteredMissing(bot, roleFilter, spellId);
            if (!target) return false;
            return castOrApproach(target, spellId, /*friendlyApproach=*/true);
        }

        if (verb == "cast_party_missing")
        {
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            Player* target = FindPartyMemberMissingAura(bot, spellId);
            if (!target) return false;
            return castOrApproach(target, spellId, /*friendlyApproach=*/true);
        }

        // "cast_loose_enemy:<spell>" — cast the spell on the nearest hostile
        // that's currently attacking an ally instead of the bot. Pairs with
        // the `enemy_loose_in_*` condition for tank taunt-the-loose-mob
        // rules. The radius is 12y by default — long enough that the tank
        // can catch a Defias archer shooting the healer from across a room.
        if (verb == "cast_loose_enemy")
        {
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            Unit* target = FindLooseEnemy(bot, 30.0f);
            if (!target) return false;
            return castOrApproach(target, spellId, /*friendlyApproach=*/true);
        }

        // "pull:<spell>" — initiate a RANGED pull. Picks the nearest hostile
        // within 30y that ISN'T in combat yet and casts <spell> at it (Throw,
        // Shoot, Hunter's Mark, etc.) WITHOUT closing to melee. Pair with an
        // `out_of_combat` condition so it pulls one mob, then the rotation /
        // engagement system takes over once that mob aggros. Lets a warrior
        // tank pull with Throw instead of charging the whole room.
        if (verb == "pull")
        {
            if (bot->IsInCombat()) return false;
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            std::list<Unit*> hostiles;
            GatherHostilesAround(bot, 30.0f, hostiles);
            Unit* best = nullptr;
            float bestD = 1e9f;
            for (Unit* u : hostiles)
            {
                if (!u || !u->IsAlive() || u->IsInCombat()) continue;  // only un-engaged
                if (!bot->IsValidAttackTarget(u)) continue;
                float const d = bot->GetDistance(u);
                if (d < bestD) { bestD = d; best = u; }
            }
            if (!best) return false;
            if (!canFireSpellOn(spellId, best)) return false;
            if (!channelClipOk()) return false;
            bot->SetTarget(best->GetGUID());   // so combat/AssistTarget picks it up
            return faceAndCast(best, spellId); // ranged: faceAndCast doesn't run in
        }

        if (verb == "rez_party")
        {
            if (bot->IsInCombat()) return false;
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            Player* target = FindDeadPartyMember(bot);
            if (!target) return false;
            return castOrApproach(target, spellId, /*friendlyApproach=*/true);
        }

        // "cure_party:<spell>" — cast `spell` on the first afflicted member.
        // We infer the dispel type from the spell template (Cure Disease →
        // DISPEL_DISEASE, Dispel Magic → DISPEL_MAGIC, Remove Curse →
        // DISPEL_CURSE, Cure Poison / Cleanse → POISON, etc.). The spell
        // itself is the cure; we just need to pick the right target.
        if (verb == "cure_party")
        {
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info) return false;
            // Walk the spell's effects to find what dispel type it actually
            // removes (Effect == SPELL_EFFECT_DISPEL + MiscValue == dispel
            // type). This is more reliable than guessing from name.
            DispelType targetType = DISPEL_NONE;
            for (uint8 i = 0; i < 3; ++i)
            {
                SpellEffectInfo const& eff = info->Effects[i];
                if (eff.Effect == SPELL_EFFECT_DISPEL)
                {
                    targetType = DispelType(eff.MiscValue);
                    break;
                }
            }
            if (targetType == DISPEL_NONE) return false;
            Player* target = FindPartyMemberWithDispelType(bot, targetType);
            if (!target) return false;
            return castOrApproach(target, spellId, /*friendlyApproach=*/true);
        }

        // "use_item:<item name>" — pop a potion / healthstone / bandage from
        // the bot's own bags, or fire an equipped on-use trinket. Casts the
        // item's ON_USE spell on the bot and (for bag consumables) eats one
        // charge. Throttled per (bot,item) by the item's spell cooldown (60s
        // floor) so a `self_health<30 | use_item:Healing Potion` rule doesn't
        // burn the whole stack while HP is still low.
        if (verb == "use_item")
        {
            std::string const needle = Lower(arg);
            auto matchName = [&](Item* it) -> bool
            {
                ItemTemplate const* t = it ? it->GetTemplate() : nullptr;
                return t && Lower(t->Name1) == needle;
            };
            Item* found = nullptr;
            bool inBag = false;
            for (uint8 s = INVENTORY_SLOT_ITEM_START; s < INVENTORY_SLOT_ITEM_END && !found; ++s)
                if (Item* it = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s))
                    if (matchName(it)) { found = it; inBag = true; }
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END && !found; ++b)
                if (Bag* bag = bot->GetBagByPos(b))
                    for (uint32 j = 0; j < bag->GetBagSize() && !found; ++j)
                        if (Item* it = bot->GetItemByPos(b, j))
                            if (matchName(it)) { found = it; inBag = true; }
            if (!found)  // equipped on-use item (trinket, etc.)
                for (uint8 e = EQUIPMENT_SLOT_START; e < EQUIPMENT_SLOT_END && !found; ++e)
                    if (Item* it = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, e))
                        if (matchName(it)) found = it;
            if (!found) return false;

            ItemTemplate const* t = found->GetTemplate();
            uint32 useSpell = 0;
            int32  cdMs     = 0;
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                if (t->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE
                    && t->Spells[i].SpellId > 0)
                {
                    useSpell = uint32(t->Spells[i].SpellId);
                    cdMs     = t->Spells[i].SpellCooldown;
                    break;
                }
            }
            if (!useSpell) return false;
            if (bot->HasAura(useSpell)) return false;   // buff already up

            static std::unordered_map<uint64, uint32> lastUseMs;
            uint64 const key = (uint64(bot->GetGUID().GetCounter()) << 32) | uint32(t->ItemId);
            uint32 const now = getMSTime();
            uint32 const throttle = cdMs > 0 ? uint32(cdMs) : 60000;
            {
                std::lock_guard<std::mutex> lock(g_useThrottleMutex);
                uint32& last = lastUseMs[key];
                if (now - last < throttle) return false;
                last = now;
            }
            bot->CastSpell(bot, useSpell, true);
            if (inBag && t->Class == ITEM_CLASS_CONSUMABLE)
            {
                if (found->GetCount() > 1)
                {
                    found->SetCount(found->GetCount() - 1);
                    found->SetState(ITEM_CHANGED, bot);
                }
                else
                    bot->DestroyItem(found->GetBagSlot(), found->GetSlot(), true);
            }
            LOG_INFO("module",
                "[WowPsParty Rotation] {} use_item '{}' (spell={})",
                bot->GetName(), t->Name1, useSpell);
            return true;
        }

        // "drink" / "eat" — sit, pull a consumable out of the SHARED
        // inventory, apply its use-spell on the bot. Rate-limited via a
        // per-bot timestamp so the rule firing every ~250 ms doesn't burn
        // 20 water in 5 seconds — the aura-presence check we previously
        // used couldn't reliably detect the drink/food aura across all
        // ranks, leading to runaway consumption.
        if (verb == "drink" || verb == "eat")
        {
            auto bailWithLog = [&](char const* reason) {
                static thread_local std::unordered_map<uint32, uint32> lastLog;
                uint32 const nowMs = getMSTime();
                uint32& last = lastLog[bot->GetGUID().GetCounter()];
                if (nowMs - last > 5000)
                {
                    last = nowMs;
                    LOG_INFO("module",
                        "[WowPsParty Rotation] {} verb={} bailed: {}",
                        bot->GetName(), verb, reason);
                }
            };
            if (bot->IsInCombat()) { bailWithLog("bot in combat");  return false; }
            // PARTY-wide combat gate: a mage drinking while the warrior
            // tanks isn't what anyone wants. Bail if ANY party member is
            // mid-fight on the same map. Enumerated from our directives.
            {
                std::vector<Player*> party;
                GatherPartyPlayers(bot, party, /*includeDead=*/true);
                for (Player* m : party)
                {
                    if (m == bot) continue;
                    if (m->IsInCombat())
                    {
                        bailWithLog("party member in combat");
                        return false;
                    }
                }
            }
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();

            // NO ITEMS REQUIRED. Party bots — henchmen especially, who have no
            // shared bags — recover for free while seated out of combat. One sit
            // restores BOTH health and mana (Food 433 + Drink 430 are independent
            // auras), topped a fraction of the max pool per 1.5 s so a level-80's
            // huge pool refills in the same ~7.5 s as a low-level one. The same
            // SustainConsume drives the "commit to consuming" hold below, so the
            // fast restore continues there too instead of stalling on one slice.
            SustainConsume(bot);
            return true;
        }

        // "hold_position" — stop in place without sitting. Used by tanks
        // who want to wait for the healer's mana to come back up before
        // taking the lead position again.
        if (verb == "hold_position")
        {
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
            WowPsParty::HoldFollower(bot->GetGUID(), 1500);
            return true;
        }

        // "cancel_form" — drop the current shapeshift (Cat/Moonkin/etc.). A feral
        // cat or moonkin shifts IN combat but must leave the form out of combat to
        // drink/eat, mount with the party, or buff Mark of the Wild. Gated by the
        // rule on out_of_combat + having the form, so it only fires when needed.
        if (verb == "cancel_form")
        {
            if (!bot->HasShapeshiftAura()) return false;
            bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            return true;
        }

        // "wand" — the actual wand shooting (spell 5019 "Shoot", an auto-repeat) is
        // maintained as a BACKGROUND auto-attack in EnsureRangedAutoAttack so it's
        // never toggled and never counts as casting. This rule just reports whether
        // the bot CAN wand the current victim, so an out-of-mana / swarmed caster
        // STOPS here (free, no-mana filler) instead of spending a GCD on a cast-time
        // nuke below it. No CastSpell here — that would fight the background repeat
        // (re-casting resets the swing, the "toggle on/off" the wand had before).
        if (verb == "wand")
        {
            Unit* v = bot->GetVictim();
            if (!v || !v->IsAlive()) return false;
            Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
            if (!ranged || ranged->GetTemplate()->SubClass != ITEM_SUBCLASS_WEAPON_WAND)
                return false;                                 // no wand equipped
            if (bot->GetDistance(v) > 30.0f) return false;    // wands are 30y
            return bot->IsWithinLOSInMap(v);
        }

        // "shoot" — fire the equipped PHYSICAL ranged weapon: a gun/bow/crossbow
        // uses "Shoot" (3018, an auto-repeat like the wand); a thrown weapon uses
        // "Throw" (2764, a single hit). Free — no rage/mana — so a warrior/rogue
        // tank OPENS a pull with its ranged weapon instead of a rage-gated ability
        // it may have no rage for. Returns false when there's no physical ranged
        // weapon (or no ammo / out of range), so an ability fallback (Heroic Throw)
        // still runs. Pair with `out_of_combat` to make it the pull opener.
        if (verb == "shoot")
            return FireRangedWeaponShot(bot, bot->GetVictim());

        // "keep_distance_enemy:N" — kite. When the target is closer than N yards,
        // hop straight away to N+4 and return true. As the LOWEST-priority rule it
        // only fires between casts: instant casts (higher rules, no is_not_moving
        // gate) keep firing while the hop runs; once the hop lands the bot is
        // briefly still and a hard cast (gate it is_not_moving) can go off before
        // the next hop. Re-issued only when not already hopping, so it's discrete
        // shoot-and-scoot rather than a per-tick stutter. Never interrupts an
        // in-flight cast. AssistTarget yields all movement while this rule exists.
        if (verb == "keep_distance_enemy")
        {
            Unit* enemy = bot->GetVictim();
            if (!enemy || !enemy->IsAlive()) return false;
            if (bot->IsNonMeleeSpellCast(false, false, true)) return false;
            float const want = float(std::atof(arg.c_str()));
            if (want <= 0.0f) return false;

            bool const inLoS = bot->IsWithinLOSInMap(enemy);
            float const dist = bot->GetDistance(enemy);

            // Kited around a corner: no line of sight to the target, so every
            // cast fails and the bot just stares at the wall. Regaining LoS
            // beats keeping distance — step back TOWARD the enemy (navmesh path
            // rounds the corner into the room) until we can see it again. This
            // is the "never stuck" guarantee: a blind kiter walks back into LoS
            // instead of freezing.
            if (!inLoS)
            {
                if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
                {
                    float x, y, z;
                    enemy->GetNearPoint(bot, x, y, z, 0.0f,
                                        std::max(want - 4.0f, 5.0f),
                                        enemy->GetAngle(bot));
                    bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE,
                                                      0.0f, 0.0f, true, false);
                }
                return true;
            }

            if (dist >= want) return false;   // far enough AND in LoS — stand & cast

            // Too close: kite only to a spot that's clear of other mobs AND keeps
            // line of sight (PickSafeKitePoint enforces both). If there is NO such
            // spot — boxed in, mobs on both sides, every lane a corner — SKIP the
            // kite entirely (return false) so the bot casts from where it stands
            // instead of freezing. Kevin's rule: a kite that yields nothing must
            // not reserve the tick.
            if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float x, y, z;
                if (!PickSafeKitePoint(bot, enemy, want + 4.0f, x, y, z))
                    return false;             // no meaningful kite — stand & cast
                // forceDestination=false: only move to a point the navmesh can
                // actually REACH, so an unreachable spot (through a wall / over
                // lava) just isn't taken instead of forcing a straight-line dive.
                bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE,
                                                  0.0f, 0.0f, /*generatePath=*/true,
                                                  /*forceDestination=*/false);
            }
            return true;
        }

        // "keep_distance_healer:N" — stay within N yards of the party healer. Steps
        // toward the healer when further than N. Pairs with keep_distance_enemy so
        // a kiter doesn't backpedal out of heal range. Same between-casts timing.
        if (verb == "keep_distance_healer")
        {
            Player* healer = FindPartyMemberByRole(bot, "healer");
            if (!healer || healer == bot || !healer->IsAlive()) return false;
            if (bot->IsNonMeleeSpellCast(false, false, true)) return false;
            float const want = float(std::atof(arg.c_str()));
            if (want <= 0.0f) return false;
            if (bot->GetDistance(healer) <= want) return false;  // close enough
            if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float x, y, z;
                healer->GetNearPoint(bot, x, y, z, 0.0f, std::max(want - 2.0f, 3.0f),
                                     healer->GetAngle(bot));
                // forceDestination=false: only move to a point the navmesh can
                // actually REACH. Without it MovePoint forces the bot straight to
                // the computed spot even when it's through a wall or over lava —
                // a kite point away from a mob landed a mage in Ragefire's lava and
                // dragged the party in. Now an unreachable kite point just stops the
                // bot on valid ground (it stands and casts) instead of diving in.
                bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE,
                                                  0.0f, 0.0f, /*generatePath=*/true,
                                                  /*forceDestination=*/false);
            }
            return true;
        }

        // "close_to_enemy:N" — advance to within N yards of the bot's current
        // victim (the enemy AssistTarget picked: leader's target / tank's / party-
        // defense / the gap-close fallback), then stand and cast. The inverse of
        // keep_distance_enemy: walks IN instead of kiting OUT. AssistTarget still
        // owns TARGETING and sets the victim (BotIsKiting matches this verb, so it
        // yields ONLY movement) — that's why the cast rules keep firing; this rule
        // just drives the feet so a ranged caster actually closes to spell range
        // instead of standing out of it. Make it the LOWEST-priority rule: the cast
        // rules above fire the instant we're in range.
        if (verb == "close_to_enemy")
        {
            Unit* enemy = bot->GetVictim();
            if (!enemy || !enemy->IsAlive()) return false;
            if (bot->IsNonMeleeSpellCast(false, false, true)) return false;
            float const want = float(std::atof(arg.c_str()));
            if (want <= 0.0f) return false;
            if (bot->GetDistance(enemy) <= want) return false;   // in range — stand & cast
            if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float x, y, z;
                enemy->GetNearPoint(bot, x, y, z, 0.0f, std::max(want - 2.0f, 3.0f),
                                    enemy->GetAngle(bot));
                // forceDestination=false: only move to a point the navmesh can
                // actually REACH. Without it MovePoint forces the bot straight to
                // the computed spot even when it's through a wall or over lava —
                // a kite point away from a mob landed a mage in Ragefire's lava and
                // dragged the party in. Now an unreachable kite point just stops the
                // bot on valid ground (it stands and casts) instead of diving in.
                bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE,
                                                  0.0f, 0.0f, /*generatePath=*/true,
                                                  /*forceDestination=*/false);
            }
            return true;
        }

        return false;
    }

    // ----- per-tick entry point -----------------------------------------------

    // True if the unit is mid drink/eat: seated with a food (MOD_REGEN) or
    // drink (MOD_POWER_REGEN) aura active. Same aura types the shared-bag
    // food/drink classifier uses, gated on the sit state so an unrelated
    // power-regen passive can't trip it. Matched by aura TYPE, not a specific
    // spell id, so it catches a real player's higher-rank water/food too (the
    // tank-pull resting check needs this for the human leader).
    bool BotIsConsuming(Player* bot)
    {
        if (!bot || bot->getStandState() != UNIT_STAND_STATE_SIT) return false;
        return bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN)
            || bot->HasAuraType(SPELL_AURA_MOD_REGEN);
    }

    // "Topped off" = full health AND (no mana pool OR full mana). Drinking
    // targets mana; eating targets health; a mage doing both needs both full.
    static bool BotIsTopped(Player* bot)
    {
        if (bot->GetHealth() < bot->GetMaxHealth()) return false;
        if (bot->getPowerType() == POWER_MANA
            && bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA))
            return false;
        return true;
    }

    // Re-summon a hunter's saved pet. Our bots hard-return out of mod-playerbots'
    // UpdateAI, so its pet-resummon never runs — and the client request that
    // normally summons a player's pet on login never comes for a bot. So a hunter
    // bot fights petless unless we resummon it ourselves. Mirrors the factory:
    // pull the saved pet (current → unslotted hunter pet → first stabled) by its
    // PetNumber and LoadPetFromDB. Out of combat only; throttled.
    static void EnsureHunterPet(Player* bot)
    {
        if (bot->getClass() != CLASS_HUNTER) return;
        if (bot->IsInCombat() || bot->GetPet() || bot->IsMounted()) return;
        if (bot->IsNonMeleeSpellCast(false, false, true)) return;

        static std::unordered_map<uint32, uint32> lastTry;
        uint32 const now = getMSTime();
        uint32& last = lastTry[bot->GetGUID().GetCounter()];
        if (last != 0 && now - last < 5000) return;   // don't hammer LoadPetFromDB
        last = now;

        PetStable* stable = bot->GetPetStable();
        if (!stable) return;
        PetStable::PetInfo const* info = nullptr;
        if (stable->CurrentPet)                            info = &stable->CurrentPet.value();
        else if (auto* up = stable->GetUnslottedHunterPet()) info = up;
        else for (auto const& s : stable->StabledPets) if (s) { info = &s.value(); break; }
        if (!info || !info->PetNumber) return;

        Pet* pet = new Pet(bot, HUNTER_PET);
        if (!pet->LoadPetFromDB(bot, 0, info->PetNumber, true))
            delete pet;   // LoadPetFromDB self-cleans on success; delete only on failure
    }

    // Drive ANY bot's pet — hunter beast, warlock demon (imp/voidwalker/felguard/
    // …), mage water elemental, DK ghoul. mod-playerbots normally runs the pet
    // (stance, ability autocast, commanding attacks) but that's gated out for our
    // bots, so a freshly-summoned pet just sits on PASSIVE and never attacks
    // (Kevin's "fresh warlock's imp won't attack" report). We re-implement the
    // essentials for every pet class:
    //   * DEFENSIVE react state (so it engages on its own when its master or the
    //     party is attacked, even before the leader tags the mob) + autocast on
    //     every autocastable ability it knows (imp Firebolt, pet Growl/Claw/Bite,
    //     Waterbolt, …) MINUS the threat-droppers / utility the AI can't time;
    //   * while the master is fighting: push the pet onto the master's victim so
    //     it focuses the same mob;
    //   * out of combat: heel it back so it doesn't body-pull the next pack.
    static void MaintainBotPet(Player* bot)
    {
        Pet* pet = bot->GetPet();
        if (!pet || !pet->IsAlive()) return;
        CharmInfo* charm = pet->GetCharmInfo();
        if (!charm) return;

        // Stance + autocast every tick rather than once-per-pet. The walk is
        // cheap (the pet's spell list is tiny) and idempotent — ToggleAutocast
        // only fires on a real mismatch, so steady state is just the scan. A
        // one-shot was unreliable: ToggleAutocast no-ops on a spell that isn't
        // in m_spells yet, so if the one-shot ran on the tick the pet was still
        // loading its spell list, autocast was never enabled and the pet sat
        // there swinging but never using Growl/Claw/Bite. Re-running self-heals
        // that (and avoids a cross-thread static-map write for bots on
        // different maps).
        if (pet->GetReactState() == REACT_PASSIVE)
        {
            pet->SetReactState(REACT_DEFENSIVE);
            charm->SetPlayerReactState(REACT_DEFENSIVE);
        }
        // Don't autocast threat-droppers / stealth / utility the AI can't time
        // (Cower lowers the threat we want Growl to build; Prowl, Leap, Spell
        // Lock, Devour Magic are situational). Same exclusions as mod-playerbots.
        static std::unordered_set<uint32> const noAutocast = {
            24450, 24452, 24453,         // Prowl 1-3
            1742, 47482,                 // Cower, Leap
            19244, 19647,                // Spell Lock 1-2
            19505, 19731, 19734, 19736,  // Devour Magic 1-4
            27276, 27277, 48011,         // Devour Magic 5-7
            58867                         // Spirit Wolf Leap
        };
        for (auto const& s : pet->m_spells)
        {
            if (s.second.state == PETSPELL_REMOVED) continue;
            SpellInfo const* si = sSpellMgr->GetSpellInfo(s.first);
            if (!si || !si->IsAutocastable()) continue;
            bool const wanted = !noAutocast.count(s.first);
            bool isAuto = false;
            for (uint32 a : pet->m_autospells) if (a == s.first) { isAuto = true; break; }
            // Toggle only on a mismatch — enables the abilities we want and
            // actively turns OFF a re-summoned pet's excluded autocasts
            // (e.g. a saved-on Cower that would shed the Growl threat).
            if (wanted != isAuto) pet->ToggleAutocast(si, wanted);
        }

        Unit* victim = bot->GetVictim();
        if (victim && victim->IsAlive() && bot->IsValidAttackTarget(victim))
        {
            // Already command-attacking the right mob — leave it; re-issuing
            // AttackStart every tick resets the pet's swing/ability timers. But
            // if the pet drifted onto this victim on its own (defensive aggro,
            // not our command) re-issue so it's flagged command-attack and won't
            // wander back to follow mid-fight.
            if (pet->GetVictim() == victim && charm->IsCommandAttack()) return;
            pet->ClearUnitState(UNIT_STATE_FOLLOW);
            pet->AttackStop();
            pet->SetTarget(victim->GetGUID());
            charm->SetIsCommandAttack(true);
            charm->SetIsAtStay(false);
            charm->SetIsFollowing(false);
            charm->SetIsCommandFollow(false);
            charm->SetIsReturning(false);
            if (pet->AI())
                pet->AI()->AttackStart(victim);
        }
        else if (!bot->IsInCombat() && (pet->GetVictim() || charm->IsCommandAttack()))
        {
            // Hunter is genuinely out of combat — call the pet off so it heels
            // instead of chasing the last mob into the next pack. (While the
            // hunter is in combat but hasn't picked a victim yet, we leave the
            // pet on whatever it's defensively engaging — don't yank it off a
            // mob that's beating on it or the hunter.)
            pet->AttackStop();
            charm->SetIsCommandAttack(false);
            charm->SetCommandState(COMMAND_FOLLOW);
            charm->SetIsCommandFollow(true);
            charm->SetIsFollowing(false);
            charm->SetIsReturning(true);
            pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
        }
    }

    // Maintain a bot's BACKGROUND ranged auto-attack — the same way for a hunter's
    // Auto Shot and a caster's wand, because both are auto-repeat spells with the
    // same pitfalls:
    //   * hunter: spell 75 "Auto Shot" with a gun/bow/crossbow + ammo.
    //   * caster (priest/mage/warlock): spell 5019 "Shoot" with a WAND equipped.
    //     (NOTE: the action verb is `wand` but the real spell is 5019 "Shoot".)
    // Cast it ONCE; the core then auto-repeats it (and cancels it on movement, so a
    // bot resumes when it plants). It must NOT be re-cast every tick — re-casting
    // resets the swing and the bot toggles it on/off without ever landing a shot —
    // so this only (re)starts it when there is NO live auto-repeat, and otherwise
    // just re-acquires on a target switch. It is NOT driven by a rotation rule (the
    // `wand` verb just confirms it's running, to suppress cast-time nukes), and it
    // never counts as "casting" because every gate passes skipAutorepeat=true, so
    // abilities/nukes still fire on top. Cast 3018 (warrior/rogue Shoot) is the
    // WRONG spell for a wand and never establishes the repeat.
    static void EnsureRangedAutoAttack(Player* bot)
    {
        Item* const ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!ranged) return;
        uint8 const sub = ranged->GetTemplate()->SubClass;

        uint32 autoSpell = 0;
        float  maxRange  = 0.0f;
        bool   needAmmo  = false;
        if (bot->getClass() == CLASS_HUNTER)
        {
            if (sub == ITEM_SUBCLASS_WEAPON_GUN || sub == ITEM_SUBCLASS_WEAPON_BOW
                || sub == ITEM_SUBCLASS_WEAPON_CROSSBOW)
            { autoSpell = 75; maxRange = 40.0f; needAmmo = true; }
        }
        else if (sub == ITEM_SUBCLASS_WEAPON_WAND)   // priest/mage/warlock with a wand
        { autoSpell = 5019; maxRange = 30.0f; }
        if (!autoSpell) return;

        Unit* victim = bot->GetVictim();

        // HUNTER only: standing its ground in melee (no tank, mob in our face) —
        // AssistTarget switched on melee swings and Auto Shot is dead-zoned this
        // close, so cancel the repeat and let the swings work. Wands have NO dead
        // zone, so a caster keeps wanding point-blank (don't cancel).
        if (autoSpell == 75 && bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            if (bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
                bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
            return;
        }

        // The repeat, once cast, runs until interrupted — AttackStop() clears the
        // melee swing, NOT the ranged repeat. Cancel only the two stale states so
        // the block below restarts a fresh one on the CURRENT victim; otherwise
        // leave the live repeat alone (re-casting it would reset the swing and
        // "toggle it on/off"):
        //   * no live victim → it keeps firing the animation out of combat;
        //   * locked on a unit we're no longer fighting → it never re-acquires the
        //     new mob and the bot "does nothing" while the animation hits a corpse.
        if (Spell* repeat = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        {
            Unit* shotAt = repeat->m_targets.GetUnitTarget();
            if (!victim || !victim->IsAlive() || shotAt != victim)
                bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
            return;   // already on the right target, or just cancelled — restart next tick
        }

        if (!victim || !victim->IsAlive()) return;
        if (bot->isMoving()) return;                           // can't start a shot mid-move
        if (needAmmo && bot->GetUInt32Value(PLAYER_AMMO_ID) == 0) return;
        if (bot->GetDistance(victim) > maxRange) return;       // out of ranged range
        if (!bot->IsWithinLOSInMap(victim)) return;
        if (autoSpell == 5019) bot->SetFacingToObject(victim); // wand repeat fails NOT_INFRONT otherwise
        SpellCastResult const r = bot->CastSpell(victim, autoSpell, false);
        if (r != SPELL_CAST_OK)
        {
            static thread_local std::unordered_map<uint32, uint32> failMs;
            uint32 const now = getMSTime();
            uint32& last = failMs[bot->GetGUID().GetCounter()];
            if (now - last > 5000)
            {
                last = now;
                LOG_INFO("module", "[WowPsParty Rotation] {} ranged auto-attack ({}) failed: result={}",
                         bot->GetName(), autoSpell, uint32(r));
            }
        }
    }

    // True if the party is ACTUALLY engaging on foot — the leader is off its mount,
    // or some other member is in combat and dismounted (force-dismounted by damage).
    // Used to decide whether a mounted bot that's IN COMBAT should drop off to fight:
    // during a mounted fly-by (everyone still riding, a mob merely chasing and never
    // landing a hit because mounted is faster) it stays mounted and keeps moving;
    // only a genuine engagement — the leader stops and dismounts, or a member gets
    // caught and knocked off — pulls the rest of the party off their mounts too.
    bool PartyEngagedDismounted(Player* bot)
    {
        if (!bot) return false;

        ObjectGuid const lg = GetLeaderFor(bot->GetGUID());
        if (!lg.IsEmpty())
            if (Player* leader = ObjectAccessor::FindConnectedPlayer(lg))
                if (leader->IsInWorld() && leader->GetMapId() == bot->GetMapId()
                    && !leader->IsMounted())
                    return true;   // the leader is on foot

        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(bot->GetGUID(), party);
        for (ObjectGuid const& g : party)
        {
            if (g == bot->GetGUID()) continue;
            Player* m = ObjectAccessor::FindConnectedPlayer(g);
            if (m && m->IsInWorld() && m->IsAlive()
                && m->GetMapId() == bot->GetMapId()
                && m->IsInCombat() && !m->IsMounted())
                return true;   // a member fighting on foot (knocked off by damage)
        }
        return false;
    }

    bool TickRotation(Player* bot)
    {
        if (!bot) return false;
        // A dead bot must not run its rotation — otherwise rules like
        // out_of_combat Stealth fire and fail SPELL_FAILED_CASTER_DEAD every
        // tick. (Resurrect-accept is handled separately in ApplyDirective.)
        if (!bot->IsAlive()) return false;

        // Mounted while traveling with the leader: do NOTHING out of combat. An
        // out_of_combat rule — a rogue's Stealth, a hunter's Call Pet, a self-buff,
        // eat/drink — would cast and DISMOUNT the bot, which the follow ticker's
        // mount-sync then re-mounts, and the rule re-fires: an endless mount/cast
        // flicker. So out of combat we just skip and stay mounted.
        //
        // IN COMBAT, dismount — but ONLY once the party is genuinely engaging, i.e.
        // the leader has dismounted or a member got knocked off by damage (see
        // PartyEngagedDismounted). A bot that entered combat mounted and never got
        // HIT (a mob merely chasing — it can't catch a mounted unit, which also
        // can't attack back) would otherwise sit stuck mounted, spamming failed
        // ENGAGEs, with the follow ticker's dismount-sync yielding in combat. But
        // if EVERYONE is still riding (mounted fly-by, incidental aggro/damage in
        // transit), stay mounted and keep moving — don't commit the party to a fight
        // it's trying to ride past. No re-mount flicker: the mount-sync only
        // re-mounts out of combat.
        if (bot->IsMounted())
        {
            if (!bot->IsInCombat() || !PartyEngagedDismounted(bot))
                return false;   // idle travel, or a mounted fly-by — stay mounted
            bot->Dismount();    // the party is fighting on foot — get off and engage
        }

        // Keep ammo/poisons topped up (self-throttled). Runs before the rotation
        // so a freshly-spawned hunter has arrows on its first idle tick and a
        // rogue stays poisoned — playerbots' own upkeep is gated out for us.
        WowPsParty::MaintainBotConsumables(bot);

        // Keep the hunter's auto-shot running between ability casts, its pet
        // summoned, and EVERY bot's pet defending + on-target (all bypassed by our
        // UpdateAI gate). MaintainBotPet covers all pet classes — without it a
        // freshly-summoned warlock imp / mage elemental sits on passive.
        EnsureRangedAutoAttack(bot);
        EnsureHunterPet(bot);
        MaintainBotPet(bot);

        std::vector<RotationRule> rules;
        {
            std::lock_guard<std::mutex> lock(g_rotationCacheMutex);
            auto it = g_rotationCache.find(bot->GetGUID().GetCounter());
            if (it == g_rotationCache.end()) return false;
            rules = it->second;  // copy, drop lock before doing real work
        }

        // Party leash: if the controlled char has run >50y away, don't cast
        // or drink — yield so the follow ticker rejoins the leader (it walks
        // back at 50y, teleports past 100y). Matches AssistTarget's leash.
        if (ObjectGuid const lg = GetLeaderFor(bot->GetGUID()))
            if (Player* leader = ObjectAccessor::FindConnectedPlayer(lg))
                if (leader->IsInWorld() && leader->GetMapId() == bot->GetMapId()
                    && bot->GetDistance(leader) > 50.0f)
                    return false;

        // Rate-limited per-tick trace. The user has reported several
        // "rule X isn't firing" symptoms (taunt looping, thunder clap
        // not casting) where the actual root cause needed full visibility
        // into the rotation loop. Log one summary block per bot every
        // ~3 s: rule-by-rule, condition pass/fail, and what ExecAction
        // returned. Off-by-default tuning would mean retrofitting logs
        // every time something misbehaves, which the project rule
        // ("Diagnostic logging in everything from the start") explicitly
        // forbids.
        static thread_local std::unordered_map<uint32, uint32> lastTraceMs;
        uint32 const nowMs = getMSTime();
        uint32& last = lastTraceMs[bot->GetGUID().GetCounter()];
        bool const trace = (nowMs - last > 3000);
        if (trace) last = nowMs;

        // Feed the TTD estimator one sample for the current victim every tick
        // (the recorder throttles to one sample / 0.5 s per target). Must run
        // unconditionally — before any early return on cond mismatch — or the
        // target_ttd condition would never accumulate history.
        TtdRecord(bot->GetVictim());

        if (trace)
        {
            Unit* victim = bot->GetVictim();
            LOG_INFO("module",
                "[WowPsParty Rotation] {} TICK rules={} rage={}/{} mana={}/{} target={} hp={}%",
                bot->GetName(), uint32(rules.size()),
                bot->GetPower(POWER_RAGE), bot->GetMaxPower(POWER_RAGE),
                bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA),
                victim ? victim->GetGUID().GetCounter() : 0,
                victim && victim->GetMaxHealth() > 0
                    ? int((float(victim->GetHealth()) / float(victim->GetMaxHealth())) * 100.0f)
                    : -1);
        }

        // Commit to drinking/eating. Once a bot is mid-consume and not yet at
        // full health/mana (and not in combat), suppress the WHOLE rotation so
        // it keeps regenerating until topped off — instead of standing up to cast
        // the instant it has enough mana for one spell. CRUCIAL: drive the SAME
        // SustainConsume the drink verb uses, so the fast restore (and the aura
        // re-apply when it lapses) keeps going — suppressing the rotation alone
        // froze recovery at one slice ("consuming, mana stuck, takes forever").
        // Combat breaks the regen aura anyway, so the !IsInCombat gate hands
        // control straight back when a fight starts.
        if (!bot->IsInCombat() && BotIsConsuming(bot) && !BotIsTopped(bot))
        {
            SustainConsume(bot);
            if (trace)
                LOG_INFO("module",
                    "[WowPsParty Rotation] {} consuming — holding until full/done "
                    "(hp={}/{} mana={}/{})",
                    bot->GetName(), bot->GetHealth(), bot->GetMaxHealth(),
                    bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA));
            return true;
        }

        for (RotationRule const& r : rules)
        {
            // A rule disabled in the editor (checkbox off) carries the
            // "disabled" flag — keep it in the list so the user doesn't lose
            // it, but never fire it.
            if (CsvContains(Lower(r.flags), "disabled"))
            {
                if (trace)
                    LOG_INFO("module",
                        "[WowPsParty Rotation]   prio={} cond=[{}] act=[{}] -> DISABLED",
                        r.priority, r.condition, r.action);
                continue;
            }

            bool const condOk = EvalCondition(r.condition, bot);
            if (!condOk)
            {
                if (trace)
                    LOG_INFO("module",
                        "[WowPsParty Rotation]   prio={} cond=[{}] act=[{}] -> NO_MATCH",
                        r.priority, r.condition, r.action);
                continue;
            }
            bool const execOk = ExecAction(r.action, bot, r.flags);
            if (trace)
                LOG_INFO("module",
                    "[WowPsParty Rotation]   prio={} cond=[{}] act=[{}] -> {}",
                    r.priority, r.condition, r.action,
                    execOk ? "FIRED" : "exec_failed_falling_through");
            if (execOk)
                return true;
        }
        return false;
    }
}

// Free-function trampoline so the [WowPsParty PATCH] block in mod-playerbots's
// PlayerbotAI.cpp can call into our module without including PartyRotation.h
// (the include path direction would be reversed). extern "C"-style binding via
// the linker — our module exports it, mod-playerbots' patch declares it.
bool WowPsParty_TickRotation_Trampoline(Player* bot)
{
    return WowPsParty::TickRotation(bot);
}
