/*
 * WowPs Party-of-5 mod — PartyMgr implementation
 */

#include "PartyMgr.h"
#include "PartyFollow.h"
#include "PartyRotation.h"

#include "Chat.h"
#include "CharacterCache.h"
#include "CharmInfo.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LootMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Unit.h"
#include "WorldSession.h"

// mod-playerbots (AC's modules build adds every subdir of every module to the
// include path, so no `Bot/` prefix is needed)
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "RandomItemMgr.h"      // sRandomItemMgr.GetAmmo
#include "PlayerbotFactory.h"   // re-level a widened henchman pick to the player
#include "AiObjectContext.h"
#include "Value.h"

#include "MotionMaster.h"
#include "Pet.h"  // PET_FOLLOW_DIST

#include "WorldSession.h"
#include "WorldPacket.h"
#include "Opcodes.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// Forward declarations of helpers defined in PartyAddonProtocol.cpp / PartyRotation.cpp
namespace WowPsParty
{
    void SendRosterTo(Player* player);
    void RotationCacheRefreshFromDB(uint32 guid);
    void TargetModeRefreshFromDB(uint32 guidLow);
    void LeadDungeonRefreshFromDB(uint32 guidLow);
    void PushControlledLoadoutTo(Player* requester, int slot);
}

namespace WowPsParty
{
    PartyMgr& PartyMgr::Instance()
    {
        static PartyMgr instance;
        return instance;
    }

    // ----- per-account feature toggles ---------------------------------------
    static std::unordered_map<uint32, PartySettings> g_accountSettings;
    static std::mutex                                g_settingsMutex;

