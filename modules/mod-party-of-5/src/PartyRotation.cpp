/*
 * WowPs Party-of-5 mod — rotation DSL implementation
 */

#include "PartyRotation.h"
#include "PartyFollow.h"

#include "Bag.h"
#include "Cell.h"
#include "CellImpl.h"
#include "DatabaseEnv.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "WorldSession.h"

#include <functional>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace WowPsParty
{
    static std::unordered_map<uint32, std::vector<RotationRule>> g_rotationCache;
    static std::mutex g_rotationCacheMutex;

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
        }
        return out.str();
    }

    // ----- cache --------------------------------------------------------------

    void RotationCacheSet(uint32 guid, std::vector<RotationRule> rules)
    {
        std::lock_guard<std::mutex> lock(g_rotationCacheMutex);
        if (rules.empty())
            g_rotationCache.erase(guid);
        else
            g_rotationCache[guid] = std::move(rules);
    }

    void RotationCacheClear(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(g_rotationCacheMutex);
        g_rotationCache.erase(guid);
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

    // ----- party helpers ------------------------------------------------------

    // Walks the bot's group and returns the lowest-HP alive party member
    // (including the bot itself). Returns nullptr if the bot isn't in a group
    // or no member is alive in range.
    static Player* GetLowestHpPartyMember(Player* bot)
    {
        if (!bot) return nullptr;
        Group* g = bot->GetGroup();
        if (!g)
            return bot->IsAlive() ? bot : nullptr;

        Player* best = nullptr;
        float bestPct = 200.0f;
        for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
        {
            Player* m = itr->GetSource();
            if (!m || !m->IsAlive() || !m->IsInWorld()) continue;
            if (m->GetMapId() != bot->GetMapId()) continue;
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

    // True if target has any aura cast by anyone from spell `spellId`.
    static bool HasAuraFromSpell(Unit* target, uint32 spellId)
    {
        if (!target || !spellId) return false;
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

    // Find an enemy within `radius` of the bot whose current victim is a
    // party member OTHER than the bot itself. Used by the tank's
    // `cast_loose_enemy:Taunt` rule to pull aggro off the healer / casters.
    static Unit* FindLooseEnemy(Player* bot, float radius)
    {
        if (!bot) return nullptr;
        std::list<Unit*> targets;
        GatherHostilesAround(bot, radius, targets);
        Group* g = bot->GetGroup();
        for (Unit* enemy : targets)
        {
            if (!enemy || !enemy->IsAlive()) continue;
            Unit* victim = enemy->GetVictim();
            if (!victim) continue;
            if (victim == bot) continue;          // already on us
            if (!victim->IsPlayer()) continue;     // only care about party
            if (g)
            {
                if (!g->IsMember(victim->GetGUID())) continue;
            }
            else
            {
                // No group → only "ally" is self; we already excluded bot
                continue;
            }
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
    static uint32 ConsumeSharedConsumable(Player* bot, bool drink)
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
        Group* g = bot->GetGroup();
        if (!g) return nullptr;
        for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
        {
            Player* m = itr->GetSource();
            if (!m || !m->IsInWorld() || m == bot) continue;
            if (m->GetMapId() != bot->GetMapId()) continue;
            if (m->IsAlive()) continue;
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
        Group* g = bot->GetGroup();
        auto check = [type, bot](Player* m) -> bool {
            if (!m || !m->IsAlive() || !m->IsInWorld()) return false;
            if (m->GetMapId() != bot->GetMapId()) return false;
            return HasDebuffOfType(m, type);
        };
        if (!g)
            return check(bot) ? bot : nullptr;
        for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
        {
            Player* m = itr->GetSource();
            if (check(m)) return m;
        }
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
    static bool TargetHasNamedAura(Unit* target, std::string const& name)
    {
        if (!target || name.empty()) return false;
        std::string needle;
        needle.reserve(name.size());
        for (char c : name) needle.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        for (auto const& kv : target->GetAppliedAuras())
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
            if (lower == needle) return true;
        }
        return false;
    }

    // Forward decl so EvalCondition can recurse through AND-chains.
    static bool EvalSingleCondition(std::string const& cond, Player* bot);

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
            if (!clause.empty() && !EvalSingleCondition(clause, bot))
                return false;
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
        }
        if (cond == "in_combat")     return bot->IsInCombat();
        if (cond == "out_of_combat") return !bot->IsInCombat();
        if (cond == "has_target")    return bot->GetTarget() != ObjectGuid::Empty;
        if (cond == "no_target")     return bot->GetTarget() == ObjectGuid::Empty;

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

        return false;
    }

    // ----- action executor ----------------------------------------------------

    // Find a spell in the bot's spellbook by case-insensitive name. Returns 0
    // if not known.
    static uint32 FindKnownSpellByName(Player* bot, std::string const& name)
    {
        std::string needle;
        needle.reserve(name.size());
        for (char c : name) needle.push_back(std::tolower(static_cast<unsigned char>(c)));

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
            if (lower == needle) return kv.first;
        }
        return 0;
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
        Group* g = bot->GetGroup();
        auto consider = [&](Player* m) -> Player* {
            if (!m || !m->IsAlive() || !m->IsInWorld()) return nullptr;
            if (m->GetMapId() != bot->GetMapId()) return nullptr;
            char const* kw = ClassKeyword(m->getClass());
            if (!*kw) return nullptr;
            if (!CsvContains(csv, kw)) return nullptr;
            if (HasAuraFromSpell(m, spellId)) return nullptr;
            return m;
        };
        if (!g) return consider(bot);
        for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
        {
            if (Player* hit = consider(itr->GetSource())) return hit;
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
        Group* g = bot->GetGroup();
        if (!g)
            return (bot->IsAlive() && !HasAuraFromSpell(bot, spellId)) ? bot : nullptr;

        for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
        {
            Player* m = itr->GetSource();
            if (!m || !m->IsAlive() || !m->IsInWorld()) continue;
            if (m->GetMapId() != bot->GetMapId()) continue;
            if (HasAuraFromSpell(m, spellId)) continue;
            return m;
        }
        return nullptr;
    }

    static bool ExecAction(std::string const& act, Player* bot)
    {
        // action format: <verb>:<arg>
        auto colon = act.find(':');
        std::string verb = act.substr(0, colon);
        std::string arg  = colon == std::string::npos ? "" : act.substr(colon + 1);

        // A cast is only "really fireable" when EVERYTHING checks out:
        // cooldown clear, enough mana/power, within range, line of sight,
        // target alive + valid. Any failure → rule returns false and the
        // rotation drops to the next lower-priority rule. Without this,
        // an `always | cast: Holy Light` on a low-mana paladin would
        // halt the loop every tick (rule "fired" but actual cast failed
        // silently), starving every lower-priority rule.
        auto canFireSpellOn = [bot](uint32 spellId, Unit* target) -> bool
        {
            if (!spellId || !target) return false;
            if (bot->HasSpellCooldown(spellId)) return false;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info) return false;

            // GCD: AC stores it in a per-player GlobalCooldownMgr, NOT in
            // the regular cooldown map. StartRecoveryCategory alone misses
            // it. Without this check the mage would happily try to cast
            // Frostbolt during a GCD, fail with SPELL_FAILED_NOT_READY
            // (result=67), and the rotation would fall through to Shoot.
            if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
                return false;
            if (info->StartRecoveryCategory > 0 &&
                bot->HasSpellCooldown(info->StartRecoveryCategory))
                return false;

            // Shapeshift / stance gate. Without this, a battle-stance
            // warrior with a Taunt rule (requires Defensive Stance)
            // fires CastSpell every tick, server replies with
            // SPELL_FAILED_ONLY_SHAPESHIFT (94), and the rule loops
            // forever instead of falling through to Thunder Clap /
            // Heroic Strike. SpellInfo::CheckShapeshift handles the
            // full mask (Stances + StancesNot + cancellable-form bits).
            if (info->CheckShapeshift(uint32(bot->GetShapeshiftForm())) != SPELL_CAST_OK)
                return false;

            // Power cost (mana / rage / energy / etc).
            int32 const cost = info->CalcPowerCost(bot, info->GetSchoolMask());
            if (cost > 0)
            {
                Powers const powerType = Powers(info->PowerType);
                if (int32(bot->GetPower(powerType)) < cost) return false;
            }

            // Self-casts don't need range / LoS checks.
            if (target != bot)
            {
                float const maxRange = info->GetMaxRange(info->IsPositive(), bot);
                if (maxRange > 0 && !bot->IsWithinDistInMap(target, maxRange))
                    return false;
                if (!bot->IsWithinLOSInMap(target))
                    return false;
            }

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
        auto faceAndCast = [bot](Unit* target, uint32 spellId) -> bool
        {
            if (target && target != bot)
                bot->SetFacingToObject(target);
            int32 castMs = 0;
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                castMs = info->CalcCastTime();
            if (castMs > 0)
            {
                bot->StopMoving();
                bot->GetMotionMaster()->Clear();
                WowPsParty::HoldFollower(bot->GetGUID(),
                    uint32(castMs) + 500);
            }
            SpellCastResult const r = bot->CastSpell(target, spellId, false);
            if (r != SPELL_CAST_OK)
            {
                LOG_INFO("module",
                    "[WowPsParty Rotation] cast {} failed on guid={} result={}",
                    spellId, bot->GetGUID().GetCounter(), uint32(r));
                return false;
            }
            return true;
        };

        if (verb == "cast" || verb == "cast_self")
        {
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            Unit* target = (verb == "cast_self") ? bot : bot->GetVictim();
            if (!target) return false;
            if (!canFireSpellOn(spellId, target)) return false;
            if (bot->IsNonMeleeSpellCast(false)) return false;
            return faceAndCast(target, spellId);
        }

        if (verb == "buff_self")
        {
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            if (HasAuraFromSpell(bot, spellId)) return false;
            if (!canFireSpellOn(spellId, bot)) return false;
            if (bot->IsNonMeleeSpellCast(false)) return false;
            return faceAndCast(bot, spellId);
        }

        if (verb == "cast_party_lowest" || verb == "cast_party_lowest_hot")
        {
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            Player* target = GetLowestHpPartyMember(bot);
            if (!target) return false;
            if (verb == "cast_party_lowest_hot" && HasAuraFromSpell(target, spellId))
                return false;
            if (!canFireSpellOn(spellId, target)) return false;
            if (bot->IsNonMeleeSpellCast(false)) return false;
            return faceAndCast(target, spellId);
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
            if (!canFireSpellOn(spellId, target)) return false;
            if (bot->IsNonMeleeSpellCast(false)) return false;
            return faceAndCast(target, spellId);
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
            if (!canFireSpellOn(spellId, target)) return false;
            if (bot->IsNonMeleeSpellCast(false)) return false;
            return faceAndCast(target, spellId);
        }

        if (verb == "cast_party_missing")
        {
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            Player* target = FindPartyMemberMissingAura(bot, spellId);
            if (!target) return false;
            if (!canFireSpellOn(spellId, target)) return false;
            if (bot->IsNonMeleeSpellCast(false)) return false;
            return faceAndCast(target, spellId);
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
            if (!canFireSpellOn(spellId, target)) return false;
            if (bot->IsNonMeleeSpellCast(false)) return false;
            return faceAndCast(target, spellId);
        }

        if (verb == "rez_party")
        {
            if (bot->IsInCombat()) return false;
            uint32 const spellId = FindKnownSpellByName(bot, arg);
            if (!spellId) return false;
            Player* target = FindDeadPartyMember(bot);
            if (!target) return false;
            if (!canFireSpellOn(spellId, target)) return false;
            if (bot->IsNonMeleeSpellCast(false)) return false;
            return faceAndCast(target, spellId);
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
            if (!canFireSpellOn(spellId, target)) return false;
            if (bot->IsNonMeleeSpellCast(false)) return false;
            return faceAndCast(target, spellId);
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
            // tanks isn't what anyone wants. Bail if ANY group member is
            // mid-fight on the same map.
            if (Group* g = bot->GetGroup())
            {
                for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
                {
                    Player* m = itr->GetSource();
                    if (!m || m == bot) continue;
                    if (m->GetMapId() != bot->GetMapId()) continue;
                    if (m->IsInCombat())
                    {
                        bailWithLog("party member in combat");
                        return false;
                    }
                }
            }
            bool const wantDrink = (verb == "drink");
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
            if (bot->getStandState() != UNIT_STAND_STATE_SIT)
                bot->SetStandState(UNIT_STAND_STATE_SIT);

            // Drink auras in 3.3.5a last 20-30 s. 20 s between consumes
            // gives a tiny gap of passive regen before the next stack
            // burns, which is the natural "drink → wait → drink" cadence
            // a real player would use anyway.
            static std::unordered_map<uint32, uint32> lastDrinkMs;
            static std::unordered_map<uint32, uint32> lastEatMs;
            auto& last = wantDrink
                ? lastDrinkMs[bot->GetGUID().GetCounter()]
                : lastEatMs  [bot->GetGUID().GetCounter()];
            uint32 const now = getMSTime();
            if (now - last >= 20000)
            {
                uint32 const spellId = ConsumeSharedConsumable(bot, wantDrink);
                if (spellId)
                {
                    bot->CastSpell(bot, spellId, true);  // triggered, no GCD/cost
                    last = now;
                    LOG_INFO("module",
                        "[WowPsParty Rotation] {} verb={} consumed shared item, cast spell={}",
                        bot->GetName(), verb, spellId);
                }
                else
                {
                    LOG_INFO("module",
                        "[WowPsParty Rotation] {} verb={} no shared {} in party bags",
                        bot->GetName(), verb, wantDrink ? "drink" : "food");
                }
            }
            WowPsParty::HoldFollower(bot->GetGUID(), 1500);
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

        return false;
    }

    // ----- per-tick entry point -----------------------------------------------

    bool TickRotation(Player* bot)
    {
        if (!bot) return false;
        std::vector<RotationRule> rules;
        {
            std::lock_guard<std::mutex> lock(g_rotationCacheMutex);
            auto it = g_rotationCache.find(bot->GetGUID().GetCounter());
            if (it == g_rotationCache.end()) return false;
            rules = it->second;  // copy, drop lock before doing real work
        }

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

        for (RotationRule const& r : rules)
        {
            bool const condOk = EvalCondition(r.condition, bot);
            if (!condOk)
            {
                if (trace)
                    LOG_INFO("module",
                        "[WowPsParty Rotation]   prio={} cond=[{}] act=[{}] -> NO_MATCH",
                        r.priority, r.condition, r.action);
                continue;
            }
            bool const execOk = ExecAction(r.action, bot);
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