    void EnsureSettingsTable()
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `party_account_settings` ("
            "`account` INT UNSIGNED NOT NULL PRIMARY KEY, "
            "`spawn_companions` TINYINT NOT NULL DEFAULT 1, "
            "`shared_inventory` TINYINT NOT NULL DEFAULT 1, "
            "`shared_gear` TINYINT NOT NULL DEFAULT 1, "
            "`shared_progression` TINYINT NOT NULL DEFAULT 1, "
            "`quest_xp_rate` SMALLINT UNSIGNED NOT NULL DEFAULT 200, "
            "`kill_xp_rate` SMALLINT UNSIGNED NOT NULL DEFAULT 200)");
        // Migrate installs that predate the XP-rate columns. The DB server is
        // MySQL, which — unlike MariaDB — has no `ADD COLUMN IF NOT EXISTS`; that
        // syntax errors 1064 and AC aborts the worldserver on any SQL error. So
        // probe information_schema and add each column only when it's missing.
        auto columnMissing = [](char const* col) -> bool
        {
            return !CharacterDatabase.Query(
                "SELECT 1 FROM `information_schema`.`COLUMNS` "
                "WHERE `TABLE_SCHEMA` = DATABASE() "
                "AND `TABLE_NAME` = 'party_account_settings' "
                "AND `COLUMN_NAME` = '{}'", col);
        };
        if (columnMissing("quest_xp_rate"))
            CharacterDatabase.DirectExecute(
                "ALTER TABLE `party_account_settings` "
                "ADD COLUMN `quest_xp_rate` SMALLINT UNSIGNED NOT NULL DEFAULT 200");
        if (columnMissing("kill_xp_rate"))
            CharacterDatabase.DirectExecute(
                "ALTER TABLE `party_account_settings` "
                "ADD COLUMN `kill_xp_rate` SMALLINT UNSIGNED NOT NULL DEFAULT 200");
        // New accounts default to x2 XP. Installs whose columns were first added
        // with the old DEFAULT 100 get their new-row default switched to 200.
        // Only ALTER when a default actually isn't 200 yet, so we're not running
        // unconditional DDL on every startup (existing rows are untouched).
        auto defaultNot200 = [](char const* col) -> bool
        {
            return CharacterDatabase.Query(
                "SELECT 1 FROM `information_schema`.`COLUMNS` "
                "WHERE `TABLE_SCHEMA` = DATABASE() "
                "AND `TABLE_NAME` = 'party_account_settings' "
                "AND `COLUMN_NAME` = '{}' AND `COLUMN_DEFAULT` <> '200'", col) != nullptr;
        };
        if (defaultNot200("quest_xp_rate") || defaultNot200("kill_xp_rate"))
            CharacterDatabase.DirectExecute(
                "ALTER TABLE `party_account_settings` "
                "ALTER `quest_xp_rate` SET DEFAULT 200, ALTER `kill_xp_rate` SET DEFAULT 200");
    }

    void AccountSettingsRefreshFromDB(uint32 account)
    {
        PartySettings s;  // all-ON default
        QueryResult q = CharacterDatabase.Query(
            "SELECT `spawn_companions`,`shared_inventory`,`shared_gear`,"
            "`shared_progression`,`quest_xp_rate`,`kill_xp_rate` "
            "FROM `party_account_settings` WHERE `account` = {}",
            account);
        if (q)
        {
            Field* f = q->Fetch();
            s.spawnCompanions   = f[0].Get<uint8>() != 0;
            s.sharedInventory   = f[1].Get<uint8>() != 0;
            s.sharedGear        = f[2].Get<uint8>() != 0;
            s.sharedProgression = f[3].Get<uint8>() != 0;
            s.questXpRate       = std::clamp<uint32>(f[4].Get<uint16>(), XP_RATE_MIN, XP_RATE_MAX);
            s.killXpRate        = std::clamp<uint32>(f[5].Get<uint16>(), XP_RATE_MIN, XP_RATE_MAX);
        }
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        g_accountSettings[account] = s;
    }

    PartySettings GetAccountSettings(uint32 account)
    {
        {
            std::lock_guard<std::mutex> lock(g_settingsMutex);
            auto it = g_accountSettings.find(account);
            if (it != g_accountSettings.end()) return it->second;
        }
        AccountSettingsRefreshFromDB(account);
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        return g_accountSettings[account];
    }

    void SetAccountSetting(uint32 account, std::string const& key, bool value)
    {
        static const std::unordered_map<std::string, int> cols = {
            {"spawn_companions", 0}, {"shared_inventory", 1},
            {"shared_gear", 2}, {"shared_progression", 3} };
        if (cols.find(key) == cols.end()) return;
        uint8 const v = value ? 1 : 0;
        // Upsert the single column; unspecified columns keep their table
        // default (1) on insert, which matches "all ON by default".
        CharacterDatabase.Execute(
            "INSERT INTO `party_account_settings` (`account`, `{}`) VALUES ({}, {}) "
            "ON DUPLICATE KEY UPDATE `{}` = {}",
            key, account, uint32(v), key, uint32(v));
        // Refresh the cached struct field.
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        PartySettings& s = g_accountSettings[account];
        if      (key == "spawn_companions")   s.spawnCompanions   = value;
        else if (key == "shared_inventory")   s.sharedInventory   = value;
        else if (key == "shared_gear")        s.sharedGear        = value;
        else if (key == "shared_progression") s.sharedProgression = value;
    }

    void SetAccountXpRate(uint32 account, bool quest, uint32 rate)
    {
        rate = std::clamp<uint32>(rate, XP_RATE_MIN, XP_RATE_MAX);
        char const* const col = quest ? "quest_xp_rate" : "kill_xp_rate";
        // Upsert the single column; other columns keep their defaults on insert.
        CharacterDatabase.Execute(
            "INSERT INTO `party_account_settings` (`account`, `{}`) VALUES ({}, {}) "
            "ON DUPLICATE KEY UPDATE `{}` = {}",
            col, account, rate, col, rate);
        // Populate the cache from DB first (on a miss) so the other fields aren't
        // clobbered with defaults, then update just this one.
        GetAccountSettings(account);
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        PartySettings& s = g_accountSettings[account];
        if (quest) s.questXpRate = rate;
        else       s.killXpRate  = rate;
    }

    // ----- Henchmen ----------------------------------------------------------

    // Cached CSV of random-bot account ids ("rndbot*"), read once from the
    // auth DB. Used to scope the henchman candidate pool to random-pool chars.
    static std::string RndbotAccountCsv()
    {
        static std::string cached;
        if (!cached.empty()) return cached;   // only cache a non-empty result
        QueryResult q = LoginDatabase.Query(
            "SELECT `id` FROM `account` WHERE `username` LIKE 'rndbot%'");
        std::ostringstream csv;
        uint32 n = 0;
        bool first = true;
        if (q) do {
            if (!first) csv << ',';
            first = false;
            csv << q->Fetch()[0].Get<uint32>();
            ++n;
        } while (q->NextRow());
        cached = csv.str();
        LOG_INFO("module",
            "[WowPsParty Henchmen] random-bot account pool: {} accounts", n);
        return cached;   // empty stays uncached → retried next call
    }

    // class id -> default henchman role.
    static char const* ClassDefaultRole(uint8 cls)
    {
        switch (cls)
        {
            case 1:  return "tank";    // Warrior
            case 2:  return "tank";    // Paladin (prot-ish)
            case 6:  return "tank";    // Death Knight
            case 5:  return "healer";  // Priest
            case 7:  return "healer";  // Shaman
            case 11: return "healer";  // Druid
            default: return "dps";     // Hunter/Rogue/Mage/Warlock
        }
    }

    uint32 HenchmanHireCost(uint8 level)
    {
        // ~2.5 silver per level — "a little money" (L18 ≈ 45s, L80 = 2g).
        return uint32(level) * 250u;
    }

    // Map a set of learned talent-rank spell ids to a combat role, by resolving
    // each to its tree (tabpage 0/1/2) via the Talent DBC, summing points spent
    // per tree and taking the dominant one. This is what keeps a Fury warrior or
    // Shadow priest from getting the flat class-default role-aware rotation.
    // Falls back to `fallback` when the char has no talents yet (very low level).
    static std::string RoleFromTalents(uint8 cls,
                                       std::unordered_set<uint32> const& known,
                                       std::string const& fallback)
    {
        if (known.empty()) return fallback;
        uint32 points[3] = { 0, 0, 0 };
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* tal = sTalentStore.LookupEntry(i);
            if (!tal) continue;
            TalentTabEntry const* tab = sTalentTabStore.LookupEntry(tal->TalentTab);
            if (!tab || tab->tabpage > 2) continue;
            // Highest known rank of this talent = points spent in it.
            for (int rank = int(tal->RankID.size()) - 1; rank >= 0; --rank)
                if (tal->RankID[rank] && known.count(tal->RankID[rank]))
                {
                    points[tab->tabpage] += uint32(rank + 1);
                    break;
                }
        }
        if (!points[0] && !points[1] && !points[2]) return fallback;

        uint8 tree = 0;
        if (points[1] > points[tree]) tree = 1;
        if (points[2] > points[tree]) tree = 2;

        // tabpage order is the standard WotLK tree layout per class.
        switch (cls)
        {
            case 1:  return tree == 2 ? "tank" : "dps";                       // Warrior: Prot
            case 2:  return tree == 0 ? "healer" : (tree == 1 ? "tank" : "dps"); // Paladin: Holy/Prot/Ret
            case 5:  return tree == 2 ? "dps" : "healer";                     // Priest: Shadow vs Disc/Holy
            case 6:  return tree == 0 ? "tank" : "dps";                       // DK: Blood
            case 7:  return tree == 2 ? "healer" : "dps";                     // Shaman: Resto
            case 11: // Druid: Balance=dps, Resto=healer, Feral=bear(tank)/cat(dps)
                if (tree == 0) return "dps";       // Balance
                if (tree == 2) return "healer";    // Restoration
                // Feral covers BOTH the bear tank and the cat DPS build, so a
                // tree-only check wrongly tanks every feral. Disambiguate the
                // way mod-playerbots does: a bear maxes Thick Hide (rank 3,
                // talent spell 16931); a cat doesn't. No rank 3 → it's a cat.
                return known.count(16931) ? "tank" : "dps";
            default: return "dps";                                            // Hunter/Rogue/Mage/Warlock
        }
    }

    // Role of an OFFLINE candidate, read from character_talent in the DB. Used
    // pre-hire (the bot isn't in world yet).
    static std::string InferHenchmanRole(uint32 guid, uint8 cls,
                                         std::string const& fallback)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `spell` FROM `character_talent` WHERE `guid` = {}", guid);
        if (!q) return fallback;
        std::unordered_set<uint32> known;
        do { known.insert(q->Fetch()[0].Get<uint32>()); } while (q->NextRow());
        return RoleFromTalents(cls, known, fallback);
    }

    // Role of a LIVE bot, read from its in-memory learned talents. Used right
    // after a re-level re-rolls the spec, when the DB write is still an async
    // transaction in flight (a DB read would race and return the old spec).
    static std::string InferHenchmanRoleLive(Player* p, std::string const& fallback)
    {
        if (!p) return fallback;
        std::unordered_set<uint32> known;
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* tal = sTalentStore.LookupEntry(i);
            if (!tal) continue;
            for (int rank = int(tal->RankID.size()) - 1; rank >= 0; --rank)
                // HasTalent (not HasSpell): keyed exactly by talent-rank spell id,
                // so a trained class spell can't masquerade as a spent talent.
                if (tal->RankID[rank] && p->HasTalent(tal->RankID[rank], p->GetActiveSpec()))
                {
                    known.insert(tal->RankID[rank]);
                    break;
                }
        }
        return RoleFromTalents(p->getClass(), known, fallback);
    }

    // Canonical per-class, per-role starter rotation. Built as a priority list
    // (the engine fires the highest-priority rule whose conditions pass and whose
    // spell is castable, falling through on cooldown/unknown). Two properties make
    // these robust without knowing the bot's spec or level:
    //   * spell NAMES, so the engine resolves the highest rank the bot knows;
    //   * unknown spells fall through (FindKnownSpellByName == 0), so a single
    //     rotation can list EVERY spec's key abilities and degrade gracefully —
    //     a low-level or off-spec bot simply skips what it hasn't learned and
    //     drops to the filler it does know.
    // The melee-vs-ranged hybrid DPS specs (enhancement shaman, feral cat vs
    // balance druid) additionally gate rules on `primary_tree:N` so a melee spec
    // doesn't try to stand and cast (and the follow layer kites/closes to match);
    // each such branch keeps a non-spec-gated filler so no spec is left ruleless.
    // `role` ("tank"/"healer"/"dps") tunes the warrior/DK stance/presence, the
    // hybrids' heal aggressiveness, and tank threat/taunt rules. Empty role →
    // the class's default role. Shared by `.party preset` and henchman hire.
    std::string DefaultRotationForClass(uint8 cls, std::string const& role)
    {
        std::string r = role.empty() ? std::string(ClassDefaultRole(cls)) : role;
        bool const isTank   = (r == "tank");
        bool const isHealer = (r == "healer");

        std::vector<std::string> rules;
        auto add = [&rules](char const* cond, char const* action, int prio,
                            char const* flags = nullptr)
        {
            std::string rule = std::string(cond) + '|' + action + '|' + std::to_string(prio);
            // Optional 4th DSL field (e.g. "disabled") — present in the default
            // rotation but skipped by the engine until the user ticks it on in
            // the editor.
            if (flags && *flags)
            {
                rule += '|';
                rule += flags;
            }
            rules.emplace_back(std::move(rule));
        };

        switch (cls)
        {
            case 1: // Warrior
                if (isTank)
                {
                    // Survival cooldowns first (both off the GCD in-game). Shield
                    // Wall = ~40% DR panic; Last Stand = +30% max health to buy the
                    // healer time. Gated low so they stay dormant until real danger.
                    add("self_health<20", "buff_self:Shield Wall", 95);
                    add("target_casting&target_interruptible", "cast:Shield Bash", 92);
                    add("enemy_loose_in_range", "cast_loose_enemy:Taunt", 90);
                    add("self_health<30", "buff_self:Last Stand", 89);
                    add("always", "buff_self:Defensive Stance", 84);
                    add("always", "buff_self:Commanding Shout", 80);
                    add("has_target", "cast:Shield Slam", 74);
                    // Maintain Shield Block for steady mitigation (skips while the
                    // block buff is up; refreshes when it lapses and is off CD).
                    add("has_target", "buff_self:Shield Block", 72);
                    add("enemies_in_melee>2", "cast:Thunder Clap", 70);
                    add("has_target", "cast:Revenge", 66);
                    add("enemies_in_melee>2&target_missing_aura:Demoralizing Shout", "cast:Demoralizing Shout", 58);
                    add("has_target", "cast:Devastate", 52);
                    add("has_target", "cast:Sunder Armor", 44);
                    // Ranged pull, preferred order. A fresh tank has ~0 rage, so
                    // open with the FREE ranged weapon if one's equipped (gun/bow/
                    // crossbow/thrown → `shoot`) — gated out_of_combat so it's the
                    // pull opener, not an in-combat filler that'd cost melee uptime.
                    // Heroic Throw (also free, 30y, but a 1-min cooldown) is the
                    // fallback when there's no ranged weapon AND doubles as the
                    // in-combat ranged threat filler while the pack closes.
                    add("out_of_combat&has_target", "shoot", 43);
                    add("has_target", "cast:Heroic Throw", 42);
                    add("self_rage>45", "cast:Heroic Strike", 30);
                }
                else
                {
                    // Berserker Stance, not Battle: it's the only stance that
                    // enables both the interrupt (Pummel) and Whirlwind, and the
                    // core strikes (Mortal Strike/Bloodthirst/Slam/Execute/Heroic
                    // Strike/Cleave) are stanceless. Battle-only abilities
                    // (Overpower/Rend/Thunder Clap) are dropped — they'd be dead
                    // weight here and Pummel is the more valuable pick.
                    add("target_casting&target_interruptible", "cast:Pummel", 92);
                    add("target_health<20", "cast:Execute", 90);
                    add("always", "buff_self:Berserker Stance", 82);
                    add("always", "buff_self:Battle Shout", 80);
                    add("has_target", "cast:Mortal Strike", 72);
                    add("has_target", "cast:Bloodthirst", 71);
                    add("enemies_in_melee>1", "cast:Whirlwind", 62);
                    add("enemies_in_melee>2&self_rage>45", "cast:Cleave", 50);
                    add("has_target", "cast:Slam", 40);
                    add("self_rage>55", "cast:Heroic Strike", 30);
                }
                break;

            case 2: // Paladin
                if (isHealer)
                {
                    add("party_lowest_health<60", "cast_party_lowest:Holy Shock", 94);
                    add("party_lowest_health<35", "cast_party_lowest:Holy Light", 90);
                    add("tank_health<55", "cast_party_lowest:Flash of Light", 86);
                    add("party_has_dead", "rez_party:Redemption", 82);
                    add("party_has_magic", "cure_party:Cleanse", 78);
                    add("party_has_poison", "cure_party:Cleanse", 77);
                    add("party_has_disease", "cure_party:Cleanse", 76);
                    add("party_lowest_health<75", "cast_party_lowest:Flash of Light", 70);
                    add("always", "buff_self:Devotion Aura", 62);
                    add("always", "buff_self:Seal of Wisdom", 58);
                    // Healers only DPS at near-full mana — conserve for healing.
                    add("self_mana>85&has_target", "cast:Judgement of Light", 36);
                }
                else if (isTank)
                {
                    // Panic full self-heal before anything else (shares Forbearance
                    // with Divine Protection below, so the <15 gate keeps it as the
                    // true last resort once the 50% DR can't save us).
                    add("self_health<15", "cast_self:Lay on Hands", 95);
                    add("enemy_loose_in_range", "cast_loose_enemy:Hand of Reckoning", 92);
                    // Paladins have no true interrupt in 3.3.5a — stun the caster
                    // with Hammer of Justice (no-op on stun-immune bosses).
                    add("target_casting&target_interruptible", "cast:Hammer of Justice", 91);
                    // 50% damage reduction when in danger (off the GCD in-game; the
                    // rotation just spends one tick popping it). Forbearance.
                    add("self_health<40", "buff_self:Divine Protection", 89);
                    add("party_lowest_health<35", "cast_party_lowest:Flash of Light", 86);
                    add("always", "buff_self:Righteous Fury", 82);
                    add("always", "buff_self:Devotion Aura", 80);
                    add("always", "buff_self:Seal of Righteousness", 78);
                    // Holy Shield: block-chance + holy-damage shield. Core prot
                    // ability that was missing — both steady mitigation AND threat.
                    add("has_target", "buff_self:Holy Shield", 76);
                    add("party_has_magic", "cure_party:Cleanse", 74);
                    add("has_target", "cast:Avenger's Shield", 70);
                    add("enemies_in_melee>2", "cast:Consecration", 66);
                    // Shield of Righteousness (lvl 75): big single-target threat,
                    // scales with block value — was missing entirely. Falls through
                    // harmlessly below 75 / with no shield equipped.
                    add("has_target", "cast:Shield of Righteousness", 63);
                    add("has_target", "cast:Hammer of the Righteous", 60);
                    add("enemies_in_melee>2", "cast:Holy Wrath", 56);
                    add("has_target", "cast:Judgement of Light", 48);
                    add("has_target", "cast:Crusader Strike", 40);
                }
                else
                {
                    add("self_health<15", "cast_self:Lay on Hands", 95);
                    // Stun-interrupt — paladins have no kick in 3.3.5a.
                    add("target_casting&target_interruptible", "cast:Hammer of Justice", 90);
                    add("party_lowest_health<35", "cast_party_lowest:Flash of Light", 86);
                    add("target_health<20", "cast:Hammer of Wrath", 84);
                    add("always", "buff_self:Retribution Aura", 80);
                    // +20% damage burst cooldown — popped as soon as it's off CD
                    // (high prio is free: buff_self falls through while it's active
                    // or on cooldown, so it only ever wins the tick it actually
                    // fires). Forbearance, but Ret has no panic that needs it.
                    add("has_target", "buff_self:Avenging Wrath", 79);
                    // Seal of Command cleaves up to 3 targets (better on packs);
                    // fall back to Seal of Righteousness only if SoC isn't known —
                    // the self_missing gate stops SoR overwriting an active SoC.
                    add("always", "buff_self:Seal of Command", 78);
                    add("self_missing_aura:Seal of Command", "buff_self:Seal of Righteousness", 77);
                    // Refill mana before the rotation stalls out (off the GCD).
                    add("self_mana<20", "buff_self:Divine Plea", 75);
                    // The Art of War proc → instant, free Exorcism on ANY target.
                    // This is the main Ret damage that was being left on the table:
                    // the old unconditional Exorcism just failed on non-undead.
                    add("self_has_aura:The Art of War", "cast:Exorcism", 73);
                    add("has_target", "cast:Judgement of Wisdom", 70);
                    add("has_target", "cast:Crusader Strike", 64);
                    add("has_target", "cast:Divine Storm", 60);
                    add("enemies_in_melee>2", "cast:Consecration", 54);
                    add("enemies_in_melee>2", "cast:Holy Wrath", 48);
                    // Without the proc, Exorcism is only castable on undead/demon.
                    add("target_type_undead", "cast:Exorcism", 43);
                    add("target_type_demon", "cast:Exorcism", 42);
                }
                break;

            case 3: // Hunter
                // Pet summon/revive only out of combat: both fail in combat
                // (SPELL_FAILED_DONT_REPORT) and, left ungated, fire every tick —
                // holding the hunter mid-fight while it tries to re-summon.
                add("out_of_combat&pet_missing", "cast_self:Call Pet", 90);
                add("out_of_combat&pet_dead", "cast_self:Revive Pet", 88);
                add("target_casting&target_interruptible", "cast:Silencing Shot", 87);
                add("target_health<20", "cast:Kill Shot", 86);
                add("pet_health<50", "cast_pet:Mend Pet", 78);
                add("always", "buff_self:Aspect of the Hawk", 74);
                add("target_missing_aura:Hunter's Mark", "cast:Hunter's Mark", 70);
                add("target_missing_aura:Serpent Sting", "cast:Serpent Sting", 66);
                add("has_target", "cast:Chimera Shot", 62);
                add("has_target", "cast:Explosive Shot", 61);
                add("has_target", "cast:Aimed Shot", 56);
                // AoE on the densest mob CLUSTER, not hostiles near the hunter
                // (who stands at range). Volley (placed ground AoE) leads on a
                // pack; Multi-Shot is the instant fallback.
                add("enemies_clustered:8>2", "cast:Volley", 54);
                add("enemies_clustered:8>2", "cast:Multi-Shot", 52);
                // Forced into melee (no tank peeled the mob off us, so we stand
                // our ground): a hunter can't shoot in the dead zone, so weave
                // the melee strikes. Gate on target_attacking_me (we have AGGRO)
                // as well as a mob in melee range — otherwise, when the victim is
                // our far ranged target, Raptor Strike fails out-of-range and the
                // cast path walks us INTO melee, then we back out, forever.
                add("enemies_in_melee>0&target_attacking_me", "cast:Raptor Strike", 51);
                add("enemies_in_melee>0&target_attacking_me", "cast:Mongoose Bite", 50);
                add("has_target", "cast:Arcane Shot", 46);
                add("has_target", "cast:Steady Shot", 36);
                break;

            case 4: // Rogue
                add("target_casting&target_interruptible", "cast:Kick", 92);
                add("out_of_combat&self_missing_aura:Stealth", "cast_self:Stealth", 80);
                // Execute finisher: a dying target gets Eviscerated NOW with as few
                // as 3 combo points rather than waiting for 5 (or wasting a bleed) —
                // above Slice and Dice so we don't refresh a buff on a corpse.
                add("target_health<20&self_combo>2", "cast:Eviscerate", 78);
                // Slice and Dice only on ELITES/bosses (long fights). On normal
                // trash, maintaining SnD just burns the first 2 combo every time
                // the short buff lapses, and with rogue energy being slow the mob
                // dies before combo ever banks to 5 — so Rupture/Eviscerate never
                // fire. Gating SnD to elites lets trash combo flow to the finisher
                // (the <20% execute above, or a full Eviscerate) instead.
                add("self_missing_aura:Slice and Dice&self_combo>1&target_is_elite", "cast:Slice and Dice", 76);
                // Rupture only if the bleed has time to pay off: target_ttd>8 is
                // TRUE for an unmeasured/long-lived mob (fresh boss) and FALSE for a
                // trash mob about to die — so we never waste a finisher's combo on a
                // DoT that won't tick. (The <20% execute above already pre-empts it
                // for dying targets at 3+ combo.)
                add("self_combo>4&target_missing_aura:Rupture&target_ttd>8", "cast:Rupture", 70);
                add("self_combo>4", "cast:Eviscerate", 66);
                add("enemies_in_melee>2", "cast:Fan of Knives", 58);
                add("has_target", "cast:Mutilate", 46);
                add("has_target", "cast:Hemorrhage", 44);
                add("has_target", "cast:Sinister Strike", 40);
                break;

            case 5: // Priest
                if (isHealer)
                {
                    add("party_lowest_health<30", "cast_party_lowest:Flash Heal", 96);
                    add("tank_health<50", "cast_party_lowest:Greater Heal", 90);
                    add("party_has_dead", "rez_party:Resurrection", 84);
                    add("party_has_magic", "cure_party:Dispel Magic", 80);
                    add("party_has_disease", "cure_party:Cure Disease", 79);
                    add("party_lowest_health<75", "cast_party_lowest:Power Word: Shield", 74);
                    add("party_lowest_health<70", "cast_party_lowest_hot:Renew", 68);
                    add("party_lowest_health<75", "cast_party_lowest:Flash Heal", 62);
                    add("always", "cast_party_missing:Power Word: Fortitude", 54);
                    // Healers only DPS at near-full mana — conserve for healing.
                    add("self_mana>85&target_missing_aura:Shadow Word: Pain", "cast:Shadow Word: Pain", 34);
                    add("self_mana>85&has_target", "cast:Smite", 30);
                }
                else
                {
                    add("party_lowest_health<30", "cast_party_lowest:Flash Heal", 86);
                    add("target_casting&target_interruptible", "cast:Silence", 80);
                    add("always", "buff_self:Shadowform", 76);
                    add("always", "cast_party_missing:Power Word: Fortitude", 56);
                    add("target_missing_aura:Shadow Word: Pain", "cast:Shadow Word: Pain", 70);
                    add("target_missing_aura:Vampiric Touch", "cast:Vampiric Touch", 66);
                    add("target_missing_aura:Devouring Plague", "cast:Devouring Plague", 62);
                    add("has_target", "cast:Mind Blast", 54);
                    add("has_target", "cast:Mind Flay", 40);
                    add("has_target", "cast:Smite", 28);
                }
                break;

            case 6: // Death Knight
                if (isTank)
                {
                    add("enemy_loose_in_range", "cast_loose_enemy:Dark Command", 90);
                    // Icebound Fortitude = damage reduction + stun immunity panic
                    // (baseline). Vampiric Blood = +max HP & +healing taken (Blood
                    // talent; falls through if unspecced). Both off the GCD.
                    add("self_health<35", "buff_self:Icebound Fortitude", 89);
                    add("self_health<45", "buff_self:Vampiric Blood", 88);
                    add("target_casting&target_interruptible", "cast:Mind Freeze", 86);
                    add("self_health<55", "cast:Death Strike", 80);
                    add("self_health<40", "cast_self:Rune Tap", 78);
                    add("always", "buff_self:Blood Presence", 74);
                    add("target_missing_aura:Frost Fever", "cast:Icy Touch", 70);
                    add("target_missing_aura:Blood Plague", "cast:Plague Strike", 69);
                    add("enemies_in_melee>2", "cast:Death and Decay", 64);
                    add("enemies_in_melee>2", "cast:Pestilence", 58);
                    add("has_target", "cast:Heart Strike", 52);
                    add("has_target", "cast:Blood Strike", 48);
                    add("has_target", "cast:Rune Strike", 44);
                    add("has_target", "cast:Death Coil", 38);
                }
                else
                {
                    add("target_casting&target_interruptible", "cast:Mind Freeze", 86);
                    add("self_health<50", "cast:Death Strike", 80);
                    add("always", "buff_self:Unholy Presence", 74);
                    add("target_missing_aura:Frost Fever", "cast:Icy Touch", 70);
                    add("target_missing_aura:Blood Plague", "cast:Plague Strike", 69);
                    add("enemies_in_melee>2", "cast:Death and Decay", 64);
                    add("enemies_in_melee>2", "cast:Pestilence", 58);
                    add("has_target", "cast:Scourge Strike", 54);
                    add("has_target", "cast:Obliterate", 53);
                    add("has_target", "cast:Heart Strike", 50);
                    add("has_target", "cast:Blood Strike", 46);
                    add("has_target", "cast:Frost Strike", 42);
                    add("has_target", "cast:Death Coil", 40);
                }
                break;

            case 7: // Shaman
                if (isHealer)
                {
                    add("party_lowest_health<30", "cast_party_lowest:Healing Wave", 96);
                    add("tank_health<55", "cast_party_lowest:Lesser Healing Wave", 90);
                    add("party_has_dead", "rez_party:Ancestral Spirit", 84);
                    add("party_has_poison", "cure_party:Cure Toxins", 80);
                    add("party_has_disease", "cure_party:Cure Toxins", 79);
                    add("party_has_curse", "cure_party:Cleanse Spirit", 78);
                    add("party_lowest_health<70", "cast_party_lowest_hot:Riptide", 72);
                    add("party_lowest_health<75", "cast_party_lowest:Lesser Healing Wave", 64);
                    add("always", "buff_self:Water Shield", 58);
                    // Healers only DPS at near-full mana — conserve for healing.
                    add("self_mana>85&target_missing_aura:Flame Shock", "cast:Flame Shock", 34);
                    add("self_mana>85&has_target", "cast:Lightning Bolt", 30);
                }
                else
                {
                    add("party_lowest_health<30", "cast_party_lowest:Healing Wave", 86);
                    add("target_casting&target_interruptible", "cast:Wind Shear", 82);
                    add("always", "buff_self:Lightning Shield", 78);
                    add("target_missing_aura:Flame Shock", "cast:Flame Shock", 72);
                    // ENHANCEMENT (talent tree 1): melee. Stormstrike/Lava Lash,
                    // Earth Shock as the instant dump, and an INSTANT Lightning
                    // Bolt at 5 stacks of Maelstrom Weapon. The follow layer
                    // treats tree-1 shamans as melee so they close to contact.
                    add("primary_tree:1&has_target", "cast:Stormstrike", 70);
                    add("primary_tree:1&has_target", "cast:Lava Lash", 64);
                    add("primary_tree:1&self_aura_stacks:Maelstrom Weapon>4", "cast:Lightning Bolt", 60);
                    // ELEMENTAL (tree 0): ranged nuker.
                    add("primary_tree:0&has_target", "cast:Lava Burst", 66);
                    // Cluster-gated AoE (both specs cast it from range).
                    add("enemies_clustered:8>2", "cast:Chain Lightning", 56);
                    add("has_target", "cast:Earth Shock", 46);     // enh dump / ele instant
                    add("has_target", "cast:Lightning Bolt", 38);  // filler
                }
                break;

            case 8: // Mage
                add("target_casting&target_interruptible", "cast:Counterspell", 88);
                add("self_missing_aura:Ice Barrier", "cast_self:Ice Barrier", 80);
                add("enemies_in_melee>0", "cast_self:Frost Nova", 76);
                add("always", "buff_self:Frost Armor", 72);
                add("target_missing_aura:Living Bomb", "cast:Living Bomb", 68);
                add("always", "cast_party_missing:Arcane Intellect", 60);
                // Placed AoE keys off the densest mob CLUSTER (3+ enemies within
                // ~8y of each OTHER), not hostiles near the mage — a ranged mage
                // stands well back, so a bot-centred count reads 0 on a pack it
                // could nuke. Blizzard leads by default; a Fire-specced mage
                // (primary_tree:1) prefers Flamestrike. The lower Blizzard rule
                // doubles as a Fire mage's fallback when Flamestrike is on CD.
                add("enemies_clustered:8>2&primary_tree:1", "cast:Flamestrike", 60);
                add("enemies_clustered:8>2", "cast:Blizzard", 58);
                add("enemies_clustered:8>2", "cast:Flamestrike", 56);
                add("has_target", "cast:Frostbolt", 44);
                add("has_target", "cast:Fireball", 42);
                add("has_target", "cast:Arcane Blast", 40);
                // Disabled by default: henchmen recover for free (eat/drink
                // below) and don't need conjured items, and a henchman mage
                // burning mana to conjure between pulls just slows the party.
                // Kept in the rotation (flagged "disabled") so a player running
                // a mage as one of their own alt-bots can tick it on in the
                // editor to stock the shared bags.
                add("out_of_combat&shared_drink<5", "cast_self:Conjure Water", 18, "disabled");
                add("out_of_combat&shared_food<5", "cast_self:Conjure Food", 16, "disabled");
                break;

            case 9: // Warlock
                add("pet_missing", "cast_self:Summon Imp", 88);
                add("self_health<35", "cast:Death Coil", 82);
                add("self_missing_aura:Demon Armor", "cast_self:Demon Armor", 76);
                add("target_missing_aura:Immolate", "cast:Immolate", 72);
                add("target_missing_aura:Corruption", "cast:Corruption", 70);
                add("target_missing_aura:Curse of Agony", "cast:Curse of Agony", 66);
                add("target_missing_aura:Unstable Affliction", "cast:Unstable Affliction", 62);
                // AoE on the densest mob CLUSTER, not hostiles near the warlock
                // (who stands at range). Seed of Corruption (spreads off the
                // current target) leads; Rain of Fire (placed ground AoE) backs
                // it up on a fresh pack with no Seed yet.
                add("enemies_clustered:8>2&target_missing_aura:Seed of Corruption", "cast:Seed of Corruption", 58);
                add("enemies_clustered:8>2", "cast:Rain of Fire", 56);
                add("target_health<25", "cast:Drain Soul", 54);
                add("has_target", "cast:Haunt", 50);
                add("has_target", "cast:Incinerate", 44);
                add("has_target", "cast:Shadow Bolt", 42);
                break;

            case 11: // Druid
                if (isHealer)
                {
                    add("party_lowest_health<30", "cast_party_lowest:Healing Touch", 96);
                    add("tank_health<55", "cast_party_lowest:Regrowth", 90);
                    add("party_has_dead", "rez_party:Revive", 84);
                    add("party_has_curse", "cure_party:Remove Curse", 80);
                    add("party_has_poison", "cure_party:Abolish Poison", 79);
                    add("party_lowest_health<70", "cast_party_lowest_hot:Rejuvenation", 72);
                    add("party_lowest_health<75", "cast_party_lowest_hot:Regrowth", 66);
                    add("party_lowest_health<85", "cast_party_lowest:Nourish", 60);
                    add("always", "cast_party_missing:Mark of the Wild", 54);
                    // Healers only DPS at near-full mana — conserve for healing.
                    add("self_mana>85&target_missing_aura:Moonfire", "cast:Moonfire", 32);
                    add("self_mana>85&has_target", "cast:Wrath", 28);
                }
                else if (isTank)
                {
                    add("enemy_loose_in_range", "cast_loose_enemy:Growl", 90);
                    // Barkskin = 20% DR (any form, off the GCD). Survival Instincts
                    // = +30% max health (feral talent). Frenzied Regeneration =
                    // rage->health self-heal (bear). All gated on real danger.
                    add("self_health<50", "buff_self:Barkskin", 89);
                    add("self_health<35", "buff_self:Survival Instincts", 88);
                    add("self_health<35", "buff_self:Frenzied Regeneration", 87);
                    add("always", "buff_self:Bear Form", 84);
                    add("enemies_in_melee>2", "cast:Swipe", 70);
                    add("has_target", "cast:Mangle (Bear)", 68);
                    add("target_missing_aura:Lacerate", "cast:Lacerate", 64);
                    add("has_target", "cast:Lacerate", 58);
                    add("target_missing_aura:Faerie Fire (Feral)", "cast:Faerie Fire (Feral)", 54);
                    add("has_target", "cast:Maul", 40);
                }
                else
                {
                    add("party_lowest_health<30", "cast_party_lowest:Healing Touch", 84);
                    add("always", "cast_party_missing:Mark of the Wild", 60);
                    // FERAL CAT (talent tree 1): melee combo build/spend, like a
                    // rogue. The follow layer treats tree-1 druids as melee. Shift
                    // to Cat Form only in combat (has_target) so it can still mount
                    // / buff out of combat. Savage Roar gated to elites for the
                    // same reason rogue Slice and Dice is — on trash it just eats
                    // the combo the damage finishers need (see LESSONS).
                    add("primary_tree:1&has_target&self_missing_aura:Cat Form", "buff_self:Cat Form", 82);
                    add("primary_tree:1&has_target&self_energy<35", "cast_self:Tiger's Fury", 79);
                    add("primary_tree:1&self_missing_aura:Savage Roar&self_combo>0&target_is_elite", "cast:Savage Roar", 77);
                    add("primary_tree:1&target_health<25&self_combo>2", "cast:Ferocious Bite", 75);
                    add("primary_tree:1&self_combo>4&target_missing_aura:Rip&target_ttd>8", "cast:Rip", 73);
                    add("primary_tree:1&self_combo>4", "cast:Ferocious Bite", 70);
                    add("primary_tree:1&target_missing_aura:Rake", "cast:Rake", 66);
                    add("primary_tree:1&has_target", "cast:Mangle (Cat)", 58);
                    add("primary_tree:1&has_target", "cast:Claw", 48);   // pre-Mangle fallback
                    // Leave the form out of combat so the cat can drink/mount/buff
                    // (above eat/drink at 12-14 so it drops form FIRST).
                    add("primary_tree:1&out_of_combat&self_has_aura:Cat Form", "cancel_form", 16);
                    // BALANCE — and the universal NON-feral fallback (!tree 1),
                    // so a no-talent low-level druid OR one a user manually flips
                    // to dps while Resto-specced still nukes instead of standing
                    // idle. Moonkin Form falls through harmlessly if untalented.
                    add("!primary_tree:1&has_target&self_missing_aura:Moonkin Form", "buff_self:Moonkin Form", 80);
                    add("!primary_tree:1&target_missing_aura:Moonfire", "cast:Moonfire", 72);
                    add("!primary_tree:1&target_missing_aura:Insect Swarm", "cast:Insect Swarm", 68);
                    // Cluster-gated ground AoE: balance casts from range.
                    add("!primary_tree:1&enemies_clustered:8>2", "cast:Hurricane", 58);
                    add("!primary_tree:1&has_target", "cast:Starfire", 46);
                    add("!primary_tree:1&has_target", "cast:Wrath", 44);
                    add("!primary_tree:1&out_of_combat&self_has_aura:Moonkin Form", "cancel_form", 16);
                }
                break;

            default:
                return "";
        }

        // Out-of-combat recovery (item-free — the drink/eat action regenerates
        // for free). Lowest priority so it only kicks in with nothing else to
        // do. Eat (health) for everyone; drink (mana) only for mana classes.
        // Thresholds sit near-full: the rule engine is stateless, so the bot
        // recovers up to the threshold and stops — a low <50 mana cap meant it
        // quit at half and had to re-trigger several times to ever reach full.
        // At <90 it tops off to near-full in one sitting, then natural regen
        // closes the gap.
        add("out_of_combat&self_health<90", "eat", 12);
        if (cls == 2 || cls == 3 || cls == 5 || cls == 7
            || cls == 8 || cls == 9 || cls == 11)   // Pala/Hunter/Priest/Shaman/Mage/Warlock/Druid
            add("out_of_combat&self_mana<90", "drink", 14);

        // Swarmed → wand. When >3 mobs are in melee on a DPS caster, cast-time
        // nukes are useless (constant pushback never lets them land), so revert to
        // the WAND — instant, free, reliable — instead of spamming a Frostbolt that
        // never completes. HIGH priority so it beats the single-target cast nukes
        // while swarmed; the condition keeps it dormant otherwise (interrupts and
        // self-defense above still win). Pairs with the engine STANDING the caster
        // its ground (no kiting) so the tank can pull the pack off it. Wand classes
        // only; shaman/druid use the instant shocks/Moonfire already in their kits.
        if (!isHealer && (cls == 5 || cls == 8 || cls == 9))   // shadow Priest / Mage / Warlock
            add("enemies_in_melee>3&has_target", "wand", 55);

        // Wand filler for the classes that can equip one (Priest/Mage/Warlock).
        // Lowest combat priority by design: a DPS caster only falls to it when
        // its mana-gated nukes can't fire (i.e. it's out of mana — the cast
        // fails and the engine drops through to here), and a healer only when
        // nothing higher needs doing. Free damage, no mana, no cooldown.
        if (cls == 5 || cls == 8 || cls == 9)   // Priest / Mage / Warlock
            add("has_target", "wand", 11);

        std::string out;
        for (size_t i = 0; i < rules.size(); ++i)
        {
            if (i) out += ';';
            out += rules[i];
        }
        return out;
    }

    // Query offline random-pool chars of the given classes near `level`, of the
    // given races (so henchmen match the player's faction).
    static void QueryHenchCandidates(std::string const& acctCsv,
        std::string const& classCsv, std::string const& raceCsv,
        uint8 lo, uint8 hi, uint8 level,
        uint32 limit, std::vector<HenchmanCandidate>& out)
    {
        if (acctCsv.empty() || raceCsv.empty()) return;
        (void)level;  // band already constrains proximity; order is pure random
        // ORDER BY RAND(), not ABS(level-L): proximity ordering made the same
        // few exact-level chars rank top every time, so "Refresh" re-rolled to
        // an identical-looking list. Random within the (already tight) level
        // band gives a genuinely different set each refresh.
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid`,`name`,`class`,`level` FROM `characters` "
            "WHERE `account` IN ({}) AND `online` = 0 AND `class` IN ({}) "
            "AND `race` IN ({}) AND `level` BETWEEN {} AND {} "
            "ORDER BY RAND() LIMIT {}",
            acctCsv, classCsv, raceCsv, uint32(lo), uint32(hi), limit);
        if (!q) return;
        do {
            Field* f = q->Fetch();
            HenchmanCandidate c;
            c.guid  = f[0].Get<uint32>();
            c.name  = f[1].Get<std::string>();
            c.cls   = f[2].Get<uint8>();
            c.level = f[3].Get<uint8>();
            // Show the role the bot ACTUALLY is (from its talents) — the same
            // inference the hire uses — not the flat class default. Otherwise
            // the list "marks" every druid a healer while a feral one hires in
            // as a tank (the reported bug).
            c.role  = InferHenchmanRole(c.guid, c.cls, ClassDefaultRole(c.cls));
            out.push_back(std::move(c));
        } while (q->NextRow());
    }

    std::vector<HenchmanCandidate> BuildHenchmanCandidates(Player* requester)
    {
        std::vector<HenchmanCandidate> out;
        if (!requester) return out;
        std::string const acctCsv = RndbotAccountCsv();
        if (acctCsv.empty()) return out;

        uint8 const L  = requester->GetLevel();
        // ±4 band (was ±2): a wider eligible pool so each randomized Refresh
        // shows a meaningfully different set instead of recycling the same
        // handful of exact-level chars. Still "~your level".
        uint8 const lo = uint8(std::max(1, int(L) - 4));
        uint8 const hi = uint8(std::min(80, int(L) + 4));

        // Same-faction only — a Horde henchman can't group with / heal an
        // Alliance player. Race ids by team.
        std::string const raceCsv = (requester->GetTeamId() == TEAM_ALLIANCE)
            ? "1,3,4,7,11"   // Human, Dwarf, Night Elf, Gnome, Draenei
            : "2,5,6,8,10";  // Orc, Undead, Tauren, Troll, Blood Elf

        // ~20 candidates: 4 tanks (Warr/Pala/DK/Druid), 4 healers
        // (Priest/Pala/Shaman/Druid), 12 dps (any class), each a random draw
        // from the ±4 band so Refresh re-rolls.
        QueryHenchCandidates(acctCsv, "1,2,6,11", raceCsv, lo, hi, L, 4, out);
        QueryHenchCandidates(acctCsv, "2,5,7,11", raceCsv, lo, hi, L, 4, out);
        QueryHenchCandidates(acctCsv, "1,2,3,4,5,6,7,8,9,11", raceCsv, lo, hi, L, 12, out);

        // Force the role label per the slot it was drawn for (the same char
        // could appear via two class sets; dedupe by guid keeping first role).
        std::vector<HenchmanCandidate> deduped;
        for (auto& c : out)
        {
            bool seen = false;
            for (auto const& d : deduped) if (d.guid == c.guid) { seen = true; break; }
            if (!seen) deduped.push_back(c);
        }

        // Coverage guarantee: every faction-valid class should be hirable in
        // THIS bracket. If the ±4 band produced no candidate for a class, pull
        // the nearest-level pool char of that class (any level) and present it at
        // the player's level — it's re-leveled to the player when hired (see the
        // spawn handler in HireHenchman). The RNDBOT pool holds every class per
        // faction (CreateRandomBots makes one per class per account), so this
        // needs no new characters; the rare empty case just leaves that class out.
        bool present[12] = { false };
        for (auto const& c : deduped) if (c.cls < 12) present[c.cls] = true;
        static uint8 const kAllClasses[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 11 };
        for (uint8 cls : kAllClasses)
        {
            if (present[cls]) continue;
            if (cls == 6 && L < 55) continue;   // Death Knights start at level 55
            QueryResult mq = CharacterDatabase.Query(
                "SELECT `guid`,`name` FROM `characters` "
                "WHERE `account` IN ({}) AND `online` = 0 AND `class` = {} AND `race` IN ({}) "
                "ORDER BY ABS(CAST(`level` AS SIGNED) - {}) ASC LIMIT 1",
                acctCsv, uint32(cls), raceCsv, uint32(L));
            if (!mq) continue;   // pool genuinely lacks this faction+class
            Field* mf = mq->Fetch();
            HenchmanCandidate c;
            c.guid  = mf[0].Get<uint32>();
            c.name  = mf[1].Get<std::string>();
            c.cls   = cls;
            c.level = L;   // shown + costed at the player's level; re-leveled on hire
            // Inferred from current talents like the other path. NOTE: an
            // out-of-band pick is re-rolled by Randomize on spawn, so the role
            // is re-derived AFTER that re-roll in HireHenchman — this label is
            // the best pre-hire estimate.
            c.role  = InferHenchmanRole(c.guid, cls, ClassDefaultRole(cls));
            deduped.push_back(std::move(c));
        }
        return deduped;
    }

    // Set the group's loot rule based on whether any henchman is present:
    // henchman in party → GROUP_LOOT (rolls), else FREE_FOR_ALL (premade).
    static void UpdateGroupLootForHenchmen(Player* leader)
    {
        if (!leader) return;
        Group* g = leader->GetGroup();
        if (!g) return;
        bool const hasHench = WowPsParty::CountHenchmenFor(leader->GetGUID()) > 0;
        g->SetLootMethod(hasHench ? GROUP_LOOT : FREE_FOR_ALL);
        g->SendUpdate();
        LOG_INFO("module", "[WowPsParty Henchmen] loot method -> {} (henchmen present={})",
                 hasHench ? "GROUP_LOOT" : "FREE_FOR_ALL", hasHench);
    }

    // ===== Consumable upkeep (ammo + poisons) ===============================
    //
    // Managed bots hard-return out of mod-playerbots' UpdateAI, so its InitAmmo /
    // ImbueWithPoisonAction upkeep never fires for them — a hunter henchman
    // spawns with an empty quiver and a rogue never poisons its blades. This
    // restores both, item-based (so it depletes and refills naturally) and
    // level-appropriate, without dumping stacks the user didn't ask for.

    // Add `count` of `itemId` into the bot's bags. Returns the resulting Item* or
    // nullptr if there was no room (we never force / overflow).
    static Item* GiveItem(Player* bot, uint32 itemId, uint32 count)
    {
        if (!bot || !itemId || !count) return nullptr;
        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) != EQUIP_ERR_OK)
            return nullptr;
        return bot->StoreNewItem(dest, itemId, true,
                                 Item::GenerateItemRandomPropertyId(itemId));
    }

    // Highest-rank consumable from a priority-ordered (best-first) list whose
    // RequiredLevel the bot meets. 0 if none usable yet.
    static uint32 BestUsable(Player* bot, std::vector<uint32> const& prioritized)
    {
        for (uint32 id : prioritized)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id);
            if (proto && proto->RequiredLevel <= bot->GetLevel())
                return id;
        }
        return 0;
    }

    // Hunters (and any bot wielding a bow/gun/crossbow) keep ~1 stack of the
    // right ammo and refill before it runs dry. We respect an ammo the bot
    // already has set — only topping that same type up — so a player-chosen
    // arrow isn't swapped out from under them.
    static void MaintainAmmo(Player* bot)
    {
        Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!ranged) return;
        uint32 subClass = 0;
        switch (ranged->GetTemplate()->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_GUN:                                  subClass = ITEM_SUBCLASS_BULLET; break;
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:                            subClass = ITEM_SUBCLASS_ARROW;  break;
            default: return;   // thrown / wand: no ammo slot
        }

        uint32 const REFILL_TO    = 1000;  // one stack — enough, not a bag-filler
        uint32 const REFILL_BELOW = 250;

        uint32 entry = bot->GetUInt32Value(PLAYER_AMMO_ID);
        if (entry)
        {
            // RE-VALIDATE against the CURRENT weapon. A hunter that swapped
            // bow<->gun keeps its old ammo selected (e.g. arrows on a gun), and
            // the old code just topped that stale ammo up forever — every shot
            // then failed NO_AMMO while Hunter's Mark (no ammo) still worked
            // ("hunter only auto-attacks, shots exec_fail"). If the set ammo is
            // the wrong subclass or the weapon can't use it, drop it and re-pick.
            ItemTemplate const* at = sObjectMgr->GetItemTemplate(entry);
            bool const usable = at && at->Class == ITEM_CLASS_PROJECTILE
                && at->SubClass == subClass
                && bot->CanUseAmmo(entry) == EQUIP_ERR_OK;
            if (usable)
            {
                // Right ammo — top up only if running low.
                uint32 const have = bot->GetItemCount(entry);
                if (have >= REFILL_BELOW) return;
                GiveItem(bot, entry, REFILL_TO - have);
                bot->SetAmmo(entry);
                return;
            }
            entry = 0;   // stale/mismatched — fall through to pick the right ammo
        }

        // No ammo set — pick the first level/type-appropriate ammo the bot can
        // actually USE. SetAmmo silently rejects (via CanUseAmmo) anything the bot
        // can't equip, which would leave the quiver empty and every shot failing
        // NO_AMMO, so we pre-check CanUseAmmo and skip rejects instead of blindly
        // taking the highest-ilvl entry.
        for (uint32 candidate : sRandomItemMgr.GetAmmo(bot->GetLevel(), subClass))
        {
            if (sObjectMgr->GetItemTemplate(candidate)
                && bot->CanUseAmmo(candidate) == EQUIP_ERR_OK)
            {
                entry = candidate;
                break;
            }
        }
        if (!entry) return;
        uint32 const have = bot->GetItemCount(entry);
        if (have < REFILL_TO) GiveItem(bot, entry, REFILL_TO - have);
        bot->SetAmmo(entry);
        LOG_INFO("module", "[WowPsParty Provision] {} ammo set to {} (equipped={})",
                 bot->GetName(), entry, bot->GetUInt32Value(PLAYER_AMMO_ID) == entry ? 1 : 0);
    }

    // Rogues keep a small supply of the best instant + deadly poison they can
    // use and apply them (instant MH / deadly OH) whenever a weapon's temporary
    // enchant slot is empty. ImbueItem queues the real use-item packet, so the
    // poison is consumed and refilled like a player's would be.
    static void MaintainPoisons(Player* bot)
    {
        if (bot->IsInCombat()) return;            // can't imbue mid-fight
        PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!botAI) return;

        static std::vector<uint32> const instant = {
            INSTANT_POISON_IX, INSTANT_POISON_VIII, INSTANT_POISON_VII, INSTANT_POISON_VI,
            INSTANT_POISON_V, INSTANT_POISON_IV, INSTANT_POISON_III, INSTANT_POISON_II, INSTANT_POISON };
        static std::vector<uint32> const deadly = {
            DEADLY_POISON_IX, DEADLY_POISON_VIII, DEADLY_POISON_VII, DEADLY_POISON_VI,
            DEADLY_POISON_V, DEADLY_POISON_IV, DEADLY_POISON_III, DEADLY_POISON_II, DEADLY_POISON };

        uint32 const instantId = BestUsable(bot, instant);
        uint32 const deadlyId  = BestUsable(bot, deadly);
        if (!instantId && !deadlyId) return;     // too low level for any poison

        // Keep a few on hand; top a depleted type back up to 20 (one slot).
        auto ensure = [bot](uint32 id) {
            if (!id) return;
            uint32 const have = bot->GetItemCount(id);
            if (have < 5) GiveItem(bot, id, 20 - have);
        };
        ensure(instantId);
        ensure(deadlyId);

        // Resolve the poison actually in the bags by walking the WHOLE rank list
        // (best-first). If the top rank couldn't be stored — full bags — a lower
        // rank already on hand still beats leaving the blade clean.
        auto held = [botAI](std::vector<uint32> const& list) -> Item* {
            for (uint32 id : list)
                if (Item* it = botAI->FindConsumable(id)) return it;
            return nullptr;
        };
        Item* const heldInstant = held(instant);
        Item* const heldDeadly  = held(deadly);

        // Applying breaks stealth; drop it so the imbue lands, the rotation
        // re-stealths next tick. Only happens when a weapon is actually unpoisoned
        // (~hourly), so the flicker is negligible.
        auto imbue = [&](uint8 slot, Item* first, Item* second) {
            Item* w = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!w || w->GetTemplate()->Class != ITEM_CLASS_WEAPON) return;
            if (w->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != 0) return;
            Item* poison = first ? first : second;
            if (!poison) return;
            if (bot->HasAuraType(SPELL_AURA_MOD_STEALTH))
                bot->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);
            botAI->ImbueItem(poison, slot);
        };
        imbue(EQUIPMENT_SLOT_MAINHAND, heldInstant, heldDeadly);
        imbue(EQUIPMENT_SLOT_OFFHAND,  heldDeadly, heldInstant);
    }

    // Warrior tanks pull from range (see TankLeadEngagement), which needs a
    // thrown weapon in the ranged slot. Only WARRIORS can equip thrown weapons,
    // so this is warrior-tank-only. Equips the best vendor-grade thrown weapon
    // the bot's level allows, ONCE — only when the ranged slot is empty, so a
    // player-chosen ranged item on an alt is never clobbered.
    static void MaintainTankThrown(Player* bot)
    {
        if (bot->getClass() != CLASS_WARRIOR) return;
        if (WowPsParty::RoleForGuid(bot->GetGUID()) != "tank") return;
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED)) return;

        // class=2 weapon, subclass=16 thrown, InventoryType=25 INVTYPE_THROWN.
        // Common..rare only (no heirloom/artifact). Best item level the bot can
        // actually use; CanEquipNewItem makes the final class/level call.
        QueryResult q = WorldDatabase.Query(
            "SELECT entry FROM item_template "
            "WHERE class = 2 AND subclass = 16 AND InventoryType = 25 "
            "AND RequiredLevel <= {} AND Quality BETWEEN 1 AND 3 "
            "ORDER BY ItemLevel DESC, RequiredLevel DESC LIMIT 10", uint32(bot->GetLevel()));
        if (!q) return;
        do
        {
            uint32 const entry = (*q)[0].Get<uint32>();
            uint16 dest = 0;
            if (bot->CanEquipNewItem(NULL_SLOT, dest, entry, false) == EQUIP_ERR_OK)
            {
                bot->EquipNewItem(dest, entry, true);
                LOG_INFO("module", "[WowPsParty Provision] equipped thrown weapon {} on {} (lvl {})",
                         entry, bot->GetName(), uint32(bot->GetLevel()));
                return;
            }
        } while (q->NextRow());
    }

    void MaintainBotConsumables(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !bot->GetSession()) return;

        // Throttle hard — this touches inventory and queues packets. Once every
        // ~12s per bot is plenty for "never run dry / stay poisoned".
        static std::unordered_map<uint32, uint32> lastMs;
        static std::mutex lastMsMutex;
        {
            std::lock_guard<std::mutex> lock(lastMsMutex);
            uint32 const now = getMSTime();
            uint32& last = lastMs[bot->GetGUID().GetCounter()];
            if (last != 0 && now - last < 12000) return;
            last = now;
        }

        uint8 const cls = bot->getClass();
        if (cls == CLASS_ROGUE)   MaintainPoisons(bot);
        if (cls == CLASS_WARRIOR) MaintainTankThrown(bot);   // before ammo: equips the thrown wpn
        MaintainAmmo(bot);   // any class that wields a bow/gun/thrown benefits
    }

    bool HireHenchman(Player* requester, uint32 candidateGuid,
                      std::string const& role, std::string& outMsg)
    {
        if (!requester || !requester->GetSession())
        { outMsg = "No session."; return false; }
        uint32 const account = requester->GetSession()->GetAccountId();

        // Validate candidate is an offline random-pool char.
        std::string const acctCsv = RndbotAccountCsv();
        QueryResult q = CharacterDatabase.Query(
            "SELECT `level`,`online`,`account`,`class` FROM `characters` WHERE `guid` = {}",
            candidateGuid);
        if (!q)
        {
            outMsg = "Henchman not found.";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: not in DB", candidateGuid);
            return false;
        }
        Field* f = q->Fetch();
        uint8 const level   = f[0].Get<uint8>();
        bool  const online  = f[1].Get<uint8>() != 0;
        uint8 const cls     = f[3].Get<uint8>();
        if (online)
        {
            outMsg = "That henchman is busy — pick another (Refresh the list).";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: already online", candidateGuid);
            return false;
        }

        // Party-space cap: leader + 4 companions max. Count current followers
        // (alts + henchmen) from the directive registry — works even before
        // the WoW group object exists (solo + first henchman).
        uint32 const followers = WowPsParty::CountFollowersFor(requester->GetGUID());
        if (followers >= 4)
        {
            outMsg = "Your party is full (4 companions).";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: party full (followers={})",
                     candidateGuid, followers);
            return false;
        }

        // Gold check + deduct (after all synchronous validation). A candidate
        // drawn from outside the ±4 band (a widened pick covering a class missing
        // from this bracket) is re-leveled to the player on spawn, so cost it at
        // the player's level, not its stored pool level.
        int  const lvlDiff   = int(level) - int(requester->GetLevel());
        bool const outOfBand = lvlDiff > 4 || lvlDiff < -4;
        uint8 const effLevel = outOfBand ? uint8(requester->GetLevel()) : level;
        uint32 const cost = HenchmanHireCost(effLevel);
        if (requester->GetMoney() < cost)
        {
            outMsg = "Not enough gold to hire.";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: gold {} < cost {}",
                     candidateGuid, requester->GetMoney(), cost);
            return false;
        }

        PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(requester);
        if (!mgr)
        {
            outMsg = "Bot manager not ready — try again in a moment.";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: no PlayerbotMgr", candidateGuid);
            return false;
        }

        requester->ModifyMoney(-int32(cost));

        ObjectGuid const henchGuid = ObjectGuid::Create<HighGuid::Player>(candidateGuid);
        // The client sends the class-default role; override it with the bot's
        // real spec so the role-aware rotation + directive (targeting, lead-tank)
        // match what the henchman actually is. Falls back to the sent role.
        std::string const useRole =
            InferHenchmanRole(candidateGuid, cls, role.empty() ? std::string("dps") : role);

        // Register as a henchman BEFORE spawning so the patched AddPlayerBot
        // permission check (WowPsParty_IsHenchman_Trampoline) lets the cross-
        // account random-pool char in.
        WowPsParty::AddHenchmanDirective(account, henchGuid, requester->GetGUID(), useRole);

        // Restore this henchman's persisted loadout. party_loadout is keyed by
        // character guid, so a henchman the player edited in a PRIOR hire keeps
        // those changes when re-invited (Kevin's "next time I invite this guy he
        // still has my rotation"). Falls back to sensible defaults when the bot
        // has never been customised:
        //   rotation  : saved priority_actions_json, else class default — so the
        //               henchman runs OUR combat AI (LoS approach, no melee-
        //               stacking) with sensible spells, same engine as heroes.
        //   targetmode: saved strategies_csv, else tank -> "loose" / "master".
        //   lead       : saved glyphs_csv ("0" = off), else ON.
        bool hadCustomRotation = false;   // gates the post-relevel rebuild below
        {
            QueryResult lq = CharacterDatabase.Query(
                "SELECT `priority_actions_json`,`strategies_csv`,`glyphs_csv` "
                "FROM `party_loadout` WHERE `guid` = {}", candidateGuid);
            std::string savedRot, savedMode, savedLead;
            if (lq)
            {
                Field* lf = lq->Fetch();
                savedRot  = lf[0].Get<std::string>();
                savedMode = lf[1].Get<std::string>();
                savedLead = lf[2].Get<std::string>();
            }
            hadCustomRotation = !savedRot.empty();

            WowPsParty::RotationCacheSet(candidateGuid,
                WowPsParty::ParseRotationString(
                    savedRot.empty() ? DefaultRotationForClass(cls, useRole) : savedRot));

            WowPsParty::TargetModeCacheSet(candidateGuid,
                !savedMode.empty() ? savedMode
                                   : (useRole == "tank" ? "loose" : "master"));

            WowPsParty::LeadDungeonCacheSet(candidateGuid, savedLead != "0");
        }

        mgr->AddPlayerBot(henchGuid, account);

        // The spawn is async (login query holder). After a short delay: if the
        // bot arrived, group it + set loot; if it never arrived, undo the hire
        // (drop the directive, refund the gold) so we never leak a directive or
        // charge the player for a no-show.
        ObjectGuid const leaderGuid = requester->GetGUID();
        requester->m_Events.AddEventAtOffset([leaderGuid, henchGuid, cost, cls, hadCustomRotation]()
        {
            Player* lead = ObjectAccessor::FindConnectedPlayer(leaderGuid);
            Player* hen  = ObjectAccessor::FindConnectedPlayer(henchGuid);
            if (!hen || !hen->IsInWorld())
            {
                WowPsParty::RemoveFollower(henchGuid);
                if (lead)
                {
                    lead->ModifyMoney(int32(cost));   // refund the no-show
                    UpdateGroupLootForHenchmen(lead);
                    if (lead->GetSession())
                        ChatHandler(lead->GetSession()).PSendSysMessage(
                            "|cffff5555[WowPsParty]|r Henchman didn't arrive — refunded.");
                }
                LOG_WARN("module",
                    "[WowPsParty Henchmen] spawn no-show hench_guid={} — refunded {} "
                    "(bot not in world after delay)", henchGuid.GetCounter(), cost);
                return;
            }
            if (!lead) return;

            // Level-match a henchman drawn from outside the ±4 band (a widened
            // pick covering a class missing from this bracket) to the leader, so
            // it's level-appropriate. Same in-world factory path playerbots uses
            // in RandomizeFirst. Done BEFORE grouping so any group side-effect of
            // Randomize is absorbed by the AddMember below. NOTE: Randomize
            // regenerates level-appropriate GEAR for the new level (the pool
            // char's old gear was the wrong level anyway) — the saved rotation/
            // loadout is keyed by guid in our own cache, so it is untouched. Only
            // out-of-band picks hit this; in-band hires keep their gear.
            {
                int const d = int(hen->GetLevel()) - int(lead->GetLevel());
                if (d > 4 || d < -4)
                {
                    uint32 const oldLvl = hen->GetLevel();
                    uint32 const target = lead->GetLevel();
                    PlayerbotFactory factory(hen, target);
                    factory.Randomize(false);
                    hen->SaveToDB(false, false);
                    LOG_INFO("module",
                        "[WowPsParty Henchmen] re-leveled hench guid={} {} -> {} to match leader",
                        henchGuid.GetCounter(), oldLvl, target);

                    // Randomize re-rolled the talents, so the role we inferred
                    // before spawn (and the rotation built from it) may no longer
                    // match the spec. Re-derive from the now-saved talents and,
                    // unless the player has a saved custom rotation, rebuild the
                    // role-aware defaults — otherwise a re-leveled pick can keep a
                    // tank rotation on a now-DPS spec (the hire bug, post-relevel).
                    uint32 const guidLow = henchGuid.GetCounter();
                    std::string const freshRole =
                        InferHenchmanRoleLive(hen, ClassDefaultRole(cls));
                    WowPsParty::SetHenchmanRole(henchGuid, freshRole);
                    if (!hadCustomRotation)
                    {
                        WowPsParty::RotationCacheSet(guidLow,
                            WowPsParty::ParseRotationString(DefaultRotationForClass(cls, freshRole)));
                        WowPsParty::TargetModeCacheSet(guidLow,
                            freshRole == "tank" ? "loose" : "master");
                    }
                }
            }

            Group* g = lead->GetGroup();
            if (!g)
            {
                g = new Group();
                if (!g->Create(lead)) { delete g; return; }
                sGroupMgr->AddGroup(g);
            }
            if (!g->IsMember(henchGuid))
            {
                // The henchman may still be in a STALE group left over in the DB
                // from a previous LFG dungeon. Pull it out without tripping the
                // dismiss hook (which would instantly un-hire it), then add it to
                // the player's party. This also cleans up the stale group_member.
                if (hen->GetGroup())
                {
                    struct RegroupGuard {
                        ObjectGuid g;
                        ~RegroupGuard() { WowPsParty::SetHenchmanRegrouping(g, false); }
                    } guard{henchGuid};
                    WowPsParty::SetHenchmanRegrouping(henchGuid, true);
                    hen->RemoveFromGroup();   // flag cleared by guard, even on throw
                }
                g->AddMember(hen);
            }
            UpdateGroupLootForHenchmen(lead);
            // Loot/gather like the rest of the party.
            if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(hen))
            {
                ai->ChangeStrategy("+loot",   BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);
            }
        }, std::chrono::seconds(8));

        LOG_INFO("module",
            "[WowPsParty Henchmen] HIRE account={} hench_guid={} role={} level={} cost={}",
            account, candidateGuid, useRole, uint32(level), cost);
        outMsg = "Hired!";
        return true;
    }

    void DismissHenchman(Player* requester, uint32 henchGuid)
    {
        if (!requester || !requester->GetSession()) return;
        ObjectGuid const g = ObjectGuid::Create<HighGuid::Player>(henchGuid);
        if (!WowPsParty::IsHenchman(g)) return;   // only dismiss henchmen
        WowPsParty::RemoveFollower(g);
        WowPsParty::RotationCacheClear(henchGuid);
        WowPsParty::TargetModeCacheSet(henchGuid, "master");   // drop tank "loose"
        if (Player* hen = ObjectAccessor::FindConnectedPlayer(g))
            if (hen->GetGroup()) hen->RemoveFromGroup();
        // Log the bot out via the master's mgr regardless of whether it's
        // currently found in world — it may still be mid-spawn; not doing so
        // would orphan a default-AI companion with no directive until restart.
        if (PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(requester))
            mgr->LogoutPlayerBot(g);
        UpdateGroupLootForHenchmen(requester);
        LOG_INFO("module", "[WowPsParty Henchmen] DISMISS hench_guid={}", henchGuid);
    }

    // Dismiss a henchman identified only by guid — used by the group-removal
    // hook so that uninviting a henchman from the party (by ANY means, incl.
    // the stock WoW group UI) makes it stop following and despawn ("instant
    // hearth"). Drops the directive immediately (stops our follow ticker) and
    // defers the logout one tick — we may be called mid-group-removal, and
    // LogoutPlayerBot would otherwise re-enter group teardown for this member.
    void DismissHenchmanByGuid(ObjectGuid henchGuid)
    {
        if (!WowPsParty::IsHenchman(henchGuid)) return;
        ObjectGuid const leaderGuid = WowPsParty::GetLeaderFor(henchGuid);
        WowPsParty::RemoveFollower(henchGuid);
        WowPsParty::RotationCacheClear(henchGuid.GetCounter());
        WowPsParty::TargetModeCacheSet(henchGuid.GetCounter(), "master");
        LOG_INFO("module",
            "[WowPsParty Henchmen] henchman left party — dismissing hench_guid={}",
            henchGuid.GetCounter());
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader) return;
        leader->m_Events.AddEventAtOffset([leaderGuid, henchGuid]()
        {
            Player* l = ObjectAccessor::FindConnectedPlayer(leaderGuid);
            if (!l) return;
            // Remove the henchman from any group BEFORE logging it out, so no
            // stale group_member row survives (logout alone preserves group
            // membership). Otherwise a later re-hire respawns it into that ghost
            // group and the spawn re-grouping would re-trip the dismiss. The
            // directive is already gone (RemoveFollower above), so this removal's
            // own dismiss hook is a no-op.
            if (Player* hen = ObjectAccessor::FindConnectedPlayer(henchGuid))
                if (hen->GetGroup()) hen->RemoveFromGroup();
            if (PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(l))
                mgr->LogoutPlayerBot(henchGuid);
            UpdateGroupLootForHenchmen(l);
        }, std::chrono::milliseconds(200));
    }

    void DismissAllHenchmen(Player* requester)
    {
        if (!requester) return;
        // Collect this leader's henchmen, then dismiss each.
        std::vector<ObjectGuid> hench;
        {
            std::vector<ObjectGuid> guids;
            WowPsParty::GetPartyGuidsFor(requester->GetGUID(), guids);
            for (ObjectGuid const& gg : guids)
                if (WowPsParty::IsHenchman(gg)) hench.push_back(gg);
        }
        for (ObjectGuid const& gg : hench)
            DismissHenchman(requester, gg.GetCounter());
    }

    static uint32 FetchAccountForGuid(uint32 guid)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `account` FROM `characters` WHERE `guid` = {}", guid);
        if (!q)
            return 0;
        return q->Fetch()[0].Get<uint32>();
    }

    static std::vector<std::pair<uint8 /*slot*/, uint32 /*guid*/>> FetchPartyRows(uint32 accountId)
    {
        std::vector<std::pair<uint8, uint32>> rows;
        // JOIN characters so orphan rows (whose char was deleted) are never
        // treated as party members — they must not count toward the 5-slot cap,
        // must not be spawn-attempted as bots, and must not be invited to the
        // group. The OnPlayerDelete hook removes such rows, but this keeps every
        // consumer self-healing if one ever lingers.
        QueryResult q = CharacterDatabase.Query(
            "SELECT ap.`slot`, ap.`guid` FROM `account_party` ap "
            "JOIN `characters` c ON c.`guid` = ap.`guid` "
            "WHERE ap.`account` = {} ORDER BY ap.`slot`", accountId);
        if (!q)
            return rows;
        do
        {
            Field* f = q->Fetch();
            rows.emplace_back(f[0].Get<uint8>(), f[1].Get<uint32>());
        } while (q->NextRow());
        return rows;
    }

    EnrollResult PartyMgr::Enroll(Player* requestor, uint32 targetGuid, std::string const& targetName)
    {
        if (!requestor || !requestor->GetSession())
            return EnrollResult::DatabaseError;

        uint32 const requestorAccount = requestor->GetSession()->GetAccountId();

        // Default: enroll the requestor's own currently-logged-in character.
        if (targetGuid == 0)
            targetGuid = requestor->GetGUID().GetCounter();

        // Verify the target character belongs to the requestor's account.
        uint32 const targetAccount = FetchAccountForGuid(targetGuid);
        if (targetAccount == 0)
            return EnrollResult::TargetNotFound;
        if (targetAccount != requestorAccount)
            return EnrollResult::TakenByAnotherAccount;

        // Already enrolled?
        QueryResult existing = CharacterDatabase.Query(
            "SELECT `account`, `slot` FROM `account_party` WHERE `guid` = {}", targetGuid);
        if (existing)
        {
            uint32 const ownerAccount = existing->Fetch()[0].Get<uint32>();
            if (ownerAccount == requestorAccount)
                return EnrollResult::AlreadyEnrolled;
            return EnrollResult::TakenByAnotherAccount;
        }

        // Self-heal: drop orphan rows whose character was deleted (e.g. chars
        // removed before the OnPlayerDelete hook existed). Synchronous so the
        // slot scan below sees a clean table — otherwise an orphan row at the
        // chosen slot would collide on the (account, slot) primary key, and
        // orphans would falsely push the account to "5/5 full".
        CharacterDatabase.DirectExecute(
            "DELETE ap FROM `account_party` ap "
            "LEFT JOIN `characters` c ON c.`guid` = ap.`guid` "
            "WHERE ap.`account` = {} AND c.`guid` IS NULL", requestorAccount);

        // Find next free slot 0..4 in this account.
        auto const rows = FetchPartyRows(requestorAccount);
        if (rows.size() >= PARTY_SIZE)
            return EnrollResult::PartyFull;

        uint8 nextSlot = 0;
        for (uint8 candidate = 0; candidate < PARTY_SIZE; ++candidate)
        {
            bool taken = false;
            for (auto const& row : rows)
            {
                if (row.first == candidate) { taken = true; break; }
            }
            if (!taken) { nextSlot = candidate; break; }
        }

        // Transactional insert: account_party row + characters.party_slot column.
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append(
            "INSERT INTO `account_party` (`account`, `slot`, `guid`, `is_active_on_login`) "
            "VALUES ({}, {}, {}, {})",
            requestorAccount, nextSlot, targetGuid,
            (nextSlot == 0 ? 1u : 0u));
        tx->Append(
            "UPDATE `characters` SET `party_slot` = {} WHERE `guid` = {}",
            nextSlot, targetGuid);
        CharacterDatabase.CommitTransaction(tx);

        LOG_INFO("module",
                 "[WowPsParty] enroll: account={} guid={} name={} slot={}",
                 requestorAccount, targetGuid, targetName, nextSlot);

        return EnrollResult::Ok;
    }

    bool PartyMgr::Leave(Player* requestor)
    {
        if (!requestor || !requestor->GetSession())
            return false;

        uint32 const guid = requestor->GetGUID().GetCounter();
        uint32 const account = requestor->GetSession()->GetAccountId();

        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append(
            "DELETE FROM `account_party` WHERE `account` = {} AND `guid` = {}",
            account, guid);
        tx->Append(
            "UPDATE `characters` SET `party_slot` = NULL WHERE `guid` = {}",
            guid);
        // Also clear any loadout for this character.
        tx->Append(
            "DELETE FROM `party_loadout` WHERE `guid` = {}", guid);
        CharacterDatabase.CommitTransaction(tx);

        LOG_INFO("module", "[WowPsParty] leave: account={} guid={}", account, guid);
        return true;
    }

    std::vector<PartyMember> PartyMgr::GetParty(uint32 accountId)
    {
        std::vector<PartyMember> out;

        QueryResult q = CharacterDatabase.Query(
            "SELECT ap.`slot`, ap.`guid`, c.`name`, c.`class`, c.`level` "
            "FROM `account_party` ap "
            "JOIN `characters` c ON c.`guid` = ap.`guid` "
            "WHERE ap.`account` = {} ORDER BY ap.`slot`", accountId);
        if (!q)
            return out;

        do
        {
            Field* f = q->Fetch();
            PartyMember m;
            m.slot    = f[0].Get<uint8>();
            m.guid    = f[1].Get<uint32>();
            m.name    = f[2].Get<std::string>();
            m.classId = f[3].Get<uint8>();
            m.level   = f[4].Get<uint8>();
            m.online  = ObjectAccessor::FindPlayerByLowGUID(m.guid) != nullptr;
            out.push_back(std::move(m));
        } while (q->NextRow());

        return out;
    }

    std::optional<uint8> PartyMgr::GetSlotForGuid(uint32 guid)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `party_slot` FROM `characters` WHERE `guid` = {}", guid);
        if (!q)
            return std::nullopt;

        Field* f = q->Fetch();
        if (f[0].IsNull())
            return std::nullopt;
        return f[0].Get<uint8>();
    }

    void PartyMgr::OnActiveLogin(Player* active)
    {
        if (!active || !active->GetSession())
            return;

        uint32 const account = active->GetSession()->GetAccountId();
        uint32 const activeGuid = active->GetGUID().GetCounter();

        // Load this account's feature toggles up front (cached). Solo mode
        // (companions off) keeps the enrolled roster + rotations/talents on
        // disk — we just don't spawn the bots or form the party group.
        AccountSettingsRefreshFromDB(account);
        if (!GetAccountSettings(account).spawnCompanions)
        {
            LOG_INFO("module",
                "[WowPsParty] OnActiveLogin: account={} companions OFF (solo) — "
                "not spawning party.", account);
            return;
        }

        auto const rows = FetchPartyRows(account);
        if (rows.empty())
            return;  // not enrolled — nothing to spawn

        // mod-playerbots' PlayerScript::OnLogin creates the PlayerbotMgr. AC
        // dispatches PlayerScript hooks in REGISTRATION order, not
        // alphabetical (earlier comment was wrong) -- and our PartyHooks
        // script registers first, so on first login `botMgr` may be null
        // here. Defer the bot-spawn body by 1s via the player's event queue
        // so mod-playerbots has a chance to wire up its manager.
        ObjectGuid const activeObjGuid2 = active->GetGUID();
        active->m_Events.AddEventAtOffset([activeObjGuid2, account, activeGuid, rows]()
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(activeObjGuid2);
            if (!p) return;
            PlayerbotMgr* botMgr = sPlayerbotsMgr.GetPlayerbotMgr(p);
            if (!botMgr)
            {
                LOG_WARN("module",
                         "[WowPsParty] OnActiveLogin (deferred): still no PlayerbotMgr "
                         "for guid={}; mod-playerbots is genuinely missing. Idle party "
                         "members will NOT spawn. Re-login should fix.", activeGuid);
                return;
            }
            // WoW party is capped at 5 members. Active player counts as 1,
            // so spawn at most 4 bots. Extra enrolled chars beyond that just
            // sit out this session.
            uint8 spawned = 0;
            for (auto const& row : rows)
            {
                uint32 const guid = row.second;
                RotationCacheRefreshFromDB(guid);
                TargetModeRefreshFromDB(guid);
                LeadDungeonRefreshFromDB(guid);
                if (guid == activeGuid) continue;
                if (spawned >= 4) break;
                ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
                botMgr->AddPlayerBot(og, account);
                ++spawned;
            }
            LOG_INFO("module",
                "[WowPsParty] OnActiveLogin (deferred): spawned {} idle bot(s)", spawned);

            // Install follow directives now that the bots are loaded. This
            // ALSO acts as the "is a party-of-5 bot" gate used by the
            // patched PlayerbotAI::UpdateAI — without it, the AI doesn't
            // know to suppress the default rotation and bots fall back to
            // their class strategy (priests Smite, mages Fireball, etc.).
            WowPsParty::SetActiveFollowers(account,
                ObjectGuid::Create<HighGuid::Player>(activeGuid));
        }, std::chrono::seconds(1));

        ObjectGuid const activeObjGuid = active->GetGUID();

        // Ensure all 5 are in one Group with the active as leader and FFA
        // loot. Run after a short delay so mod-playerbots' bot-spawn callbacks
        // have settled into the world.
        active->m_Events.AddEventAtOffset([activeObjGuid]()
        {
            Player* leader = ObjectAccessor::FindConnectedPlayer(activeObjGuid);
            if (!leader || !leader->GetSession()) return;
            uint32 const account = leader->GetSession()->GetAccountId();

            auto const rows = FetchPartyRows(account);
            std::unordered_set<uint32> enrolled;
            for (auto const& row : rows) enrolled.insert(row.second);

            // Purge leftover henchmen BEFORE (re)building the group. Henchmen are
            // temporary (gone on dismiss/logout), but the WoW group is saved to
            // DB, so on the next login it still lists last session's henchmen as
            // offline members — the player would have to kick them by hand. Their
            // directives were already cleared on logout, so we can't use
            // IsHenchman here; instead remove any group member that isn't the
            // leader and isn't one of this account's enrolled alts. (This party
            // group only ever holds the player + their alts + henchmen, so
            // "not enrolled" == henchman.) Done first because RemoveMember can
            // DISBAND the group when it drops below 2 — we re-acquire it after.
            if (Group* existing = leader->GetGroup())
            {
                std::vector<ObjectGuid> toRemove;
                for (auto const& slot : existing->GetMemberSlots())
                    if (slot.guid != leader->GetGUID() &&
                        !enrolled.count(slot.guid.GetCounter()))
                        toRemove.push_back(slot.guid);
                for (ObjectGuid const& g : toRemove)
                {
                    existing->RemoveMember(g);  // may delete the group object
                    LOG_INFO("module",
                        "[WowPsParty] login purge: removed stale henchman guid={} "
                        "from party group", g.GetCounter());
                }
            }

            // Re-acquire the group — the purge above may have disbanded it.
            Group* group = leader->GetGroup();
            if (!group)
            {
                group = new Group();
                if (!group->Create(leader))
                {
                    delete group;
                    return;
                }
                sGroupMgr->AddGroup(group);
            }
            group->SetLootMethod(FREE_FOR_ALL);

            // Walk the party and invite every member that isn't already in.
            for (auto const& row : rows)
            {
                ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(row.second);
                if (og == leader->GetGUID()) continue;
                Player* mem = ObjectAccessor::FindConnectedPlayer(og);
                if (!mem || mem->GetGroup() == group) continue;
                if (mem->GetGroup())
                    mem->RemoveFromGroup();
                group->AddMember(mem);
            }

            // Enable auto-loot on every bot in the party so kills get looted
            // without the user having to right-click every corpse. mod-playerbots'
            // "+loot" strategy in BOT_STATE_NON_COMBAT triggers LootAction on
            // nearby unlooted corpses; "+gather" picks up herbs/ore. Idempotent —
            // ChangeStrategy on an already-enabled strategy is a no-op.
            for (auto const& row : rows)
            {
                ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(row.second);
                Player* mem = ObjectAccessor::FindConnectedPlayer(og);
                if (!mem) continue;
                if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(mem))
                {
                    ai->ChangeStrategy("+loot", BOT_STATE_NON_COMBAT);
                    ai->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);
                }
            }
        }, std::chrono::seconds(6));

        LOG_INFO("module",
                 "[WowPsParty] OnActiveLogin: account={} active_guid={} -- bot spawn deferred by 1s",
                 account, activeGuid);
    }

}
