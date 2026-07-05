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
#include "LFGMgr.h"             // sLFGMgr->GetLFGDungeon — read a dungeon's level range for LFG scaling
#include "LFG.h"                // lfg::LFGDungeonData
#include "SpellMgr.h"           // henchman down-level spell sanitize
#include "SpellInfo.h"          // SpellLevel / LEARN_SPELL effect inspection
#include "Bag.h"                // henchman inventory clear (GetBagByPos/GetBagSize)
#include "Unit.h"
#include "WorldSession.h"

// mod-playerbots (AC's modules build adds every subdir of every module to the
// include path, so no `Bot/` prefix is needed)
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "RandomItemMgr.h"      // sRandomItemMgr.GetAmmo
#include "PlayerbotFactory.h"   // re-level a widened henchman pick to the player
#include "PlayerbotAIConfig.h"  // randomClassSpecIndex — map a talent tree to its spec template
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
    void LeadDistRefreshFromDB(uint32 guidLow);
    void EngageRangeRefreshFromDB(uint32 guidLow);
    void AnchorTankRefreshFromDB(uint32 guidLow);
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
            "`kill_xp_rate` SMALLINT UNSIGNED NOT NULL DEFAULT 200, "
            "`lfg_autofill_optout` TINYINT NOT NULL DEFAULT 0)");
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
        if (columnMissing("lfg_autofill_optout"))
            CharacterDatabase.DirectExecute(
                "ALTER TABLE `party_account_settings` "
                "ADD COLUMN `lfg_autofill_optout` TINYINT NOT NULL DEFAULT 0");
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

    void EnsureRosterOrderColumn()
    {
        // The roster panel lets the player hand-sort ALL their characters; the
        // chosen order persists in characters.roster_order (per-account display
        // rank, NULL = not yet placed). MySQL has no ADD COLUMN IF NOT EXISTS, so
        // probe information_schema first — a bare ADD on an existing column errors
        // 1064 and AC aborts the worldserver on startup.
        bool const missing = !CharacterDatabase.Query(
            "SELECT 1 FROM `information_schema`.`COLUMNS` "
            "WHERE `TABLE_SCHEMA` = DATABASE() "
            "AND `TABLE_NAME` = 'characters' "
            "AND `COLUMN_NAME` = 'roster_order'");
        if (missing)
            CharacterDatabase.DirectExecute(
                "ALTER TABLE `characters` ADD COLUMN `roster_order` SMALLINT UNSIGNED DEFAULT NULL");
    }

    void AccountSettingsRefreshFromDB(uint32 account)
    {
        PartySettings s;  // all-ON default
        QueryResult q = CharacterDatabase.Query(
            "SELECT `spawn_companions`,`shared_inventory`,`shared_gear`,"
            "`shared_progression`,`quest_xp_rate`,`kill_xp_rate`,`lfg_autofill_optout` "
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
            s.lfgAutofillOptOut = f[6].Get<uint8>() != 0;
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
            {"shared_gear", 2}, {"shared_progression", 3},
            {"lfg_autofill_optout", 4} };
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
        if      (key == "spawn_companions")    s.spawnCompanions   = value;
        else if (key == "shared_inventory")    s.sharedInventory   = value;
        else if (key == "shared_gear")         s.sharedGear        = value;
        else if (key == "shared_progression")  s.sharedProgression = value;
        else if (key == "lfg_autofill_optout") s.lfgAutofillOptOut = value;
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

    // Dominant talent TREE (tabpage 0/1/2) from a set of learned talent-rank
    // spell ids, or -1 when the char has spent no talents yet (low level). Same
    // point-summing as RoleFromTalents; returns the raw tree so a per-spec
    // default rotation can be BAKED at hire time (mage Arcane/Fire/Frost etc.).
    static int TreeFromTalents(std::unordered_set<uint32> const& known)
    {
        if (known.empty()) return -1;
        uint32 points[3] = { 0, 0, 0 };
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* tal = sTalentStore.LookupEntry(i);
            if (!tal) continue;
            TalentTabEntry const* tab = sTalentTabStore.LookupEntry(tal->TalentTab);
            if (!tab || tab->tabpage > 2) continue;
            for (int rank = int(tal->RankID.size()) - 1; rank >= 0; --rank)
                if (tal->RankID[rank] && known.count(tal->RankID[rank]))
                {
                    points[tab->tabpage] += uint32(rank + 1);
                    break;
                }
        }
        if (!points[0] && !points[1] && !points[2]) return -1;
        int tree = 0;
        if (points[1] > points[uint32(tree)]) tree = 1;
        if (points[2] > points[uint32(tree)]) tree = 2;
        return tree;
    }

    // Dominant talent tree of an OFFLINE candidate, read from character_talent.
    // Used pre-hire (the bot isn't in world yet) to bake its per-spec rotation.
    static int DominantTalentTabDB(uint32 guid)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `spell` FROM `character_talent` WHERE `guid` = {}", guid);
        if (!q) return -1;
        std::unordered_set<uint32> known;
        do { known.insert(q->Fetch()[0].Get<uint32>()); } while (q->NextRow());
        return TreeFromTalents(known);
    }

    // Short spec abbreviation for the hire screen (e.g. "Holy", "Resto", "Frost").
    // Same dominant-tree resolution as RoleFromTalents; "" when the char has no
    // talents yet (very low level — the screen then shows just the role).
    static std::string SpecAbbrevFromTalents(uint8 cls,
                                             std::unordered_set<uint32> const& known)
    {
        if (known.empty()) return "";
        uint32 points[3] = { 0, 0, 0 };
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* tal = sTalentStore.LookupEntry(i);
            if (!tal) continue;
            TalentTabEntry const* tab = sTalentTabStore.LookupEntry(tal->TalentTab);
            if (!tab || tab->tabpage > 2) continue;
            for (int rank = int(tal->RankID.size()) - 1; rank >= 0; --rank)
                if (tal->RankID[rank] && known.count(tal->RankID[rank]))
                {
                    points[tab->tabpage] += uint32(rank + 1);
                    break;
                }
        }
        if (!points[0] && !points[1] && !points[2]) return "";
        uint8 tree = 0;
        if (points[1] > points[tree]) tree = 1;
        if (points[2] > points[tree]) tree = 2;

        // tabpage order = the standard WotLK tree layout per class.
        static char const* const SPEC[12][3] = {
            { "", "", "" },                       // 0 unused
            { "Arms", "Fury", "Prot" },           // 1  Warrior
            { "Holy", "Prot", "Ret" },            // 2  Paladin
            { "BM", "MM", "Surv" },               // 3  Hunter
            { "Assa", "Combat", "Subt" },         // 4  Rogue
            { "Disc", "Holy", "Shadow" },         // 5  Priest
            { "Blood", "Frost", "Unholy" },       // 6  Death Knight
            { "Ele", "Enh", "Resto" },            // 7  Shaman
            { "Arcane", "Fire", "Frost" },        // 8  Mage
            { "Affl", "Demo", "Destro" },         // 9  Warlock
            { "", "", "" },                       // 10 unused
            { "Balance", "Feral", "Resto" },      // 11 Druid
        };
        if (cls >= 1 && cls <= 11) return SPEC[cls][tree];
        return "";
    }

    // One DB read → both the candidate's role AND spec abbreviation (avoids a
    // second query per hire-screen row).
    static void InferHenchmanRoleAndSpec(uint32 guid, uint8 cls, std::string const& fallback,
                                         std::string& outRole, std::string& outSpec)
    {
        std::unordered_set<uint32> known;
        QueryResult q = CharacterDatabase.Query(
            "SELECT `spell` FROM `character_talent` WHERE `guid` = {}", guid);
        if (q)
            do { known.insert(q->Fetch()[0].Get<uint32>()); } while (q->NextRow());
        outRole = RoleFromTalents(cls, known, fallback);
        outSpec = SpecAbbrevFromTalents(cls, known);
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

    // Dominant talent TREE (tabpage 0/1/2) of a LIVE bot from its spent talents, or
    // -1 if it has none yet. Same point-summing as RoleFromTalents, but returns the
    // raw tree so a re-level can RESTORE the spec the player picked at the hire screen
    // (the Ret-paladin-downleveled-into-Holy bug) rather than keep the random re-roll.
    static int DominantTalentTabLive(Player* p)
    {
        if (!p) return -1;
        uint32 points[3] = { 0, 0, 0 };
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* tal = sTalentStore.LookupEntry(i);
            if (!tal) continue;
            TalentTabEntry const* tab = sTalentTabStore.LookupEntry(tal->TalentTab);
            if (!tab || tab->tabpage > 2) continue;
            for (int rank = int(tal->RankID.size()) - 1; rank >= 0; --rank)
                if (tal->RankID[rank] && p->HasTalent(tal->RankID[rank], p->GetActiveSpec()))
                {
                    points[tab->tabpage] += uint32(rank + 1);
                    break;
                }
        }
        if (!points[0] && !points[1] && !points[2]) return -1;
        int tree = 0;
        if (points[1] > points[uint32(tree)]) tree = 1;
        if (points[2] > points[uint32(tree)]) tree = 2;
        return tree;
    }

    // The talent TREE (tabpage 0/1/2) a class must be in to FILL a given role, or -1 when
    // the role doesn't pin a spec (dps takes any dps tree) or this class can't do the role.
    // Used to FORCE a tank/healer hire onto the right spec: a pool char picked for a
    // tank/healer slot can be DPS-specced (or, with no saved talents, default to the slot's
    // role while actually spawning DPS) — e.g. a warrior shown as tank that fights Arms
    // (Kevin: "arms warrior with the tank role"). Inverse of RoleFromTalents' tree→role map.
    static int DesiredTalentTabForRole(uint8 cls, std::string const& role)
    {
        if (role == "tank")
            switch (cls) { case 1: return 2; case 2: return 1; case 6: return 0; case 11: return 1; default: return -1; }
        if (role == "healer")
            switch (cls) { case 2: return 0; case 5: return 1; case 7: return 2; case 11: return 2; default: return -1; }
        return -1;   // dps (any dps spec is fine), or this class can't fill the role
    }

    // Dominant talent tree (0/1/2) for baking a per-spec rotation, -1 if none.
    // Public wrapper: a connected bot's live talents win (authoritative right
    // after a re-spec), else the offline character_talent table.
    int DominantTreeForGuid(uint32 guid)
    {
        if (Player* p = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(guid)))
            return DominantTalentTabLive(p);
        return DominantTalentTabDB(guid);
    }

    // Canonical per-class, per-SPEC starter rotation. Built as a priority list
    // (the engine fires the highest-priority rule whose conditions pass and whose
    // spell is castable, falling through on cooldown/unknown). Two properties make
    // these robust:
    //   * spell NAMES, so the engine resolves the highest rank the bot knows;
    //   * unknown spells fall through (FindKnownSpellByName == 0), so a low-level
    //     bot simply skips what it hasn't learned and drops to a filler it knows.
    // A class with multiple DPS specs is BAKED per spec from `tree` (the dominant
    // talent tab, see DominantTreeForGuid): each spec lists ONLY its own abilities
    // — a Frost mage never sees a Fire spell, an Arcane mage gets its own rotation
    // — instead of one cross-school list gated at runtime by `primary_tree:N`
    // (which leaked the opposing school whenever the spec's own spell was briefly
    // unavailable, the "frost mage casting Fireball" bug). `tree < 0` (pre-10 / no
    // talents / the spec-less `.party preset`) → a simple basic rotation.
    // `role` ("tank"/"healer"/"dps") still selects the tank/healer branch and
    // tunes warrior/DK stance/presence. Empty role → the class's default role.
    // Shared by `.party preset`, henchman hire, and the editor's Generate button.
    std::string DefaultRotationForClass(uint8 cls, std::string const& role, int tree)
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

        // Off-target emergency, every class/role: answer a deadly enemy cast even
        // if it ISN'T our current target. STUN the silence-immune ones (only a stun
        // stops them, e.g. the Blood Furnace Shadowmoon Technician's Throw Dynamite)
        // and KICK the interruptible ones — both keyed off the curated registries in
        // PartyRotation's DangerousCastList, which grow over time. No-op for a class
        // with no stun/kick (available_* resolves to nothing). Priority sits below
        // emergency heals / survival CDs (95-98) but above normal DPS; rules are
        // priority-sorted so position here doesn't matter.
        add("target_casting:known_dangerous_uninterruptible", "cast_scan:available_stun", 94);
        add("target_casting:known_dangerous",                 "cast_scan:available_interrupt", 93);

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
                    // AoE taunt FIRST when 2+ mobs the tank doesn't hold are loose nearby —
                    // Challenging Shout grabs them all at once instead of single-Taunting one.
                    add("loose_enemies>1", "cast_self:Challenging Shout", 91);
                    add("enemy_loose_in_range", "cast_loose_enemy:Taunt", 90);
                    add("self_health<30", "buff_self:Last Stand", 89);
                    // In-combat rage jump-start so the tank can AoE-threat the pack right away
                    // (esp. now AoE is threat-capped): Bloodrage when rage is low. 1-min CD.
                    add("in_combat&self_rage<25", "cast_self:Bloodrage", 86);
                    // Charge the first mob to OPEN a pull — closes the gap fast and banks rage so
                    // the tank can threat the pull immediately. Out of combat only (Charge), and
                    // only when the mob is far enough to matter. Above Defensive Stance (84) so a
                    // non-Warbringer prot dances into Battle to Charge; once the mob is in melee
                    // this stops firing and the Defensive Stance rule restores the stance. (The
                    // MULTI-pull opener charge is driven from the pull layer — the rotation is
                    // suppressed during a body-pull — so this covers the single-pull opener.)
                    add("out_of_combat&target_dist>11", "charge", 85);
                    add("always", "buff_self:Defensive Stance", 84);
                    add("always", "buff_self:Commanding Shout", 80);
                    // Thunder Clap LEADS the AoE pull: with 3+ in melee it's the
                    // first threat ability (above Shield Slam), hitting the whole
                    // pack at once for immediate snap-aggro — was 70, beneath Shield
                    // Slam, so the tank single-target-slammed first and loose adds
                    // slipped to the casters. Single-target pulls still open on
                    // Shield Slam (Thunder Clap gated enemies_in_melee>2).
                    add("enemies_in_melee>2", "cast:Thunder Clap", 76);
                    add("has_target", "cast:Shield Slam", 74);
                    // Maintain Shield Block for steady mitigation (skips while the
                    // block buff is up; refreshes when it lapses and is off CD).
                    add("has_target", "buff_self:Shield Block", 72);
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
                else   // DPS — Arms(0) / Fury(1), baked per spec.
                {
                    // SHARED by both DPS warrior specs (and the low-level fallback).
                    auto warDpsShared = [&]
                    {
                        // Berserker is the goal stance (enables Pummel + Whirlwind; the core
                        // strikes are stanceless) but it's L30 — until then hold BATTLE, never
                        // Defensive. Keep Berserker high, and a Battle fallback gated
                        // !stance_is_berserker so once Berserker is up this stops firing (no
                        // stance-dance).
                        add("target_casting&target_interruptible", "cast:Pummel", 92);
                        add("target_health<20", "cast:Execute", 90);
                        // Charge to OPEN on a far target (closes the gap + banks rage). Out of
                        // combat only; above the stance rules so it dances into Battle Stance to
                        // Charge, then the Berserker/Battle rules below take the stance back.
                        add("out_of_combat&target_dist>11", "charge", 83);
                        add("always", "buff_self:Berserker Stance", 82);
                        add("!stance_is_berserker", "buff_self:Battle Stance", 81);
                        add("always", "buff_self:Battle Shout", 80);
                        // Bloodrage: free rage in combat (off the GCD; buff_self skips while up).
                        add("in_combat", "buff_self:Bloodrage", 78);
                        // Victory Rush: big free hit, only castable in its post-kill proc window
                        // (engine-gated) — fire it the instant it lights up.
                        add("has_target", "cast:Victory Rush", 74);
                        // Whirlwind LEADS the AoE (hits the whole pack each swing), 3+ in melee.
                        add("enemies_in_melee>2", "cast:Whirlwind", 73);
                        // Cleave (next-swing, hits 2/3 glyphed) is the AoE rage dump, NOT Slam:
                        // Slam as a plain filler drained every spare rage each GCD so Cleave
                        // never fired (Kevin). Gate Slam to single-target so AoE rage funnels
                        // into Cleave; the engine still won't cast either when it can't afford it.
                        add("enemies_in_melee>1", "cast:Cleave", 50);
                        add("enemies_in_melee<2&has_target", "cast:Slam", 40);
                        add("self_rage>55", "cast:Heroic Strike", 30);
                    };

                    if (tree == 0)   // ARMS — Mortal Strike + bleeds + cleave
                    {
                        warDpsShared();
                        // Sweeping Strikes (Arms talent): lead an AoE pull so the next strikes
                        // cleave a second target. No-ops if untalented.
                        add("in_combat&enemies_in_melee>2", "buff_self:Sweeping Strikes", 76);
                        add("has_target", "cast:Mortal Strike", 72);   // signature
                        add("target_missing_aura:Rend", "cast:Rend", 64);
                        add("has_target", "cast:Overpower", 60);       // dodge-proc'd (engine-gated)
                    }
                    else if (tree == 1)   // FURY — Bloodthirst + Whirlwind/Slam (shared)
                    {
                        warDpsShared();
                        add("has_target", "cast:Bloodthirst", 71);   // signature
                        // (Fury skips Rend on purpose — it lives in Berserker stance and
                        // funnels rage into Whirlwind/Slam/Heroic Strike, not a bleed.)
                    }
                    else   // low level / no talents — basic warrior (shared + Rend)
                    {
                        warDpsShared();
                        add("target_missing_aura:Rend", "cast:Rend", 64);
                    }
                }
                break;

            case 2: // Paladin
                // Blessings — maintained by EVERY paladin spec. ONE buff for the
                // whole party: Blessing of Kings on everyone (and on the paladin
                // itself). Out of combat only (don't burn a GCD re-buffing mid-fight)
                // and high priority so a fresh/expired party gets blessed before the
                // pull; cast_party_missing/buff_self fall through once Kings is up, so
                // the priority is free. We deliberately do NOT split Might onto the
                // physical classes: a single paladin's blessings are mutually
                // exclusive on a target, so mixing Might + Kings made the pala
                // re-shuffle the tank between the two every tick.
                add("party_out_of_combat", "cast_party_missing:Blessing of Kings", 85);
                add("out_of_combat", "buff_self:Blessing of Kings", 85);
                if (isHealer)
                {
                    // Holy paladin — SINGLE-TARGET healer (no group AoE heal in 3.3.5a).
                    // Same tiered structure as the priest: buffs/rez OOC, defensive CDs,
                    // then heals by role (strong-CD -> fast -> efficient), proactive tank
                    // absorbs, interrupt, cleanse, filler. Best-first within a tier;
                    // unlearned spells fall through. Eat/Drink auto-append below — every
                    // priority here stays >14 so they sink to the bottom.

                    // BUFFS — self auras/seal (persistent; skip if already up, so the
                    // high priority never costs a heal). Party-wide Blessing of Kings
                    // is handled by the shared paladin blessing block above.
                    add("!is_mounted", "buff_self:Devotion Aura", 98);
                    add("is_mounted",  "buff_self:Crusader Aura", 97);  // mount-speed aura (rotation else suppressed mounted)
                    add("always", "buff_self:Seal of Wisdom", 96);

                    // RESURRECTION — out of combat.
                    add("party_has_dead&party_out_of_combat", "rez_party:Redemption", 95);

                    // DEFENSIVE COOLDOWNS.
                    add("party_lowest_health<25", "cast_party_lowest:Lay on Hands", 92);  // full-heal panic
                    add("self_health<40", "buff_self:Divine Protection", 91);            // self damage cut

                    // (No group AoE heal exists for a 3.3.5a paladin.)

                    // STRONG HEAL — Holy Shock, instant, on CD, for <70.
                    add("party_lowest_health<70", "cast_party_lowest:Holy Shock", 78);   // Holy talent
                    // FAST HEAL — short cast, for <50.
                    add("party_lowest_health<50", "cast_party_lowest:Flash of Light", 76);
                    // EFFICIENT SLOW HEAL — for <70.
                    add("party_lowest_health<70", "cast_party_lowest:Holy Light", 74);

                    // PROACTIVE — keep Beacon mirroring + Sacred Shield absorb on the tank
                    // (Holy talents; fall through if unspecced).
                    add("always", "cast_role_missing:tank:Beacon of Light", 68);
                    add("tank_health<90", "cast_role_missing:tank:Sacred Shield", 66);

                    // INTERRUPT a known-dangerous cast — Hammer of Justice stun (paladins
                    // have no kick; available_stun resolves to HoJ, no-ops if unknown).
                    add("target_casting:known_dangerous", "cast_scan:available_stun", 60);

                    // MANA upkeep.
                    add("self_mana<30", "buff_self:Divine Plea", 56);

                    // CLEANSE (one spell removes magic / poison / disease).
                    add("party_has_magic",   "cure_party:Cleanse", 52);
                    add("party_has_poison",  "cure_party:Cleanse", 51);
                    add("party_has_disease", "cure_party:Cleanse", 50);

                    // FILLER — only at near-full mana (conserve for healing). Judgement
                    // of Light is the reliable ranged filler (Exorcism only lands on
                    // Undead/Demon, so it's omitted — it would no-op on most mobs).
                    add("self_mana>90&has_target", "cast:Judgement of Light", 40);
                }
                else if (isTank)
                {
                    // Panic full self-heal before anything else (shares Forbearance
                    // with Divine Protection below, so the <15 gate keeps it as the
                    // true last resort once the 50% DR can't save us).
                    add("self_health<15", "cast_self:Lay on Hands", 95);
                    // Righteous Defense FIRST when 2+ mobs are loose: it's cast on the ALLY
                    // being beaten on and taunts up to 3 of their attackers off them at once —
                    // the paladin's multi-taunt. Single loose mob -> Hand of Reckoning on it.
                    add("loose_enemies>1", "cast_defend_ally:Righteous Defense", 92);
                    add("enemy_loose_in_range", "cast_loose_enemy:Hand of Reckoning", 91);
                    // Paladins have no true interrupt in 3.3.5a — stun the caster
                    // with Hammer of Justice (no-op on stun-immune bosses).
                    add("target_casting&target_interruptible", "cast:Hammer of Justice", 91);
                    // 50% damage reduction when in danger (off the GCD in-game; the
                    // rotation just spends one tick popping it). Forbearance.
                    add("self_health<40", "buff_self:Divine Protection", 89);
                    add("party_lowest_health<35", "cast_party_lowest:Flash of Light", 86);
                    add("always", "buff_self:Righteous Fury", 82);
                    // Crusader Aura (mount speed) while mounted; Devotion on foot (mutually
                    // exclusive via !is_mounted so they don't fight).
                    add("is_mounted",  "buff_self:Crusader Aura", 81);
                    add("!is_mounted", "buff_self:Devotion Aura", 80);
                    add("always", "buff_self:Seal of Righteousness", 78);
                    // Holy Shield: block-chance + holy-damage shield. Core prot
                    // ability that was missing — both steady mitigation AND threat.
                    add("has_target", "buff_self:Holy Shield", 76);
                    // Consecration LEADS the AoE pull: dropped at the paladin's feet
                    // for immediate threat across the whole pack, ahead of the
                    // single-target threat abilities below (was 66, beneath Avenger's
                    // Shield — so it landed several GCDs in and loose adds slipped to
                    // the casters).
                    add("enemies_in_melee>2", "cast:Consecration", 75);
                    add("party_has_magic", "cure_party:Cleanse", 74);
                    add("has_target", "cast:Avenger's Shield", 70);
                    // Shield of Righteousness (lvl 75): big single-target threat,
                    // scales with block value — was missing entirely. Falls through
                    // harmlessly below 75 / with no shield equipped.
                    add("has_target", "cast:Shield of Righteousness", 63);
                    add("has_target", "cast:Hammer of the Righteous", 60);
                    add("enemies_in_melee>2", "cast:Holy Wrath", 56);
                    // Prefer Judgement of Wisdom (returns mana to the pack's attackers)
                    // when trained; fall through to Judgement of Light if it isn't known.
                    add("has_target", "cast:Judgement of Wisdom", 49);
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
                    // Crusader Aura (mount speed) while mounted; Retribution Aura on foot
                    // (mutually exclusive via !is_mounted so they don't fight).
                    add("is_mounted",  "buff_self:Crusader Aura", 81);
                    add("!is_mounted", "buff_self:Retribution Aura", 80);
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

            case 3: // Hunter — Beast Mastery(0) / Marksmanship(1) / Survival(2), baked per spec.
            {
                // SHARED by every hunter spec (and the low-level fallback).
                auto hunterShared = [&]
                {
                    // Pet summon/revive only OUT OF COMBAT: both fail in combat and, left
                    // ungated, fire every tick — holding the hunter while it re-summons.
                    add("out_of_combat&pet_missing", "cast_self:Call Pet", 90);
                    add("out_of_combat&pet_dead", "cast_self:Revive Pet", 88);
                    add("target_casting&target_interruptible", "cast:Silencing Shot", 87);
                    add("target_health<20", "cast:Kill Shot", 86);
                    add("pet_health<50", "cast_pet:Mend Pet", 78);
                    // Aspect management. IN COMBAT keep the damage aspect (Hawk) up; OUT OF COMBAT
                    // run Aspect of the Pack for party travel speed (deliberately NOT Cheetah — same
                    // daze-on-hit, but Pack buffs the whole group). Both use buff_self, which only
                    // casts when the aspect is actually missing. Low-mana handling still drops to
                    // Aspect of the Viper and swaps back once Viper regen's past 30%; the Hawk rule
                    // stays gated on NOT being in Viper so it can't instantly overwrite the mana
                    // aspect (during Viper, Hawk IS missing) — the 10%-down / 30%-up hysteresis holds.
                    add("self_mana<10&self_missing_aura:Aspect of the Viper", "cast_self:Aspect of the Viper", 75);
                    add("self_mana>30&self_has_aura:Aspect of the Viper", "cast_self:Aspect of the Hawk", 75);
                    add("party_in_combat&self_missing_aura:Aspect of the Viper", "buff_self:Aspect of the Hawk", 74);
                    add("party_out_of_combat&self_missing_aura:Aspect of the Pack", "buff_self:Aspect of the Pack", 74);
                    add("target_missing_aura:Hunter's Mark", "cast:Hunter's Mark", 70);
                    add("target_missing_aura:Serpent Sting", "cast:Serpent Sting", 66);
                    // AoE on the densest CLUSTER (a ranged hunter stands back). Volley
                    // (placed ground AoE) leads; Multi-Shot is the instant fallback.
                    add("enemies_clustered:8>2", "cast:Volley", 54);
                    add("enemies_clustered:8>2", "cast:Multi-Shot", 52);
                    // Forced into melee (no tank peeled the mob): a hunter can't shoot in
                    // the dead zone, so weave the strikes. Gate on target_attacking_me (we
                    // have AGGRO) + a mob in melee range — else, with a far ranged victim,
                    // Raptor Strike fails out-of-range and the cast path walks us INTO
                    // melee, then back out, forever.
                    add("enemies_in_melee>0&target_attacking_me", "cast:Raptor Strike", 51);
                    add("enemies_in_melee>0&target_attacking_me", "cast:Mongoose Bite", 50);
                    add("has_target", "cast:Arcane Shot", 46);
                    add("has_target", "cast:Steady Shot", 36);   // signature-shot filler (all specs)
                };

                if (tree == 0)   // BEAST MASTERY — pet burst
                {
                    hunterShared();
                    add("has_target", "buff_self:Bestial Wrath", 60);   // damage CD (off the GCD)
                    add("has_target", "cast:Kill Command", 58);         // signature
                }
                else if (tree == 1)   // MARKSMANSHIP — aimed/chimera shots
                {
                    hunterShared();
                    add("has_target", "cast:Chimera Shot", 62);   // signature (refreshes Serpent Sting)
                    add("has_target", "cast:Aimed Shot", 56);
                }
                else if (tree == 2)   // SURVIVAL — explosive/black arrow
                {
                    hunterShared();
                    add("target_missing_aura:Black Arrow", "cast:Black Arrow", 62);   // signature DoT (talent)
                    add("has_target", "cast:Explosive Shot", 60);                     // signature (Lock and Load)
                }
                else   // low level / no talents — basic hunter (shared shots cover it)
                {
                    hunterShared();
                }
                break;
            }

            case 4: // Rogue — Assassination(0) / Combat(1) / Subtlety(2), baked per spec.
            {
                // SHARED by every rogue spec: utility, finishers, AoE. The combo
                // BUILDER differs per spec (added in the branch).
                auto rogueShared = [&]
                {
                    add("target_casting&target_interruptible", "cast:Kick", 92);
                    add("out_of_combat&self_missing_aura:Stealth", "cast_self:Stealth", 80);
                    // Sprint to run down a distant target (no rogue charge exists). Only when the
                    // gap is real (>18y); the melee abilities below are out of range until then, so
                    // they fall through to this. Doesn't break Stealth.
                    add("target_dist>18", "sprint", 60);
                    // Execute finisher: a dying target gets Eviscerated NOW with as few as
                    // 3 combo rather than waiting for 5 — above Slice and Dice so we don't
                    // refresh a buff on a corpse.
                    add("target_health<20&self_combo>2", "cast:Eviscerate", 78);
                    // Slice and Dice: keep it up at all times (the haste buff is worth more
                    // than the combo). Needs 3+ combo so it isn't refreshed off a single point.
                    add("self_missing_aura:Slice and Dice&self_combo>2", "cast:Slice and Dice", 76);
                    // Rupture only if the bleed has time to pay off (target_ttd>8 = a long-
                    // lived/boss mob; FALSE for trash about to die).
                    add("self_combo>4&target_missing_aura:Rupture&target_ttd>8", "cast:Rupture", 70);
                    add("self_combo>4", "cast:Eviscerate", 66);
                    // Killing Spree (Combat 51-pt talent): burst cleave CD that leaps between
                    // nearby enemies. Same AoE trigger as Fan of Knives and sits right above it; a
                    // 2-min cooldown so it self-throttles (falls through while on CD, and is unknown
                    // -> skipped for Assassination/Subtlety), leaving the high slot free (Mill).
                    add("enemies_in_melee>2", "cast:Killing Spree", 59);
                    add("enemies_in_melee>2", "cast:Fan of Knives", 58);   // AoE
                    add("has_target", "cast:Sinister Strike", 40);          // universal builder fallback
                };

                if (tree == 0)   // ASSASSINATION — Mutilate builder, Envenom finisher
                {
                    rogueShared();
                    add("self_combo>4", "cast:Envenom", 67);   // poison finisher (above the Eviscerate fallback)
                    add("has_target", "cast:Mutilate", 46);    // signature builder (needs daggers)
                }
                else if (tree == 1)   // COMBAT — Sinister Strike + Blade Flurry cleave
                {
                    rogueShared();
                    // Blade Flurry: cleave CD whenever 3+ are in melee. Sits JUST BELOW
                    // Stealth (80 -> 79, Kevin) so it leads the in-combat rotation; buff_self
                    // falls through while active / on cooldown, so the high priority is free.
                    add("enemies_in_melee>2", "buff_self:Blade Flurry", 79);
                }
                else if (tree == 2)   // SUBTLETY — Hemorrhage builder
                {
                    rogueShared();
                    add("has_target", "cast:Hemorrhage", 44);   // signature builder (above Sinister Strike)
                }
                else   // low level / no talents — basic rogue (Sinister Strike + finishers)
                {
                    rogueShared();
                }
                break;
            }

            case 5: // Priest
                if (isHealer)
                {
                    // Holy & Discipline healing, in strict priority tiers (Kevin's
                    // tuning). Spells are listed BEST-FIRST and the engine falls through
                    // to the next rule when a spell isn't KNOWN (cast_party_lowest returns
                    // false on FindKnownSpellByName==0) — so "Heal" only fires when Greater
                    // Heal is unlearned, "Lesser Heal" only when Flash Heal is, etc., and a
                    // sub-20 priest with only Lesser Heal/Heal still heals. Disc vs Holy
                    // each use whichever spec spells they own; talented CDs fall through
                    // cleanly when unspecced. Eat / Drink / Wand are NOT listed here — the
                    // shared sustain+filler block below auto-appends them (eat/drink prio
                    // ~12-14, wand prio 11). EVERY rule here therefore stays >14 so those
                    // common rules sink to the bottom; a low number would let the appended
                    // wand outrank Renew/interrupt/cleanse.

                    // BUFFS — out of combat only, so they never interrupt healing.
                    add("party_out_of_combat", "cast_party_missing:Power Word: Fortitude", 99);
                    add("party_out_of_combat", "cast_party_missing:Divine Spirit", 98);   // Disc; falls through if unknown
                    add("party_out_of_combat", "buff_self:Inner Fire", 97);

                    // RESURRECTION — out of combat.
                    add("party_has_dead&party_out_of_combat", "rez_party:Resurrection", 95);

                    // DEFENSIVE COOLDOWNS on a critically-low member (talented; fall through).
                    add("party_lowest_health<30", "cast_party_lowest:Guardian Spirit", 92);  // Holy
                    add("party_lowest_health<30", "cast_party_lowest:Pain Suppression", 91); // Disc

                    // AoE HEALS — only when 3+ members are hurt near each other AND one
                    // is meaningfully low. (Editor labels the cluster condition "Enemies
                    // 30y" but it counts INJURED ALLIES — display quirk, not the gate.)
                    add("party_injured_clustered:30>2&party_lowest_health<80", "cast_party_lowest:Prayer of Healing", 85);
                    add("party_injured_clustered:30>2&party_lowest_health<80", "cast_party_lowest:Circle of Healing", 84); // Holy talent, instant

                    // SINGLE-TARGET HEALS — Penance -> Flash Heal -> Binding Heal ->
                    // Greater Heal -> Heal -> Lesser Heal. PW:Shield keeps the top
                    // priority but is gated hard (<40%): the absorb is mana-inefficient,
                    // so it's a PANIC instant-absorb on a badly-hurt member — thrown
                    // first when someone spikes low — NOT a preventative bubble slapped
                    // on anyone who dips below 80%. (Harmlessly skips a Weakened Soul
                    // target and falls through to the heals below.)
                    add("party_lowest_health<40", "cast_party_lowest:Power Word: Shield", 78);
                    add("party_lowest_health<60", "cast_party_lowest:Penance", 77);
                    add("party_lowest_health<50", "cast_party_lowest:Flash Heal", 76);
                    add("party_lowest_health<70", "cast_party_lowest:Binding Heal", 75);  // heals the priest too
                    add("party_lowest_health<70", "cast_party_lowest:Greater Heal", 74);
                    add("party_lowest_health<70", "cast_party_lowest:Heal", 73);          // if no Greater Heal
                    add("party_lowest_health<70", "cast_party_lowest:Lesser Heal", 72);   // if no Flash Heal

                    // RENEW — keep a HoT rolling on the lowest member missing it.
                    add("party_lowest_health<80", "cast_party_lowest_hot:Renew", 66);

                    // INTERRUPT a known-dangerous cast (priest: Silence; no-ops if unknown).
                    add("target_casting:known_dangerous", "cast_scan:available_interrupt", 60);

                    // CLEANSE party members.
                    add("party_has_disease", "cure_party:Abolish Disease", 52);  // if no Abolish, falls to Cure
                    add("party_has_disease", "cure_party:Cure Disease", 51);
                    add("party_has_magic", "cure_party:Dispel Magic", 50);

                    // FILLER DPS — only at near-full mana (conserve for healing). Wand
                    // (the shared filler below, prio 11) is the no-mana fallback under these.
                    add("self_mana>90&target_missing_aura:Shadow Word: Pain", "cast:Shadow Word: Pain", 40);
                    add("self_mana>90&has_target", "cast:Smite", 38);
                }
                else
                {
                    add("party_lowest_health<30", "cast_party_lowest:Flash Heal", 86);
                    add("target_casting&target_interruptible", "cast:Silence", 80);
                    add("always", "buff_self:Shadowform", 76);
                    add("always", "cast_party_missing:Power Word: Fortitude", 56);
                    // SW:P is a SPREAD dot — keep the whole pull dotted. in_combat (not
                    // has_target) so it fires with no specific victim, like warlock Corruption.
                    add("in_combat", "cast_spread:Shadow Word: Pain", 70);
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
                    // Death Grip range-PULLS a loose mob to the tank AND taunts it
                    // (35s CD) — the preferred way to grab a caster/runner off the
                    // party. Dark Command is the short-CD ranged taunt backup for
                    // when Death Grip is down or the mob is already in melee.
                    add("enemy_loose_in_range&enemies_in_melee>1", "cast_loose_enemy:Death Grip", 91);
                    add("enemy_loose_in_range", "cast_loose_enemy:Dark Command", 90);
                    // Icebound Fortitude = damage reduction + stun immunity panic
                    // (baseline). Vampiric Blood = +max HP & +healing taken (Blood
                    // talent; falls through if unspecced). Both off the GCD.
                    add("self_health<35", "buff_self:Icebound Fortitude", 89);
                    add("self_health<45", "buff_self:Vampiric Blood", 88);
                    // Mind Freeze is the melee kick; Strangulate the ranged silence
                    // for a caster the tank can't reach (falls through when it's on
                    // its long CD, or unlearned).
                    add("target_casting&target_interruptible", "cast:Mind Freeze", 86);
                    add("target_casting&target_interruptible", "cast:Strangulate", 85);
                    add("self_health<55", "cast:Death Strike", 80);
                    add("self_health<40", "cast_self:Rune Tap", 78);
                    // Frost Presence is the TANK presence (+HP, +threat, -damage).
                    add("always", "buff_self:Frost Presence", 74);
                    // Horn of Winter: self-cast, buffs the whole nearby party's AP
                    // (and tops up runic power); maintained when the buff drops.
                    add("always", "buff_self:Horn of Winter", 72);
                    add("target_missing_aura:Frost Fever", "cast:Icy Touch", 70);
                    add("target_missing_aura:Blood Plague", "cast:Plague Strike", 69);
                    // Mark of Blood on bosses: their melee heals whoever they hit
                    // (self-sustain for the tank). Blood talent — skips if unspecced.
                    add("target_is_boss&target_missing_aura:Mark of Blood", "cast:Mark of Blood", 68);
                    add("enemies_in_melee>2", "cast:Death and Decay", 64);
                    // Pestilence spreads the tank's diseases to the pack — only worth
                    // a rune while a nearby engaged mob still lacks one, so gate it on
                    // that instead of re-casting every tick.
                    add("enemies_in_melee>2&enemy_needs_aura:Frost Fever,Blood Plague", "cast:Pestilence", 58);
                    // Rune Strike is a NEXT-SWING runic-power dump — arm it once per
                    // swing (not spammed), above the rune strikes as requested.
                    add("has_target", "cast_on_swing:Rune Strike", 54);
                    add("has_target", "cast:Heart Strike", 52);
                    add("has_target", "cast:Blood Strike", 48);
                    add("has_target", "cast:Death Coil", 38);
                }
                else   // DPS — Blood(0) / Frost(1) / Unholy(2), baked per spec.
                {
                    // SHARED by every DPS DK spec (and the low-level fallback): interrupt,
                    // self-heal strike, the two diseases, AoE, rune builder + RP dump.
                    auto dkDpsShared = [&]
                    {
                        add("target_casting&target_interruptible", "cast:Mind Freeze", 86);
                        add("self_health<50", "cast:Death Strike", 80);   // self-heal strike
                        add("always", "buff_self:Horn of Winter", 72);
                        add("target_missing_aura:Frost Fever", "cast:Icy Touch", 70);
                        add("target_missing_aura:Blood Plague", "cast:Plague Strike", 69);
                        add("enemies_in_melee>2", "cast:Death and Decay", 64);   // ground AoE
                        add("enemies_in_melee>2&enemy_needs_aura:Frost Fever,Blood Plague", "cast:Pestilence", 63);        // spread diseases
                        add("has_target", "cast:Blood Strike", 46);   // rune builder
                        add("has_target", "cast:Death Coil", 40);     // runic-power dump
                    };

                    if (tree == 1)   // FROST — Obliterate + Frost Strike
                    {
                        dkDpsShared();
                        add("always", "buff_self:Blood Presence", 74);   // the DPS presence (+damage)
                        add("enemies_in_melee>2", "cast:Howling Blast", 56);   // frost AoE (talent)
                        add("has_target", "cast:Obliterate", 54);              // signature
                        add("has_target", "cast:Frost Strike", 42);            // RP dump (above Death Coil)
                    }
                    else if (tree == 2)   // UNHOLY — Scourge Strike + Death Coil
                    {
                        dkDpsShared();
                        add("always", "buff_self:Blood Presence", 74);   // the DPS presence (+damage)
                        add("has_target", "cast:Scourge Strike", 54);     // signature
                    }
                    else   // BLOOD dps(0) — Heart Strike; also the low-level fallback.
                    {
                        dkDpsShared();
                        add("always", "buff_self:Blood Presence", 74);   // +damage
                        add("has_target", "cast:Heart Strike", 52);      // signature
                    }
                }
                break;

            case 7: // Shaman
                if (isHealer)
                {
                    // Resto shaman — same tiered structure as the priest. Best-first
                    // within a tier; unlearned spells fall through at every level.
                    // Eat/Drink auto-append below — every priority here stays >14.

                    // BUFFS — weapon imbue + mana/dmg shield (OOC), buff totem SET anytime.
                    add("out_of_combat", "buff_self:Earthliving Weapon", 99);   // skip-if-imbued, so high prio is free
                    add("party_has_tank",  "buff_self:Water Shield", 98);       // mana sustain behind a tank
                    add("!party_has_tank", "buff_self:Lightning Shield", 97);   // dmg shield with no tank
                    // Buff totem SET in one off-GCD tick (Call-of-the-Elements emulation):
                    // earth/water/air at once whenever any is gone or out-run, incl. during
                    // a multi-pull so the aura follows a party that moved up.
                    add("totem_set_stale:Stoneskin Totem,Mana Spring Totem,Wrath of Air Totem",
                        "cast_totem_set:Stoneskin Totem,Mana Spring Totem,Wrath of Air Totem", 96);

                    // RESURRECTION — out of combat.
                    add("party_has_dead&party_out_of_combat", "rez_party:Ancestral Spirit", 95);

                    // DEFENSIVE / EMERGENCY COOLDOWN — Nature's Swiftness makes the next
                    // heal instant (talent); the big Healing Wave below then lands instantly.
                    add("party_lowest_health<30", "buff_self:Nature's Swiftness", 92);

                    // AoE HEAL — Chain Heal when 3+ are hurt near each other (above singles).
                    add("party_injured_clustered:30>2&party_lowest_health<80", "cast_party_lowest:Chain Heal", 85); // jumps between them (L40)

                    // STRONG HEAL — Riptide, instant HoT+heal, on CD, for <70.
                    add("party_lowest_health<70", "cast_party_lowest:Riptide", 78);
                    // FAST HEAL — short cast, for <50.
                    add("party_lowest_health<50", "cast_party_lowest:Lesser Healing Wave", 76);
                    // EFFICIENT SLOW HEAL — for <70.
                    add("party_lowest_health<70", "cast_party_lowest:Healing Wave", 74);

                    // HoT — keep Riptide rolling on the lowest <90 missing it.
                    add("party_lowest_health<90", "cast_party_lowest_hot:Riptide", 66);
                    // PROACTIVE — Earth Shield absorb-on-hit on the tank (talent).
                    add("always", "cast_role_missing:tank:Earth Shield", 64);

                    // INTERRUPT a known-dangerous cast — Wind Shear.
                    add("target_casting:known_dangerous", "cast_scan:available_interrupt", 60);

                    // CLEANSE.
                    add("party_has_poison",  "cure_party:Cure Toxins", 52);
                    add("party_has_disease", "cure_party:Cure Toxins", 51);
                    add("party_has_curse",   "cure_party:Cleanse Spirit", 50);

                    // FILLER — only at near-full mana (conserve for healing).
                    add("self_mana>90&target_missing_aura:Flame Shock", "cast:Flame Shock", 40);
                    add("self_mana>90&has_target", "cast:Lightning Bolt", 38);
                }
                else   // DPS — Elemental(0) / Enhancement(1), baked per spec.
                {
                    // SHARED by both DPS shaman specs (and the low-level fallback).
                    // Flame Shock + Earth Shock are NOT shared — enh wants them low in its
                    // damage order (Kevin), ele/low want Flame Shock high — so each branch
                    // adds its own at the priority that spec needs (conditions identical).
                    auto shaDpsShared = [&]
                    {
                        add("party_lowest_health<30", "cast_party_lowest:Healing Wave", 86);
                        add("target_casting&target_interruptible", "cast:Wind Shear", 82);
                        // Totems are added PER SPEC below — the buff SET differs by spec
                        // (elemental gets Totem of Wrath in the fire slot; enh/low keep
                        // Searing as a separate attack totem). The set drops as one off-GCD
                        // action (Call-of-the-Elements emulation), incl. during a multi-pull.
                    };

                    if (tree == 1)   // ENHANCEMENT — melee
                    {
                        shaDpsShared();
                        add("always", "buff_self:Lightning Shield", 78);   // damage-on-hit (in melee)
                        // Dual-imbue (Kevin): Windfury on the MAIN hand for the burst proc,
                        // Flametongue on the OFF hand (+25% Lava Lash). buff_self always
                        // targets the main hand, so it can't place a second imbue — the
                        // slot-pinned verbs do. Both use the normal temp-enchant path.
                        add("out_of_combat", "buff_mainhand:Windfury Weapon", 85);
                        add("out_of_combat", "buff_offhand:Flametongue Weapon", 83);
                        // Rockbiter is the no-Windfury fallback (low level, where there's no
                        // Flametongue to conflict). It STAYS on buff_self: its shaman-family
                        // handler enchants both weapons itself and selects the enchant
                        // dynamically (ignoring MiscValue), so the slot-pinned verbs — which
                        // key off MiscValue — don't apply to it.
                        add("out_of_combat&!spell_ready:Windfury Weapon", "buff_self:Rockbiter Weapon", 84);
                        // Buff totems dropped as a SET in one off-GCD tick (Call-of-the-
                        // Elements emulation): earth/water/air at once, incl. DURING a
                        // multi-pull (party_in_combat) so the shaman doesn't burn a GCD per
                        // totem. Fire is the Searing ATTACK totem, walked to the pack (kept
                        // separate, held during the pull since it pulls threat).
                        add("party_in_combat&totem_attack_needed:fire", "cast_totem_attack:Searing Totem", 54);
                        // Recall Searing the moment the WHOLE party leaves combat — the attack
                        // totem outlives the fight and keeps shooting anything that wanders near,
                        // pulling bystanders while the party drinks.
                        add("party_out_of_combat&self_totem_active:Searing Totem", "recall_totem:Searing Totem", 60);
                        add("party_in_combat&totem_set_stale:Strength of Earth Totem,Mana Spring Totem,Windfury Totem",
                            "cast_totem_set:Strength of Earth Totem,Mana Spring Totem,Windfury Totem", 52);
                        // Damage order (Kevin), high->low below the totems: dump Maelstrom
                        // procs FIRST (CL in a cluster, else LB) — they're instant and expire,
                        // so they must beat the melee strikes — then Stormstrike, Lava Lash,
                        // and finally the shocks. CL/LB stay gated on the 5-stack proc, so a
                        // hard cast never pulls enh to range (the "enh fights at range" bug).
                        add("self_aura_stacks:Maelstrom Weapon>4&enemies_clustered:8>2", "cast:Chain Lightning", 50);
                        add("self_aura_stacks:Maelstrom Weapon>4", "cast:Lightning Bolt", 48);
                        add("has_target", "cast:Stormstrike", 46);
                        add("has_target", "cast:Lava Lash", 44);
                        add("target_missing_aura:Flame Shock", "cast:Flame Shock", 42);
                        add("has_target", "cast:Earth Shock", 40);   // instant dump
                    }
                    else if (tree == 0)   // ELEMENTAL — ranged nuker
                    {
                        shaDpsShared();
                        add("target_missing_aura:Flame Shock", "cast:Flame Shock", 72);
                        // Chain Lightning sits just BELOW Wind Shear and ABOVE Flame Shock
                        // (Kevin): high-priority cluster AoE so it leads the elemental rotation
                        // when a pack is stacked. Still AoE-gated so single-target stays on LB.
                        add("enemies_clustered:8>2", "cast:Chain Lightning", 74);
                        // Water Shield (mana) behind a tank; Lightning Shield when it holds
                        // its own aggro (Kevin: ranged shamans prefer water with a tank).
                        add("party_has_tank",  "buff_self:Water Shield", 78);
                        add("!party_has_tank", "buff_self:Lightning Shield", 77);
                        add("out_of_combat", "buff_self:Flametongue Weapon", 85);
                        // Buff totems dropped as a SET in one off-GCD tick (Call-of-the-
                        // Elements emulation), incl. DURING a multi-pull (party_in_combat).
                        // Fire = Totem of Wrath (a BUFF totem) when learned; air = Wrath of
                        // Air, else Windfury (list order = priority, first LEARNED per element
                        // wins). Searing is only used as the fire totem when Totem of Wrath
                        // ISN'T learned (below).
                        add("party_in_combat&totem_set_stale:Totem of Wrath,Strength of Earth Totem,Mana Spring Totem,Wrath of Air Totem,Windfury Totem",
                            "cast_totem_set:Totem of Wrath,Strength of Earth Totem,Mana Spring Totem,Wrath of Air Totem,Windfury Totem", 52);
                        // Fire FALLBACK: no Totem of Wrath learned -> the fire slot has no
                        // buff totem, so drop Searing at the pack instead. Suppressed while
                        // Totem of Wrath holds the fire slot (it's the better elemental totem).
                        add("party_in_combat&totem_attack_needed:fire&!self_totem_active:Totem of Wrath",
                            "cast_totem_attack:Searing Totem", 50);
                        // Recall the Searing fallback once combat ends (only relevant when
                        // Totem of Wrath isn't learned, so it never touches the ToW buff totem).
                        add("party_out_of_combat&self_totem_active:Searing Totem", "recall_totem:Searing Totem", 60);
                        add("has_target", "cast:Lava Burst", 66);
                        add("has_target", "cast:Earth Shock", 46);                 // instant filler
                        add("has_target", "cast:Lightning Bolt", 38);              // ranged filler
                    }
                    else   // low level / no talents — basic caster shaman
                    {
                        shaDpsShared();
                        add("target_missing_aura:Flame Shock", "cast:Flame Shock", 72);
                        add("has_target", "cast:Earth Shock", 46);
                        add("party_has_tank",  "buff_self:Water Shield", 78);
                        add("!party_has_tank", "buff_self:Lightning Shield", 77);
                        add("out_of_combat", "buff_self:Flametongue Weapon", 85);
                        // Buff totems as a SET (whichever of these are learned); Searing is
                        // the fire attack totem. ResolveTotemSet skips any not-yet-trained.
                        add("party_in_combat&totem_set_stale:Strength of Earth Totem,Mana Spring Totem",
                            "cast_totem_set:Strength of Earth Totem,Mana Spring Totem", 52);
                        add("party_in_combat&totem_attack_needed:fire", "cast_totem_attack:Searing Totem", 54);
                        // Recall Searing after combat so the lingering attack totem stops
                        // pulling bystanders while the party drinks.
                        add("party_out_of_combat&self_totem_active:Searing Totem", "recall_totem:Searing Totem", 60);
                        add("has_target", "cast:Lightning Bolt", 38);
                    }
                }
                break;

            case 8: // Mage — Arcane(0) / Fire(1) / Frost(2), baked per spec.
            {
                // SHARED by every mage spec (and the low-level fallback).
                auto mageShared = [&]
                {
                    add("target_casting&target_interruptible", "cast:Counterspell", 88);
                    // Out of mana mid-fight: channel Evocation to refill — high priority,
                    // since a dry mage contributes nothing anyway. Mage-only (cooldown
                    // enforced by the engine); gated <10% so it fires only when genuinely
                    // empty, never as a filler. Sits just under the interrupt.
                    add("in_combat&self_mana<10", "cast_self:Evocation", 87);
                    // Ice Barrier is a Frost talent — falls through (unknown) for Fire/
                    // Arcane, who have no comparable absorb in their default kit. In combat
                    // only (Kevin): don't burn mana + a GCD shielding while idle out of combat.
                    add("in_combat&self_missing_aura:Ice Barrier", "cast_self:Ice Barrier", 80);
                    // Only when a melee is ACTUALLY swinging at the mage (root-and-run defence),
                    // not merely near it — else the mage roots the tank's body-pull mob in place
                    // far from the tank the instant it walks past (Mill). melee_attackers = mobs
                    // in the mage's attacker set, vs enemies_in_melee = mere proximity.
                    add("melee_attackers>0", "cast_self:Frost Nova", 76);
                    add("always", "cast_party_missing:Arcane Intellect", 60);
                    // Disabled by default: henchmen recover for free (eat/drink below) and
                    // don't need conjured items. Kept (flagged "disabled") so a player
                    // running a mage as an alt-bot can tick it on to stock the shared bags.
                    add("out_of_combat&shared_drink<5", "cast_self:Conjure Water", 18, "disabled");
                    add("out_of_combat&shared_food<5", "cast_self:Conjure Food", 16, "disabled");
                };

                if (tree == 1)   // FIRE
                {
                    mageShared();
                    // Molten Armor (L62, +crit/+spell-hit) is the fire/arcane armor; Frost
                    // Armor only while Molten isn't trained yet (!spell_ready == not known,
                    // armor has no cooldown), so it never overwrites an active Molten Armor.
                    add("always", "buff_self:Molten Armor", 72);
                    add("!spell_ready:Molten Armor", "buff_self:Frost Armor", 71);
                    // Hot Streak (two crits in a row) makes the next Pyroblast INSTANT &
                    // free — fire it the instant the proc is up (before the 10s buff lapses).
                    // Gated on the proc aura so the 5s hard-cast Pyroblast never clogs the
                    // rotation; no proc → no Pyroblast, fall through to Fireball.
                    add("self_has_aura:Hot Streak", "cast:Pyroblast", 69);
                    add("target_missing_aura:Living Bomb", "cast:Living Bomb", 68);
                    // Ground AoE on the densest CLUSTER (a ranged mage stands back, so a
                    // bot-centred melee count reads 0 on a pack it could nuke).
                    add("enemies_clustered:8>2", "cast:Flamestrike", 60);
                    add("has_target", "cast:Fireball", 46);   // primary nuke
                    add("has_target", "cast:Scorch", 40);     // instant-ish filler / on the move
                    add("has_target", "cast:Fire Blast", 38); // instant, off cooldown
                }
                else if (tree == 2)   // FROST
                {
                    mageShared();
                    add("always", "buff_self:Ice Armor", 72);  // Frost's own armor (L34 upgrade of Frost Armor)
                    add("!spell_ready:Ice Armor", "buff_self:Frost Armor", 71);
                    // Brain Freeze (procs off Frostbolt / Frostfire Bolt) makes the next Fireball
                    // or Frostfire Bolt INSTANT & free — fire it the instant the proc is up, before
                    // the buff lapses (mirrors Fire's Hot Streak -> Pyroblast). The proc BUFF shows
                    // as "Fireball!" (spell 57761); the aura literally named "Brain Freeze" is the
                    // passive TALENT, which FindNamedAura skips — so match "Fireball!" here.
                    // Frostfire Bolt (L75, scales with frost+fire) is the preferred dump; a sub-75
                    // deep-frost mage lacks it and falls through to the Fireball line.
                    add("self_has_aura:Fireball!", "cast:Frostfire Bolt", 69);
                    add("self_has_aura:Fireball!", "cast:Fireball", 68);
                    add("enemies_clustered:8>2", "cast:Blizzard", 60);  // frost ground AoE
                    add("has_target", "cast:Frostbolt", 46);   // primary nuke
                    add("has_target", "cast:Ice Lance", 40);   // instant filler (Fingers of Frost / movement)
                }
                else if (tree == 0)   // ARCANE
                {
                    mageShared();
                    add("always", "buff_self:Molten Armor", 72);
                    add("!spell_ready:Molten Armor", "buff_self:Frost Armor", 71);
                    // Arcane has no ranged AoE of its own, so it borrows Blizzard (Kevin)
                    // — high priority so it LEADS in AoE situations from range. Arcane
                    // Explosion (PBAoE) only when mobs are ACTUALLY in melee on the mage.
                    add("enemies_clustered:8>2", "cast:Blizzard", 60);
                    add("enemies_in_melee>2", "cast:Arcane Explosion", 58);
                    add("has_target", "cast:Arcane Blast", 46);    // primary nuke (ramps)
                    add("has_target", "cast:Arcane Missiles", 44); // Missile Barrage proc / filler
                    add("has_target", "cast:Arcane Barrage", 40);  // instant dump / movement (L80)
                }
                else   // tree < 0 — low level / no talents: basic mage.
                {
                    mageShared();
                    add("always", "buff_self:Frost Armor", 72);
                    add("has_target", "cast:Frostbolt", 46);
                    add("has_target", "cast:Fireball", 44);
                    add("has_target", "cast:Fire Blast", 40);
                }
                break;
            }

            case 9: // Warlock — Affliction(0) / Demonology(1) / Destruction(2), baked per spec.
            {
                // SHARED by every warlock spec (and the low-level fallback).
                auto wlShared = [&]
                {
                    add("pet_missing", "cast_self:Summon Imp", 88);
                    add("self_health<35", "cast:Death Coil", 82);
                    // Fel Armor (L62, +spell power & healing taken) is the caster armor;
                    // Demon Armor only while Fel Armor isn't trained yet (!spell_ready ==
                    // not known; armor has no cooldown, so it never overwrites Fel Armor).
                    add("always", "buff_self:Fel Armor", 76);
                    add("!spell_ready:Fel Armor", "buff_self:Demon Armor", 75);
                    // AoE on the densest mob CLUSTER (a ranged warlock stands well back) —
                    // above Curse of Agony so a pack gets Rain of Fire before the single-
                    // target curse (Kevin). Still below Corruption (the spread dot, 70).
                    add("enemies_clustered:8>2", "cast:Rain of Fire", 68);   // placed ground AoE
                    // Curse of Agony RAMPS — most of its damage lands in the back third of
                    // its 24 s, so it's wasted on a mob that dies first. Apply only when the
                    // target will live long enough to reach that payoff (target_ttd>20), but
                    // ALWAYS on a boss: TTD reads 0 for the first ~5 s of any fight, so the
                    // boss override is what gets it up at the pull instead of stalling.
                    add("target_missing_aura:Curse of Agony&target_is_boss", "cast:Curse of Agony", 66);
                    add("target_missing_aura:Curse of Agony&target_ttd>20", "cast:Curse of Agony", 66);
                    add("has_target", "cast:Shadow Bolt", 42);               // universal filler
                };

                if (tree == 0)   // AFFLICTION — DoTs + drain
                {
                    // Felhunter (Shadow Bite scales with the lock's DoTs) is the
                    // Affliction pet; falls through to the Imp in wlShared if the lock
                    // isn't deep enough in the tree to know it yet.
                    add("pet_missing", "cast_self:Summon Felhunter", 89);
                    wlShared();
                    // Keep Corruption rolling on the MAIN target — Affliction's Eradication
                    // procs off Corruption TICKS, and in an AoE pull (Seed spam below) it
                    // would otherwise fall off. Gated target_missing_aura (NOT a bare
                    // has_target): re-casting every GCD would clip the DoT for a net loss
                    // AND starve Seed of Corruption (has_target is always true in combat).
                    // This is the "extra Corruption that supersedes Seed" — sits ABOVE it.
                    add("target_missing_aura:Corruption", "cast:Corruption", 71);
                    // AoE workhorse: Seed every mob in a big pull (>3 clustered). cast_spread
                    // seeds each undebuffed engaged mob so the detonations chain across the
                    // pack. Just UNDER the main-target Corruption (71) so Corruption keeps
                    // Eradication proccing, and ABOVE the pack Corruption-spread (69).
                    add("enemies_clustered:8>3", "cast_spread:Seed of Corruption", 70);
                    // Spread Corruption across the REST of the pack as AoE filler — gated
                    // in_combat (a spread dot needs no specific victim; has_target was false
                    // with no target so it fell straight to Rain of Fire and never dotted).
                    add("in_combat", "cast_spread:Corruption", 69);
                    // Unstable Affliction also ramps over its duration — wasted on a dying
                    // add. Apply only when the target lives long enough (target_ttd>8 ≈ cast
                    // + a few ticks), but ALWAYS on a boss (TTD is 0 for the pull's first
                    // ~5 s, so the boss override lands it immediately instead of stalling).
                    add("target_missing_aura:Unstable Affliction&target_is_boss", "cast:Unstable Affliction", 62);
                    add("target_missing_aura:Unstable Affliction&target_ttd>8", "cast:Unstable Affliction", 62);
                    add("target_health<25", "cast:Drain Soul", 54);   // execute drain
                    add("has_target", "cast:Haunt", 50);
                }
                else if (tree == 1)   // DEMONOLOGY — Felguard + Immolate/Corruption + Shadow Bolt
                {
                    // Signature FELGUARD above the Imp; falls through to the Imp (in
                    // wlShared) if not deep enough in Demo to know it.
                    add("pet_missing", "cast_self:Summon Felguard", 89);
                    wlShared();
                    add("target_missing_aura:Immolate", "cast:Immolate", 72);
                    add("in_combat", "cast_spread:Corruption", 70);
                }
                else if (tree == 2)   // DESTRUCTION — direct fire
                {
                    wlShared();
                    add("target_missing_aura:Immolate", "cast:Immolate", 72);   // core, keep up
                    add("has_target", "cast:Conflagrate", 68);   // consumes Immolate (talent)
                    add("has_target", "cast:Chaos Bolt", 60);    // talent nuke (L80)
                    add("has_target", "cast:Incinerate", 44);    // primary filler (above Shadow Bolt)
                }
                else   // low level / no talents — basic warlock
                {
                    wlShared();
                    add("has_target", "cast:Corruption", 70);
                }
                break;
            }

            case 11: // Druid
                // Innervate the party's healer the moment their mana bottoms out — applies to
                // EVERY druid spec (a resto healer innervates itself; a moonkin / feral DPS
                // innervates whoever heals). cast_role_missing:healer only (re)casts on a healer
                // who isn't already innervated, and healer_mana<20 is the trigger — so it fires
                // once when the healer drops under 20% and not again until it wears off. Excluded
                // in Bear form so a bear TANK never leaves the pack to walk over and cast it
                // (Innervate has NO stance restriction, so castOrApproach WOULD path a bear to the
                // healer); a cat / moonkin / tree / caster druid casts it fine.
                add("healer_mana<20&!form_is_bear", "cast_role_missing:healer:Innervate", 88);
                if (isHealer)
                {
                    // Resto druid — HoT-centric, same tiered structure as the priest.
                    // Best-first within a tier; unlearned spells fall through at every
                    // level. Eat/Drink auto-append below — every priority here stays >14.

                    // BUFFS — out of combat.
                    add("party_out_of_combat", "cast_party_missing:Mark of the Wild", 99);

                    // RESURRECTION — out of combat.
                    add("party_has_dead&party_out_of_combat", "rez_party:Revive", 95);

                    // DEFENSIVE COOLDOWNS.
                    add("party_lowest_health<30", "buff_self:Nature's Swiftness", 92);  // next heal instant (talent)
                    add("self_health<40", "buff_self:Barkskin", 91);                    // self damage cut

                    // AoE HEALS — when 3+ are hurt near each other (above single heals).
                    add("party_injured_clustered:30>2&party_lowest_health<80", "cast_party_lowest:Wild Growth", 85); // instant AoE HoT (talent)
                    add("party_injured_clustered:30>2&party_lowest_health<80", "cast_self:Tranquility", 84);         // AoE channel (L30, big CD)

                    // STRONG HEAL — Swiftmend, instant (consumes a HoT), on CD, for <70.
                    add("party_lowest_health<70", "cast_party_lowest:Swiftmend", 78);  // talent
                    // FAST HEAL — short cast, for <50: Nourish (L80) -> Regrowth (L12).
                    add("party_lowest_health<50", "cast_party_lowest:Nourish", 76);
                    add("party_lowest_health<50", "cast_party_lowest:Regrowth", 75);
                    // EFFICIENT SLOW HEAL — for <70.
                    add("party_lowest_health<70", "cast_party_lowest:Healing Touch", 74);

                    // HoTs — low priority, kept rolling: Rejuv <90, Regrowth-HoT <80,
                    // Lifebloom maintained on the tank.
                    add("party_lowest_health<90", "cast_party_lowest_hot:Rejuvenation", 66);
                    add("party_lowest_health<80", "cast_party_lowest_hot:Regrowth", 64);
                    add("always", "cast_role_missing:tank:Lifebloom", 62);  // stacking HoT on the tank

                    // (No caster-form interrupt for a resto druid.)

                    // CLEANSE.
                    add("party_has_curse",  "cure_party:Remove Curse", 52);
                    add("party_has_poison", "cure_party:Abolish Poison", 51);

                    // FILLER — only at near-full mana (conserve for healing).
                    add("self_mana>90&target_missing_aura:Moonfire", "cast:Moonfire", 40);
                    add("self_mana>90&has_target", "cast:Wrath", 38);
                }
                else if (isTank)
                {
                    // AoE taunt FIRST when 2+ mobs the tank doesn't hold are loose nearby —
                    // Challenging Roar grabs them all at once instead of single-Growling one.
                    add("loose_enemies>1", "cast_self:Challenging Roar", 91);
                    add("enemy_loose_in_range", "cast_loose_enemy:Growl", 90);
                    // Barkskin = 20% DR (any form, off the GCD). Survival Instincts
                    // = +30% max health (feral talent). Frenzied Regeneration =
                    // rage->health self-heal (bear). All gated on real danger.
                    add("self_health<50", "buff_self:Barkskin", 89);
                    add("self_health<35", "buff_self:Survival Instincts", 88);
                    add("self_health<35", "buff_self:Frenzied Regeneration", 87);
                    // In-combat rage jump-start (the bear's Bloodrage analogue) so it can Swipe
                    // the pack for threat right away, esp. now AoE is threat-capped. Enrage needs
                    // bear form (the always-rule below keeps it) and has a 1-min CD.
                    add("in_combat&self_rage<25", "cast_self:Enrage", 86);
                    // Buff Mark of the Wild ABOVE Bear Form (can't be cast in bear form): the
                    // engine drops the form to cast it when idle+safe (TryDropFormForBuff),
                    // then this rule stops firing once everyone has it and Bear Form below
                    // reforms. Gated out_of_combat + the engine's own pull/combat guard so it
                    // never drops form mid-fight or mid-body-pull (Mill: bear couldn't MotW).
                    add("out_of_combat", "cast_party_missing:Mark of the Wild", 85);
                    // Prefer DIRE BEAR FORM when it's learned (more armor + health than plain
                    // Bear); buff_self no-ops once it's up. The Bear Form fallback is gated
                    // self_missing_aura:Dire Bear Form so a dire bear never flaps back down to
                    // plain Bear, and a low druid that hasn't learned Dire Bear still shifts.
                    add("always", "buff_self:Dire Bear Form", 84);
                    add("self_missing_aura:Dire Bear Form", "buff_self:Bear Form", 83);
                    // Feral Charge the first mob to OPEN a pull (closes the gap fast). Out of
                    // combat only + far enough to matter; needs bear form (kept above). RAGE-
                    // GATED (self_rage>5): Feral Charge costs no rage, but an empty-rage bear
                    // — fresh, or just shifted — kept misfiring it mid body-pull and breaking
                    // the gather (Kevin), so it only charges once it has some rage; otherwise
                    // it walks in. The MULTI-pull opener charge (pull layer) is rage-gated too.
                    add("out_of_combat&target_dist>11&self_rage>5", "charge", 82);
                    // Demoralizing Roar (-melee AP debuff, mitigation) HIGH PRIORITY: snap it on
                    // for an AoE pull (3+ in melee) and on ANY dungeon/raid boss, but only when
                    // the target lacks it (self-gates so it fires once then goes dormant, never
                    // starving the threat rotation). Mirrors the warrior tank's Demoralizing
                    // Shout; the cast's own range check covers "in range".
                    add("enemies_in_melee>2&target_missing_aura:Demoralizing Roar", "cast:Demoralizing Roar", 73);
                    add("target_is_boss&target_missing_aura:Demoralizing Roar", "cast:Demoralizing Roar", 72);
                    add("enemies_in_melee>2", "cast:Swipe (Bear)", 70);
                    add("has_target", "cast:Mangle (Bear)", 68);
                    add("target_missing_aura:Lacerate", "cast:Lacerate", 64);
                    add("has_target", "cast:Lacerate", 58);
                    add("target_missing_aura:Faerie Fire (Feral)", "cast:Faerie Fire (Feral)", 54);
                    add("has_target", "cast:Maul", 40);
                }
                else   // DPS — Feral cat(1) vs Balance(0). The follow layer reads live
                {      // talents (PrimaryTalentTree) to kite/close, independent of this.
                    add("party_lowest_health<30", "cast_party_lowest:Healing Touch", 84);
                    // Mark of the Wild can't be cast in Cat/Bear/Moonkin form — casting it
                    // would drop the form, and with bots perpetually "missing" the buff an
                    // `always` rule re-broke the form every tick (the druid "spazzing
                    // through forms, never fighting"). Only when genuinely idle (out of
                    // combat AND no target), the same moment cancel_form drops to caster.
                    add("out_of_combat&no_target", "cast_party_missing:Mark of the Wild", 60);
                    if (tree == 1)   // FERAL CAT — melee combo build/spend, like a rogue.
                    {
                        // Shift into Cat on IN_COMBAT, not has_target: a target isn't always
                        // set the instant a fight starts (esp. an AoE opener keyed off a
                        // cluster), which left the druid un-shifted and unable to act.
                        add("in_combat&self_missing_aura:Cat Form", "buff_self:Cat Form", 82);
                        // Close the gap on a far target: Feral Charge (Cat) in the 8-25y band, or
                        // Dash (sprint) to run down anything farther. Needs Cat form (kept above).
                        add("target_dist>11", "charge", 81);
                        add("target_dist>22", "sprint", 80);   // Dash — run down a distant target
                        add("has_target&self_energy<35", "cast_self:Tiger's Fury", 79);
                        // Savage Roar gated to elites (like rogue Slice and Dice): on trash
                        // it just eats the combo the damage finishers need.
                        add("self_missing_aura:Savage Roar&self_combo>0&target_is_elite", "cast:Savage Roar", 77);
                        add("target_health<25&self_combo>2", "cast:Ferocious Bite", 75);
                        add("self_combo>4&target_missing_aura:Rip&target_ttd>8", "cast:Rip", 73);
                        add("self_combo>4", "cast:Ferocious Bite", 70);
                        add("target_missing_aura:Rake", "cast:Rake", 66);
                        // Cat-form Swipe is a front cone — aim it at the most enemies.
                        add("enemies_in_melee>2", "cast_cone:Swipe (Cat)", 62);
                        add("has_target", "cast:Mangle (Cat)", 58);
                        add("has_target", "cast:Claw", 48);   // pre-Mangle fallback
                        // Leave form out of combat so the cat can drink/mount/buff.
                        add("out_of_combat&no_target&self_has_aura:Cat Form", "cancel_form", 16);
                    }
                    else   // BALANCE(0) — ranged caster; also the non-feral / low-level
                    {      // fallback (a no-talent low druid, or a Resto flipped to dps).
                        // Moonkin on PARTY combat, not the bot's own: keyed off self in_combat
                        // it very often hadn't entered combat yet when it fired an AoE (Hurricane)
                        // — so shift the moment the PARTY engages. has_target is also avoided (a
                        // balance opener keys off a cluster, so has_target is often false at the
                        // pull). Falls through harmlessly if untalented. cancel_form reverts it
                        // out of combat.
                        add("party_in_combat&self_missing_aura:Moonkin Form", "buff_self:Moonkin Form", 80);
                        // --- AoE (higher priority than the single-target filler) --------------
                        // Starfall: a ~90s-CD nuke used in BOTH AoE and single-target boss fights;
                        // cast on cooldown (self-throttles — the cast falls through while on CD).
                        // Gated to a pack OR a boss so it isn't blown on a lone trash mob.
                        add("enemies_clustered:8>2", "cast:Starfall", 72);
                        add("target_is_boss", "cast:Starfall", 71);
                        // Hurricane: ranged ground AoE — spam it on a cluster (leads the AoE).
                        add("enemies_clustered:8>2", "cast:Hurricane", 70);
                        // --- single-target upkeep: BOSSES ONLY (trash dies too fast to be worth
                        //     the GCDs / a 3-min treant cooldown) --------------------------------
                        add("target_is_boss&target_missing_aura:Faerie Fire", "cast:Faerie Fire", 69);
                        add("target_is_boss", "cast:Force of Nature", 67);   // treant CD; self-throttles
                        add("target_is_boss&target_missing_aura:Insect Swarm", "cast:Insect Swarm", 66);
                        // --- Eclipse filler cycle --------------------------------------------
                        // Wrath procs Lunar Eclipse (buffs Starfire); Starfire procs Solar Eclipse
                        // (buffs Wrath). eclipse_favor_starfire (coded, in PartyRotation) is TRUE
                        // while we should cast Starfire — during Lunar AND, continuing after it
                        // fades, until Solar procs — and FALSE (cast Wrath) during Solar and until
                        // Lunar procs; it opens on Wrath. So these two lines ARE the whole cycle.
                        add("eclipse_favor_starfire", "cast:Starfire", 46);
                        add("has_target", "cast:Wrath", 44);   // filler whenever we're not on Starfire
                        // Revert on PARTY out-of-combat to match the party_in_combat shift above —
                        // keyed to the bot's own out_of_combat it would fight the shift in the
                        // window where the party is fighting but this bot isn't in combat yet.
                        add("party_out_of_combat&no_target&self_has_aura:Moonkin Form", "cancel_form", 16);
                    }
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

    std::vector<HenchmanCandidate> BuildHenchmanCandidates(Player* requester)
    {
        std::vector<HenchmanCandidate> out;
        if (!requester) return out;
        std::string const acctCsv = RndbotAccountCsv();
        if (acctCsv.empty()) return out;

        uint8 const L  = requester->GetLevel();

        // Same-faction only — a Horde henchman can't group with / heal an
        // Alliance player. Race ids by team.
        std::string const raceCsv = (requester->GetTeamId() == TEAM_ALLIANCE)
            ? "1,3,4,7,11"   // Human, Dwarf, Night Elf, Gnome, Draenei
            : "2,5,6,8,10";  // Orc, Undead, Tauren, Troll, Blood Elf

        // RANDOM slice of the whole OFFLINE faction pool — every Refresh (REQ_HENCHMEN)
        // re-runs this, so ORDER BY RAND() rerolls the offers to a fresh set (Kevin: the
        // old level-proximity order was deterministic, so Refresh did nothing and specs
        // that weren't near the player's level could never be surfaced). NOT level-banded:
        // any-level picks are re-leveled to the player on hire (HireHenchman's out-of-band
        // path), so any spec becomes reachable by refreshing. `online = 0` already excludes
        // every BUSY pool bot (hired henchmen + LFG/BG/WG fill bots are logged in); the
        // IsHenchman check below is the in-memory belt-and-suspenders. LIMIT is generous so
        // the random slice covers ~every (class, spec) most refreshes; the per-class cap
        // below keeps the shown list a reasonable size.
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid`,`name`,`class`,`level` FROM `characters` "
            "WHERE `account` IN ({}) AND `online` = 0 AND `race` IN ({}) "
            "ORDER BY RAND() LIMIT 400",
            acctCsv, raceCsv);

        struct Raw { uint32 guid; std::string name; uint8 cls; uint8 level; };
        std::vector<Raw> raws;
        std::string guidCsv;
        if (q) do {
            Field* f = q->Fetch();
            uint32 const guidLow = f[0].Get<uint32>();
            if (WowPsParty::IsHenchman(ObjectGuid::Create<HighGuid::Player>(guidLow)))
                continue;   // busy: already hired by someone
            Raw r;
            r.guid  = guidLow;
            r.name  = f[1].Get<std::string>();
            r.cls   = f[2].Get<uint8>();
            r.level = f[3].Get<uint8>();
            if (!guidCsv.empty()) guidCsv += ',';
            guidCsv += std::to_string(guidLow);
            raws.push_back(std::move(r));
        } while (q->NextRow());

        // One batch talent read for ALL candidates (vs a query per row) — keyed
        // by guid so each candidate's role/spec is inferred from its own tree.
        std::unordered_map<uint32, std::unordered_set<uint32>> talents;
        if (!guidCsv.empty())
        {
            QueryResult tq = CharacterDatabase.Query(
                "SELECT `guid`,`spell` FROM `character_talent` WHERE `guid` IN ({})", guidCsv);
            if (tq) do {
                Field* tf = tq->Fetch();
                talents[tf[0].Get<uint32>()].insert(tf[1].Get<uint32>());
            } while (tq->NextRow());
        }

        // A Death Knight with ZERO spent talents (the lvl-55 starter state) has no
        // spec and no real build — useless and not worth hiring — so it must never
        // be offered. SPECIFIC to DKs: every other class is legitimately talentless
        // at low level and stays hireable (Kevin: "ignore untalented death knights").
        auto isUntalentedDk = [](uint8 cls, std::unordered_set<uint32> const& known)
        {
            return cls == CLASS_DEATH_KNIGHT && known.empty();
        };

        // Walk the RANDOM-ordered slice and keep up to MAX_SPECS_PER_CLASS distinct specs
        // per class — the first ones seen, which (because the rows are RAND()-ordered) is a
        // random pick that reshuffles every Refresh. One representative char per (class,spec).
        // The per-class cap keeps the shown list a manageable size while still offering more
        // than one spec per class (Kevin: keep the full multi-candidate list, just randomised).
        constexpr uint32 MAX_SPECS_PER_CLASS = 2;
        std::unordered_set<uint32> const noTalents;
        std::unordered_map<uint8, uint32> specsShownByClass;   // class -> distinct specs kept
        std::unordered_set<std::string> seenSpec;              // "cls:spec" dedup
        for (Raw const& r : raws)
        {
            if (specsShownByClass[r.cls] >= MAX_SPECS_PER_CLASS)
                continue;   // already showing enough specs for this class this refresh
            auto const it = talents.find(r.guid);
            std::unordered_set<uint32> const& known = (it != talents.end()) ? it->second : noTalents;
            if (isUntalentedDk(r.cls, known))
                continue;   // untalented DK — never hireable
            HenchmanCandidate c;
            c.guid  = r.guid;
            c.name  = r.name;
            c.cls   = r.cls;
            // Show + cost at the char's real level only when it's genuinely near the player
            // (±4, hired as-is); anything further is re-leveled to the player on hire, so show
            // and cost it at the player's level to match HireHenchman's effLevel (else the
            // displayed price wouldn't match what's charged).
            uint32 const levelGap = (r.level > L) ? uint32(r.level - L) : uint32(L - r.level);
            c.level = (levelGap <= 4) ? r.level : L;
            // Never offer a Death Knight below level 58 — a DK only just out of the
            // 55-58 starting experience is undergeared/awkward. c.level is the level
            // it is shown, costed and effectively hired at, so gate on that.
            if (c.cls == CLASS_DEATH_KNIGHT && c.level < 58)
                continue;
            c.role  = RoleFromTalents(c.cls, known, ClassDefaultRole(c.cls));
            c.spec  = SpecAbbrevFromTalents(c.cls, known);
            // A candidate SHOWN at level 10+ must have a spec: talents unlock at 10, so an empty
            // spec there is an anomaly — an un-talented pool char, or (with the wide re-leveled
            // sample) a sub-10 char displayed at the player's level. Only a char displayed BELOW
            // level 10 may legitimately be unspecced (Mill: no high-level unspecced henchmen).
            if (c.spec.empty() && c.level >= 10)
                continue;
            if (!seenSpec.insert(std::to_string(c.cls) + ":" + c.spec).second)
                continue;   // already showing this (class, spec)
            ++specsShownByClass[r.cls];
            out.push_back(std::move(c));
        }

        // Coverage guarantee: every faction-valid class should always be hirable. If the
        // random slice above happened to surface no candidate for a class, pull the
        // nearest-level pool char of that class and present it at the player's level —
        // re-leveled to the player when hired (see HireHenchman).
        bool present[12] = { false };
        for (auto const& c : out) if (c.cls < 12) present[c.cls] = true;
        static uint8 const kAllClasses[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 11 };
        for (uint8 cls : kAllClasses)
        {
            if (present[cls]) continue;
            if (cls == 6 && L < 58) continue;   // never offer a DK below 58 (coverage char is shown at L)
            QueryResult mq = CharacterDatabase.Query(
                "SELECT `guid`,`name` FROM `characters` "
                "WHERE `account` IN ({}) AND `online` = 0 AND `class` = {} AND `race` IN ({}) "
                "ORDER BY ABS(CAST(`level` AS SIGNED) - {}) ASC LIMIT 1",
                acctCsv, uint32(cls), raceCsv, uint32(L));
            if (!mq) continue;   // pool genuinely lacks this faction+class
            Field* mf = mq->Fetch();
            uint32 const guidLow = mf[0].Get<uint32>();
            if (WowPsParty::IsHenchman(ObjectGuid::Create<HighGuid::Player>(guidLow)))
                continue;   // busy
            if (cls == CLASS_DEATH_KNIGHT && DominantTalentTabDB(guidLow) < 0)
                continue;   // untalented DK — never hireable (DominantTalentTabDB: -1 = no talents)
            HenchmanCandidate c;
            c.guid  = guidLow;
            c.name  = mf[1].Get<std::string>();
            c.cls   = cls;
            c.level = L;   // shown + costed at the player's level; re-leveled on hire
            InferHenchmanRoleAndSpec(c.guid, cls, ClassDefaultRole(cls), c.role, c.spec);
            if (c.spec.empty() && c.level >= 10)
                continue;   // never offer an unspecced char at level 10+ (see the sampler above)
            out.push_back(std::move(c));
        }

        // Tank-CLASS coverage: a player should always be able to pick from at
        // least THREE different tank classes, even when the nearby pool is thin or
        // the nearest char of every tank class happens to be DPS-specced (the
        // per-class coverage above only guarantees a class APPEARS, in whatever
        // role its nearest char is). Pull the nearest genuinely TANK-specced pool
        // char of each tank-capable class — any level, re-leveled to the player on
        // hire (the hire path preserves the picked tank tree) — until three distinct
        // tank classes are offered. (Below 58, DK is out, leaving Warr/Pal/Druid —
        // still three.)
        std::unordered_set<uint8>  tankClassesShown;
        std::unordered_set<uint32> shownGuids;
        for (auto const& c : out)
        {
            shownGuids.insert(c.guid);
            if (c.role == "tank" && c.cls < 12) tankClassesShown.insert(c.cls);
        }
        static uint8 const kTankClasses[] = { 1, 2, 6, 11 };  // Warrior/Paladin/DK/Druid → Prot/Prot/Blood/Bear
        for (uint8 cls : kTankClasses)
        {
            if (tankClassesShown.size() >= 3) break;
            if (tankClassesShown.count(cls)) continue;   // this tank class already offered
            if (cls == 6 && L < 58) continue;            // never offer a DK below 58 (tank char is shown at L)

            // Nearest-level pool chars of this class; pick the closest one that is
            // actually tank-specced. Batch one talent read for the slice.
            QueryResult cq = CharacterDatabase.Query(
                "SELECT `guid`,`name`,`level` FROM `characters` "
                "WHERE `account` IN ({}) AND `online` = 0 AND `class` = {} AND `race` IN ({}) "
                "ORDER BY ABS(CAST(`level` AS SIGNED) - {}) ASC LIMIT 60",
                acctCsv, uint32(cls), raceCsv, uint32(L));
            if (!cq) continue;

            struct TRaw { uint32 guid; std::string name; };
            std::vector<TRaw> traws;
            std::string tcsv;
            do {
                Field* tf = cq->Fetch();
                uint32 const g = tf[0].Get<uint32>();
                if (WowPsParty::IsHenchman(ObjectGuid::Create<HighGuid::Player>(g)))
                    continue;                 // busy
                if (shownGuids.count(g))
                    continue;                 // already on the list
                if (!tcsv.empty()) tcsv += ',';
                tcsv += std::to_string(g);
                traws.push_back(TRaw{ g, tf[1].Get<std::string>() });
            } while (cq->NextRow());
            if (traws.empty()) continue;

            std::unordered_map<uint32, std::unordered_set<uint32>> ttal;
            QueryResult ttq = CharacterDatabase.Query(
                "SELECT `guid`,`spell` FROM `character_talent` WHERE `guid` IN ({})", tcsv);
            if (ttq) do {
                Field* tf = ttq->Fetch();
                ttal[tf[0].Get<uint32>()].insert(tf[1].Get<uint32>());
            } while (ttq->NextRow());

            // traws is nearest-level-first — take the first genuine tank.
            for (TRaw const& t : traws)
            {
                auto const it = ttal.find(t.guid);
                std::unordered_set<uint32> const& known = (it != ttal.end()) ? it->second : noTalents;
                if (isUntalentedDk(cls, known))
                    continue;   // untalented DK — never hireable (would otherwise pass via the "tank" default role)
                if (RoleFromTalents(cls, known, ClassDefaultRole(cls)) != "tank")
                    continue;
                HenchmanCandidate c;
                c.guid  = t.guid;
                c.name  = t.name;
                c.cls   = cls;
                c.level = L;            // shown + costed at the player's level; re-leveled on hire
                c.role  = "tank";
                c.spec  = SpecAbbrevFromTalents(cls, known);
                if (c.spec.empty() && c.level >= 10)
                    continue;   // never offer an unspecced char at level 10+; try the next tank
                out.push_back(std::move(c));
                tankClassesShown.insert(cls);
                shownGuids.insert(t.guid);
                break;
            }
        }
        if (tankClassesShown.size() < 3)
            LOG_INFO("module",
                "[WowPsParty Henchmen] tank-class coverage only found {} tank class(es) for guid={} level={} "
                "(pool may lack tank-specced chars of a third tank class)",
                uint32(tankClassesShown.size()), requester->GetGUID().GetCounter(), uint32(L));

        // Sort for the hire screen: tanks first, then healers, then dps; within
        // a role by class id, then spec. (Kevin's requested ordering.)
        auto roleRank = [](std::string const& role) -> int {
            if (role == "tank")   return 0;
            if (role == "healer") return 1;
            return 2;   // dps / anything else
        };
        std::sort(out.begin(), out.end(),
            [&](HenchmanCandidate const& a, HenchmanCandidate const& b)
            {
                int const ra = roleRank(a.role), rb = roleRank(b.role);
                if (ra != rb)       return ra < rb;
                if (a.cls != b.cls) return a.cls < b.cls;
                return a.spec < b.spec;
            });
        return out;
    }

    // Set the group's loot rule based on whether any SELF-LOOTING companion is
    // present (a hired henchman or a hired alt): if so → GROUP_LOOT (rolls /
    // round-robin) so each can claim its own share by looting the corpse; else →
    // FREE_FOR_ALL (the all-enrolled premade, which auto-distributes on kill).
    static void UpdateGroupLootForHenchmen(Player* leader)
    {
        if (!leader) return;
        Group* g = leader->GetGroup();
        if (!g) return;
        bool const hasSelfLooter = WowPsParty::CountHenchmenFor(leader->GetGUID()) > 0
                                || WowPsParty::CountHiredAltsFor(leader->GetGUID()) > 0;
        g->SetLootMethod(hasSelfLooter ? GROUP_LOOT : FREE_FOR_ALL);
        g->SendUpdate();
        LOG_INFO("module", "[WowPsParty Henchmen] loot method -> {} (self-looters present={})",
                 hasSelfLooter ? "GROUP_LOOT" : "FREE_FOR_ALL", hasSelfLooter);
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

    // Level-appropriate gear picks (used by MaintainTankThrown / MaintainTankShield):
    // WotLK BoP dungeon drops carry RequiredLevel 0 with raid-tier ItemLevels (e.g.
    // the ilvl-174 "Iron Coffin Lid" shield and ilvl-174 thrown weapons), so a plain
    // "RequiredLevel <= level ORDER BY ItemLevel DESC" hands a level-34 bot a level-80
    // item. Both helpers instead EXCLUDE RequiredLevel 0 and take the highest
    // RequiredLevel the bot's level allows (ItemLevel as tiebreak), keeping the pick
    // in line with the bot's other factory gear (shield ilvl ~39 / thrown ~27 at
    // level 34, shield ~200 at 80). Verified no RequiredLevel-1..N item of either
    // slot has an anomalous ItemLevel, so excluding 0 fully removes the freaks.

    // Warrior tanks pull from range (see TankLeadEngagement), which needs a thrown
    // weapon in the ranged slot. Only WARRIORS can equip thrown weapons, so this is
    // warrior-tank-only. Equips the best level-appropriate thrown weapon when the
    // ranged slot is empty OR holds a freak over-leveled thrown (an earlier bug put
    // ilvl-174 RequiredLevel-0 throwns on low-level tanks). A player-chosen bow/gun
    // or a level-appropriate thrown is left untouched.
    static void MaintainTankThrown(Player* bot)
    {
        if (bot->getClass() != CLASS_WARRIOR) return;
        if (WowPsParty::RoleForGuid(bot->GetGUID()) != "tank") return;

        // class=2 weapon, subclass=16 thrown, InventoryType=25 INVTYPE_THROWN.
        // Common..rare only (no heirloom/artifact). See the gear-pick note above.
        uint32 const lvl = bot->GetLevel();
        QueryResult q = WorldDatabase.Query(
            "SELECT entry FROM item_template "
            "WHERE class = 2 AND subclass = 16 AND InventoryType = 25 "
            "AND Quality BETWEEN 1 AND 3 "
            "AND RequiredLevel BETWEEN 1 AND {} "
            "ORDER BY RequiredLevel DESC, ItemLevel DESC LIMIT 10", lvl);
        if (!q) return;

        // Best level-appropriate thrown the bot can use (CanUseItem is slot-
        // independent, so evaluate it before freeing the ranged slot).
        uint32 bestEntry = 0, bestIl = 0;
        do
        {
            uint32 const entry = (*q)[0].Get<uint32>();
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto || bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            bestEntry = entry;
            bestIl = proto->ItemLevel;
            break;
        } while (q->NextRow());
        if (!bestEntry) return;

        Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (ranged)
        {
            ItemTemplate const* rp = ranged->GetTemplate();
            bool const isThrown = rp->Class == ITEM_CLASS_WEAPON &&
                                  rp->SubClass == ITEM_SUBCLASS_WEAPON_THROWN;
            // Leave a player-chosen bow/gun alone, and keep a level-appropriate
            // thrown; only swap a freak over-leveled thrown (ilvl far above the pick).
            if (!isThrown) return;
            if (rp->ItemLevel <= bestIl * 2) return;

            ItemPosCountVec stash;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, stash, ranged, false) != EQUIP_ERR_OK)
                return;
            bot->RemoveItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED, true);
            bot->StoreItem(stash, ranged, true);
        }

        uint16 dest = 0;
        if (bot->CanEquipNewItem(NULL_SLOT, dest, bestEntry, false) != EQUIP_ERR_OK)
            return;
        bot->EquipNewItem(dest, bestEntry, true);
        LOG_INFO("module", "[WowPsParty Provision] equipped thrown weapon {} (ilvl {}) on {} (lvl {})",
                 bestEntry, bestIl, bot->GetName(), lvl);
    }

    // Prot tanks (warrior & paladin) MUST carry a shield — block, Shield Slam /
    // Shield of Righteousness, Holy Shield and a large slice of their mitigation
    // all key off one being equipped. The playerbot gear factory picks the
    // offhand by stat-weight and can land on a 1H weapon or a holdable frill
    // (shields aren't guaranteed into its random candidate pool), so a hired prot
    // tank can arrive shieldless. Guarantee a level-appropriate one: equip the
    // best level-appropriate shield (see the gear-pick note above MaintainTankThrown)
    // when the offhand is missing,
    // non-shield, OR a freak over-leveled shield (an earlier provisioning bug put
    // an ilvl-174 RequiredLevel-0 raid shield on a level-34 tank). Gated on
    // role=tank (only prot wants sword-and-board; a fury/arms/ret hire of this
    // class is left alone) and a no-op once an appropriate shield is on, so it
    // never thrashes equipped gear.
    static void MaintainTankShield(Player* bot)
    {
        uint8 const cls = bot->getClass();
        if (cls != CLASS_WARRIOR && cls != CLASS_PALADIN) return;
        if (WowPsParty::RoleForGuid(bot->GetGUID()) != "tank") return;

        // A 2H weapon in the mainhand blocks sword-and-board outright; don't
        // unequip the offhand chasing a shield we then can't wear (a prot tank
        // shouldn't be on a 2H — the stat-weight calc penalises it ×0.1 — but
        // guard anyway so we never strip a 2H build down to a bare offhand).
        if (Item* mh = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
            if (mh->GetTemplate()->InventoryType == INVTYPE_2HWEAPON)
                return;

        // class=4 ARMOR, subclass=6 SHIELD, InventoryType=14 INVTYPE_SHIELD.
        // Common..rare only (no heirloom/artifact). See the gear-pick note above
        // MaintainTankThrown — excludes RequiredLevel 0 so a freak raid shield
        // can't land on a low-level tank.
        uint32 const lvl = bot->GetLevel();
        QueryResult q = WorldDatabase.Query(
            "SELECT entry FROM item_template "
            "WHERE class = 4 AND subclass = 6 AND InventoryType = 14 "
            "AND Quality BETWEEN 1 AND 3 "
            "AND RequiredLevel BETWEEN 1 AND {} "
            "ORDER BY RequiredLevel DESC, ItemLevel DESC LIMIT 15", lvl);
        if (!q) return;

        // Best band shield the bot can actually use (highest ItemLevel first).
        // CanUseItem is slot-independent, so evaluate it before freeing the offhand.
        uint32 bestEntry = 0, bestIl = 0;
        do
        {
            uint32 const entry = (*q)[0].Get<uint32>();
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto || bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;   // class/skill/level gate
            bestEntry = entry;
            bestIl = proto->ItemLevel;
            break;
        } while (q->NextRow());
        if (!bestEntry) return;   // nothing usable in band

        Item* off = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        if (off && off->GetTemplate()->Class == ITEM_CLASS_ARMOR &&
            off->GetTemplate()->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)
        {
            // Already shielded — keep it unless it's wildly over-leveled for this
            // bot (a RequiredLevel-0 raid shield from the old bug, ilvl far above
            // the band). A real upgrade a notch above the band is fine; only a big
            // multiple is a freak worth swapping down.
            if (off->GetTemplate()->ItemLevel <= bestIl * 2)
                return;
        }

        // Stash the current offhand (non-shield, or the freak shield) into the
        // bags so the slot is free. If the bags can't take it, bail rather than
        // destroy a usable item.
        if (off)
        {
            ItemPosCountVec stash;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, stash, off, false) != EQUIP_ERR_OK)
                return;
            bot->RemoveItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);
            bot->StoreItem(stash, off, true);
        }

        uint16 dest = 0;
        if (bot->CanEquipNewItem(NULL_SLOT, dest, bestEntry, false) != EQUIP_ERR_OK)
        {
            // Offhand is empty now and CanUseItem already passed, so reaching here
            // means a transient block (combat/cast/stun). Next tick retries cleanly;
            // log it so a persistently shieldless prot bot is diagnosable.
            LOG_DEBUG("module", "[WowPsParty Provision] shield {} not equippable on prot {} this tick (will retry)",
                      bestEntry, bot->GetName());
            return;
        }
        bot->EquipNewItem(dest, bestEntry, true);
        LOG_INFO("module", "[WowPsParty Provision] equipped shield {} (ilvl {}) on prot {} (lvl {})",
                 bestEntry, bestIl, bot->GetName(), lvl);
    }

    // A DPS warrior (Arms/Fury) must NEVER sword-and-board, AND a Fury build must
    // dual-wield rather than swing a lone 1H. This (a) strips a shield/holdable
    // offhand to the bags, then (b) FILLS an empty offhand with a weapon so a Fury
    // hire isn't left single-wielding — the factory keeps shields out of the pool
    // but an in-band hire (kept gear) or a just-stripped shield can leave the slot
    // bare. Gated role!=tank (a Protection warrior keeps its shield) and a no-op on
    // a legitimate weapon offhand, so it never thrashes correct gear.
    static void MaintainDpsWarriorOffhand(Player* bot)
    {
        if (bot->getClass() != CLASS_WARRIOR) return;
        if (WowPsParty::RoleForGuid(bot->GetGUID()) == "tank") return;

        // (a) Strip a NON-weapon offhand (shield / holdable) to the bags.
        Item* off = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        if (off && off->GetTemplate()->Class != ITEM_CLASS_WEAPON)
        {
            ItemPosCountVec stash;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, stash, off, false) != EQUIP_ERR_OK)
                return;   // bags full — bail rather than destroy a usable item
            bot->RemoveItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);
            bot->StoreItem(stash, off, true);
            LOG_INFO("module", "[WowPsParty Provision] stripped off-hand {} from DPS warrior {} (lvl {})",
                     off->GetEntry(), bot->GetName(), bot->GetLevel());
            off = nullptr;
        }
        if (off) return;   // already holding an offhand WEAPON (Fury dual-wield) — done.

        // (b) Offhand empty — fill it to match the build:
        //   2H mainhand + Titan's Grip  -> a 2H offhand (TG Fury wields two 2H)
        //   1H mainhand + Dual Wield     -> a 1H offhand (standard Fury)
        //   otherwise (Arms 2H / no dual wield / <20) -> nothing (correct).
        Item* mh = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (!mh) return;   // no mainhand yet — that gets sorted first
        bool want2h = false;
        if (mh->GetTemplate()->InventoryType == INVTYPE_2HWEAPON)
        {
            if (!bot->CanTitanGrip()) return;   // Arms / non-TG 2H build — no offhand
            want2h = true;
        }
        else if (!bot->CanDualWield())
        {
            return;   // 1H mainhand but can't dual wield yet (sub-20) — leave empty
        }

        // Best level-appropriate weapon the bot can actually wield in the offhand.
        // 1H offhand types = INVTYPE_WEAPON(13)/INVTYPE_WEAPONOFFHAND(22); TG 2H = INVTYPE_2HWEAPON(17).
        // Excludes RequiredLevel 0 (the freak-item signature) like the shield/thrown picks.
        uint32 const lvl = bot->GetLevel();
        char const* const invFilter = want2h ? "= 17" : "IN (13, 22)";
        QueryResult q = WorldDatabase.Query(
            "SELECT entry FROM item_template "
            "WHERE class = 2 AND InventoryType {} "
            "AND Quality BETWEEN 1 AND 3 "
            "AND RequiredLevel BETWEEN 1 AND {} "
            "ORDER BY RequiredLevel DESC, ItemLevel DESC LIMIT 30", invFilter, lvl);
        if (!q) return;

        uint32 bestEntry = 0;
        uint16 bestDest  = 0;
        do
        {
            uint32 const entry = (*q)[0].Get<uint32>();
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto || bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;   // weapon-skill / level gate
            uint16 dest = 0;
            if (bot->CanEquipNewItem(EQUIPMENT_SLOT_OFFHAND, dest, entry, false) != EQUIP_ERR_OK)
                continue;   // dual-wield / Titan's-Grip gate for the offhand slot
            bestEntry = entry;
            bestDest  = dest;
            break;
        } while (q->NextRow());
        if (!bestEntry) return;

        bot->EquipNewItem(bestDest, bestEntry, true);
        LOG_INFO("module", "[WowPsParty Provision] equipped off-hand weapon {} on DPS warrior {} (lvl {})",
                 bestEntry, bot->GetName(), lvl);
    }

    // Trim a bot's Soul Shards down to exactly one. `preferKeep` (when non-null)
    // is the shard to spare — the OnPlayerStoreNewItem hook passes the item it
    // just stored so StoreNewItem's caller still dereferences a live object;
    // otherwise the first shard found is kept. Collects positions, decides the
    // keeper, then destroys the surplus (inventory slots are positional, so a
    // DestroyItem never shifts the others). Mirrors ClearHenchmanInventory's
    // backpack + equipped-bag sweep — the soul pouch is one of those bags.
    void TrimSoulShardsToOne(Player* bot, Item* preferKeep)
    {
        if (!bot) return;
        constexpr uint32 SOUL_SHARD_ITEM_ID = 6265;

        Item* spare = preferKeep;
        auto sweep = [&](uint8 bag, uint8 slot, Item* it)
        {
            if (!it || it->GetEntry() != SOUL_SHARD_ITEM_ID) return;
            if (!spare) { spare = it; return; }   // first shard becomes the keeper
            if (it == spare) return;              // keep the designated shard
            bot->DestroyItem(bag, slot, true);    // surplus
        };

        for (uint8 s = INVENTORY_SLOT_ITEM_START; s < INVENTORY_SLOT_ITEM_END; ++s)
            sweep(INVENTORY_SLOT_BAG_0, s, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = bot->GetBagByPos(bag))
                for (uint32 s = 0; s < container->GetBagSize(); ++s)
                    sweep(bag, uint8(s), container->GetItemByPos(uint8(s)));
    }

    // A hired henchman drawn from a higher level is down-leveled to the party
    // leader (PlayerbotFactory in HireHenchman). That path does NOT strip the
    // spells/ranks the bot earned at its former level: talent-granted abilities
    // whose talent it no longer has (e.g. a level-34 Prot warrior keeping
    // Devastate, a 41+ talent, or Shockwave/Vigilance from the deep tree) and
    // higher ability ranks (Cleave/Thunder Clap ranks gated above the new level).
    // Those make a "level 34" bot hit like an 80. resetTalents only clears spells
    // for talents STILL allocated, so orphans from talents already gone survive —
    // and an in-band re-hire of a bot that was previously down-leveled and logged
    // out at the reduced level (we don't restore it) carries the whole bloated
    // book. This rebuilds the book to match the bot's ACTUAL level + talents.
    static void SanitizeHenchmanSpells(Player* bot)
    {
        uint32 const level = bot->GetLevel();
        uint8 const  spec  = bot->GetActiveSpec();
        uint32 const classMask = 1u << (bot->getClass() - 1);

        // Build, over every talent of this class: the full set of talent-grantable
        // spells (universe) and the subset granted by the bot's CURRENT talents
        // (justified). A rank's spell, plus any spell it LEARN_SPELLs (Devastate is
        // learned this way), counts for both sets.
        std::unordered_set<uint32> universe, justified;
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* te = sTalentStore.LookupEntry(i);
            if (!te) continue;
            TalentTabEntry const* tab = sTalentTabStore.LookupEntry(te->TalentTab);
            if (!tab || !(tab->ClassMask & classMask)) continue;

            for (uint8 r = 0; r < MAX_TALENT_RANK; ++r)
            {
                uint32 const rankSpell = te->RankID[r];
                if (!rankSpell) continue;
                bool const allocated = bot->HasTalent(rankSpell, spec);

                auto note = [&](uint32 s) { universe.insert(s); if (allocated) justified.insert(s); };
                note(rankSpell);
                if (SpellInfo const* si = sSpellMgr->GetSpellInfo(rankSpell))
                    for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                        if (si->Effects[e].Effect == SPELL_EFFECT_LEARN_SPELL && si->Effects[e].TriggerSpell)
                            note(si->Effects[e].TriggerSpell);
            }
        }

        // Remove (a) talent spells not backed by a current talent and (b) ability
        // ranks whose spell level exceeds the bot's level. Snapshot first — the
        // map can't be mutated mid-iteration.
        std::vector<uint32> toRemove;
        for (auto const& pair : bot->GetSpellMap())
        {
            uint32 const spellId = pair.first;
            if (pair.second->State == PLAYERSPELL_REMOVED) continue;
            SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId);
            if (!si) continue;

            bool const isTalentSpell = universe.count(spellId) != 0;
            if (isTalentSpell)
            {
                if (!justified.count(spellId)) toRemove.push_back(spellId);
            }
            else if (si->SpellLevel > level)
            {
                toRemove.push_back(spellId);
            }
        }
        if (toRemove.empty()) return;

        for (uint32 s : toRemove)
            bot->removeSpell(s, SPEC_MASK_ALL, false);

        // Removing an over-level rank can leave the bot with NO rank of an ability
        // (only the highest rank was known). Re-learn the level-appropriate ranks
        // through the trainer-gated factory init — it only ADDS what CanTeachSpell
        // permits at the bot's level, so it fills the gaps without re-adding the
        // stripped over-level ranks or the orphaned talent spells.
        PlayerbotFactory factory(bot, level);
        factory.InitClassSpells();
        factory.InitAvailableSpells();
        bot->SendTalentsInfoData(false);

        LOG_INFO("module", "[WowPsParty Provision] sanitized {} spells on henchman {} (lvl {})",
                 toRemove.size(), bot->GetName(), level);
    }

    // Run the sanitize at most once per (guid, level): on the first sighting of a
    // henchman this session (heals an already-bloated pool bot) and again whenever
    // its level changes (a fresh down-level). Cheap no-op otherwise.
    static void SanitizeHenchmanSpellsIfNeeded(Player* bot)
    {
        if (!bot || !bot->IsInWorld()) return;
        if (!WowPsParty::IsHenchman(bot->GetGUID())) return;   // never touch a player's own alt

        static std::unordered_map<uint32, uint32> sanitizedAtLevel;
        static std::mutex mtx;
        uint32 const guid = bot->GetGUID().GetCounter();
        uint32 const level = bot->GetLevel();
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = sanitizedAtLevel.find(guid);
            if (it != sanitizedAtLevel.end() && it->second == level) return;
            sanitizedAtLevel[guid] = level;
        }
        SanitizeHenchmanSpells(bot);
    }

    void MaintainBotConsumables(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !bot->GetSession()) return;

        // HIRED ALTS are left EXACTLY as the player parked them — no ammo refill,
        // no poison imbue, no shard trim, no thrown/shield/offhand swap, no spell
        // sanitize. Everything below mutates the character's items or spellbook, so
        // a real alt the player will log into later must skip all of it.
        if (WowPsParty::IsHiredAlt(bot->GetGUID())) return;

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

        // Strip any over-level / orphaned-talent spells a down-leveled henchman
        // kept from a former higher level (once per guid+level; no-op once clean).
        SanitizeHenchmanSpellsIfNeeded(bot);

        uint8 const cls = bot->getClass();
        if (cls == CLASS_ROGUE)   MaintainPoisons(bot);
        if (cls == CLASS_WARRIOR) MaintainTankThrown(bot);   // before ammo: equips the thrown wpn
        if (cls == CLASS_WARRIOR || cls == CLASS_PALADIN) MaintainTankShield(bot);  // prot tanks always carry a shield
        if (cls == CLASS_WARRIOR) MaintainDpsWarriorOffhand(bot);  // Arms/Fury never sword-and-board
        // Warlock bots only (defensive GetPlayerbotAI guard — a human warlock
        // manages their own shards and must never be trimmed). Catches a bot
        // that reloaded with a pre-existing pile or sits at zero free slots,
        // where no new shard can be created to trip the store-hook cap.
        if (cls == CLASS_WARLOCK && sPlayerbotsMgr.GetPlayerbotAI(bot))
            TrimSoulShardsToOne(bot, nullptr);
        MaintainAmmo(bot);   // any class that wields a bow/gun/thrown benefits

        // Backfill class-QUEST abilities (druid forms, warlock pets, paladin
        // mounts). Bots never run the class quests, so a hired druid henchman
        // arrives with no Bear Form; LearnAllClassSpells (trainer set) won't cover
        // it. HasSpell-gated, so this is a no-op once taught.
        WowPsParty::LearnClassQuestSkills(bot);
    }

    // Re-level an already-hired, in-world henchman to an EXACT target level (up or
    // down), preserving its role. Mirrors the proven out-of-band relevel + role-force
    // path in HireHenchman, just to an arbitrary target: Randomize re-rolls
    // level-appropriate gear + stats (and a RANDOM talent tree), so we (1) restore
    // the bot's captured tree, (2) force the role's tree for a tank/healer so it
    // really tanks/heals (also the only guard for the feral cat/bear tab-1 case),
    // (3) re-derive the role from the final talents and rebuild the role-default
    // rotation/targetmode — otherwise a scaled pick could keep a tank rotation on a
    // now-DPS spec — and (4) strip over/under-level spell ranks
    // (SanitizeHenchmanSpellsIfNeeded is memoized per (guid,level), so the level
    // change re-triggers it). Henchmen never keep a custom rotation, so the rebuild
    // is unconditional.
    static void ReLevelHenchmanInPlace(Player* hen, uint8 target)
    {
        if (!hen || !hen->IsInWorld()) return;
        if (target < 1)  target = 1;
        if (target > 80) target = 80;
        uint32 const oldLvl = hen->GetLevel();
        if (uint8(oldLvl) == target) return;

        uint8 const cls = hen->getClass();
        // Capture role + tree BEFORE the re-roll (the bot's hired/forced identity).
        std::string const role = InferHenchmanRoleLive(hen, ClassDefaultRole(cls));
        int const intendedTab  = DominantTalentTabLive(hen);

        PlayerbotFactory factory(hen, target);
        factory.Randomize(false);

        bool talentsChanged = false;
        // Restore the captured tree if the re-roll moved off it.
        if (intendedTab >= 0 && DominantTalentTabLive(hen) != intendedTab)
        {
            int const specNo = int(sPlayerbotAIConfig.randomClassSpecIndex[cls][uint32(intendedTab)]);
            PlayerbotFactory::InitTalentsBySpecNo(hen, specNo, /*reset=*/true);
            hen->SendTalentsInfoData(false);
            talentsChanged = true;
        }
        // A tank/healer must actually BE that spec — force the role's canonical tree
        // (DPS returns -1 → no force, any DPS spec is fine).
        if (role == "tank" || role == "healer")
        {
            int const wantTab = DesiredTalentTabForRole(cls, role);
            if (wantTab >= 0 && DominantTalentTabLive(hen) != wantTab)
            {
                int const specNo = int(sPlayerbotAIConfig.randomClassSpecIndex[cls][uint32(wantTab)]);
                PlayerbotFactory::InitTalentsBySpecNo(hen, specNo, /*reset=*/true);
                hen->SendTalentsInfoData(false);
                talentsChanged = true;
            }
        }
        // Randomize itemized gear for the RANDOM tree it rolled; once we move the
        // talents off that tree (restore/force above) the gear no longer matches the
        // spec — the factory weighs a Ret 2H ×0.05 for Protection but ×1 for Ret, so a
        // prot tank re-specced after a Ret re-roll is left in a Ret 2H + Ret plate.
        // Re-roll equipment against the FINAL talents so the loadout matches the spec.
        if (talentsChanged)
            PlayerbotFactory(hen, target).InitEquipment(false, false);
        hen->SaveToDB(false, false);

        // Re-derive the role from the final talents and rebuild the role-default
        // rotation/targetmode so a scaled pick never keeps a stale rotation.
        std::string const freshRole = InferHenchmanRoleLive(hen, ClassDefaultRole(cls));
        WowPsParty::SetHenchmanRole(hen->GetGUID(), freshRole);
        WowPsParty::RotationCacheSet(hen->GetGUID().GetCounter(),
            WowPsParty::ParseRotationString(
                DefaultRotationForClass(cls, freshRole, DominantTalentTabLive(hen))));
        WowPsParty::TargetModeCacheSet(hen->GetGUID().GetCounter(),
            freshRole == "tank" ? "loose" : "master");

        SanitizeHenchmanSpellsIfNeeded(hen);
        LOG_INFO("module",
            "[WowPsParty LfgScale] re-leveled hench guid={} {} -> {} for dungeon range",
            hen->GetGUID().GetCounter(), oldLvl, uint32(target));
    }

    void ScaleHenchmenForDungeons(Player* leader, std::set<uint32> const& dungeons)
    {
        if (!leader || dungeons.empty()) return;
        Group* grp = leader->GetGroup();
        if (!grp) return;

        // Effective range across the selected dungeons: highest floor .. lowest
        // ceiling (so a single scaled level satisfies EVERY selected dungeon at once).
        uint8 effMin = 0, effMax = 80;
        bool any = false;
        for (uint32 d : dungeons)
        {
            lfg::LFGDungeonData const* dd = sLFGMgr->GetLFGDungeon(d);
            if (!dd || dd->maxlevel == 0) continue;   // not a real dungeon entry
            effMin = std::max<uint8>(effMin, dd->minlevel);
            effMax = std::min<uint8>(effMax, dd->maxlevel);
            any = true;
        }
        if (!any || effMin > effMax) return;   // no real range / dungeons don't overlap

        for (GroupReference* itr = grp->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* m = itr->GetSource();
            if (!m || m == leader || !m->IsInWorld()) continue;
            if (!WowPsParty::IsHenchman(m->GetGUID())) continue;
            uint8 const cur    = m->GetLevel();
            uint8 const target = cur < effMin ? effMin : (cur > effMax ? effMax : cur);
            if (target != cur)
                ReLevelHenchmanInPlace(m, target);
        }
    }

    bool HireHenchman(Player* requester, uint32 candidateGuid,
                      std::string const& role, std::string& outMsg, bool skipCharge)
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

        // Party-space cap. A normal party holds the leader + 4 companions; once
        // the leader CONVERTS the group to a RAID it holds up to 40 members, so a
        // 10/25-man can be filled entirely with companions. Count THIS leader's
        // followers (alts + henchmen) from the directive registry — registered
        // synchronously at hire, so still-spawning hires count and rapid hiring
        // can't overshoot; it also works before the WoW group object exists (solo
        // + first henchman). A second hard-stop on the live group size keeps a
        // multi-human raid from blowing past 40.
        Group* const reqGroup = requester->GetGroup();
        bool   const inRaid    = reqGroup && reqGroup->isRaidGroup();
        uint32 const companionCap = inRaid ? 39u : 4u;   // 40-/5-member group minus the leader
        uint32 const followers    = WowPsParty::CountFollowersFor(requester->GetGUID());
        if (followers >= companionCap
            || (reqGroup && reqGroup->GetMembersCount() >= (inRaid ? 40u : 5u)))
        {
            outMsg = inRaid
                ? "Your raid is full."
                : "Your party is full (4 companions). Convert your group to a raid to add more.";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: group full (followers={}, raid={})",
                     candidateGuid, followers, inRaid ? 1 : 0);
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
        if (!skipCharge && requester->GetMoney() < cost)
        {
            outMsg = "Not enough gold to hire.";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: gold {} < cost {}",
                     candidateGuid, requester->GetMoney(), cost);
            return false;
        }
        // When skipCharge is set the caller already charged a single summed price
        // (the LFG party-fill's discounted total) — this hire neither deducts nor,
        // on a no-show/full group, refunds. refundAmt flows into the deferred lambda.
        uint32 const refundAmt = skipCharge ? 0u : cost;

        PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(requester);
        if (!mgr)
        {
            outMsg = "Bot manager not ready — try again in a moment.";
            LOG_INFO("module", "[WowPsParty Henchmen] hire REFUSED guid={}: no PlayerbotMgr", candidateGuid);
            return false;
        }

        if (!skipCharge)
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

        // Restore this henchman's persisted NON-rotation loadout (targetmode / lead /
        // wait / safe-pull). The ROTATION is deliberately NOT persisted for henchmen
        // (Kevin): every hire starts from the class default — the SAME rotation the
        // editor's "Generate" hands back (both call DefaultRotationForClass with the
        // directive role) — so a henchman never carries a rotation from a prior run.
        // We also WIPE any stored priority_actions_json on hire so it can't linger.
        // (Enrolled ALT bots keep their saved rotation — this is HireHenchman only.)
        //   targetmode: saved strategies_csv, else tank -> "loose" / "master".
        //   lead       : saved glyphs_csv ("0" = off), else ON.
        //   waitthreat : saved wait_tank_threat ('1'/'0'), else unset -> a
        //                henchman's per-type default is to WAIT for tank threat.
        bool const hadCustomRotation = false;   // henchmen NEVER keep a custom rotation
        {
            QueryResult lq = CharacterDatabase.Query(
                "SELECT `strategies_csv`,`glyphs_csv`,`wait_tank_threat`,`safe_pull`,`pull_count`,`engage_range`,`follow_path` "
                "FROM `party_loadout` WHERE `guid` = {}", candidateGuid);
            std::string savedMode, savedLead, savedWait, savedSafePull, savedPullCount, savedEngageRange, savedFollowPath;
            if (lq)
            {
                Field* lf = lq->Fetch();
                savedMode        = lf[0].Get<std::string>();
                savedLead        = lf[1].Get<std::string>();
                savedWait        = lf[2].Get<std::string>();
                savedSafePull    = lf[3].Get<std::string>();
                savedPullCount   = lf[4].Get<std::string>();
                savedEngageRange = lf[5].Get<std::string>();
                savedFollowPath  = lf[6].Get<std::string>();
            }

            // Always the class default rotation (identical to "Generate"); never the
            // saved one. Then wipe any persisted rotation so it can't survive the run.
            WowPsParty::RotationCacheSet(candidateGuid,
                WowPsParty::ParseRotationString(
                    DefaultRotationForClass(cls, useRole, DominantTalentTabDB(candidateGuid))));
            CharacterDatabase.Execute(
                "UPDATE `party_loadout` SET `priority_actions_json` = '' WHERE `guid` = {}", candidateGuid);

            WowPsParty::TargetModeCacheSet(candidateGuid,
                !savedMode.empty() ? savedMode
                                   : (useRole == "tank" ? "loose" : "master"));

            WowPsParty::LeadDungeonCacheSet(candidateGuid, savedLead != "0");

            WowPsParty::WaitTankThreatCacheSet(candidateGuid,
                savedWait == "1" ? 1 : (savedWait == "0" ? 0 : -1));

            WowPsParty::SafePullCacheSet(candidateGuid,
                savedSafePull == "1" ? 1 : (savedSafePull == "0" ? 0 : -1));

            WowPsParty::PullCountCacheSet(candidateGuid,
                savedPullCount.empty() ? 0 : std::atoi(savedPullCount.c_str()));

            WowPsParty::EngageRangeCacheSet(candidateGuid,
                savedEngageRange.empty() ? 0 : std::atoi(savedEngageRange.c_str()));

            // follow_path: '0' = explicit off, else (''/'1') the default -> follow.
            WowPsParty::FollowPathCacheSet(candidateGuid,
                savedFollowPath == "1" ? 1 : (savedFollowPath == "0" ? 0 : -1));
        }

        mgr->AddPlayerBot(henchGuid, account);

        // The spawn is async (login query holder). After a short delay: if the
        // bot arrived, group it + set loot; if it never arrived, undo the hire
        // (drop the directive, refund the gold) so we never leak a directive or
        // charge the player for a no-show.
        ObjectGuid const leaderGuid = requester->GetGUID();
        requester->m_Events.AddEventAtOffset([leaderGuid, henchGuid, cost, refundAmt, cls, hadCustomRotation, useRole]()
        {
            Player* lead = ObjectAccessor::FindConnectedPlayer(leaderGuid);
            Player* hen  = ObjectAccessor::FindConnectedPlayer(henchGuid);
            if (!hen || !hen->IsInWorld())
            {
                WowPsParty::RemoveFollower(henchGuid);
                if (lead)
                {
                    if (refundAmt)
                        lead->ModifyMoney(int32(refundAmt));   // refund the no-show
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
                    // Capture the spec the PLAYER picked at the hire screen BEFORE the
                    // re-roll. Randomize(false) re-rolls talents to a RANDOM tree, which
                    // flips the role out from under the player — a Ret paladin hired and
                    // downleveled came back Holy, invalidating the party (wasted gold).
                    int const intendedTab = DominantTalentTabLive(hen);
                    PlayerbotFactory factory(hen, target);
                    factory.Randomize(false);
                    // Restore the picked tree if the re-roll moved off it. randomClassSpecIndex
                    // maps a tree (0/1/2) to its spec-template index — the same mapping
                    // InitTalentsTree uses — so InitTalentsBySpecNo re-spends the points into
                    // the chosen tree. (Druid feral cat vs bear share tree 1, so that one
                    // edge can still pick the wrong feral build; every other class is exact.)
                    if (intendedTab >= 0 && DominantTalentTabLive(hen) != intendedTab)
                    {
                        int const specNo = int(sPlayerbotAIConfig.randomClassSpecIndex[hen->getClass()][uint32(intendedTab)]);
                        PlayerbotFactory::InitTalentsBySpecNo(hen, specNo, /*reset=*/true);
                        hen->SendTalentsInfoData(false);
                        // Randomize itemized gear for the tree it rolled, not the one we
                        // just restored — re-roll equipment so the gear matches the spec
                        // (else a Ret re-roll leaves a restored Prot tank in a Ret 2H).
                        PlayerbotFactory(hen, target).InitEquipment(false, false);
                        LOG_INFO("module",
                            "[WowPsParty Henchmen] re-leveled hench guid={} restored picked spec (tree {})",
                            henchGuid.GetCounter(), intendedTab);
                    }
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
                            WowPsParty::ParseRotationString(
                                DefaultRotationForClass(cls, freshRole, DominantTalentTabLive(hen))));
                        WowPsParty::TargetModeCacheSet(guidLow,
                            freshRole == "tank" ? "loose" : "master");
                    }
                }
                else
                {
                    // In-band hire keeps the pool char's LEVEL and TALENTS — but its
                    // gear may have been itemized for a DIFFERENT spec (e.g. a Prot
                    // paladin pool char found wearing healer cloth + spellpower). The
                    // out-of-band path above re-rolls gear via Randomize; an in-band
                    // hire never did, so the mismatched loadout survived. Re-roll just
                    // the EQUIPMENT against the bot's actual spec — the same stat-
                    // weighted picker the factory uses — so the gear matches what the
                    // henchman is. Non-incremental: it replaces each slot with the best
                    // spec item (moving the old one to bags, which ClearHenchmanInventory
                    // below purges) and leaves a slot untouched only if nothing fits.
                    PlayerbotFactory factory(hen, hen->GetLevel());
                    factory.InitEquipment(false, false);
                    hen->SaveToDB(false, false);
                    LOG_INFO("module",
                        "[WowPsParty Henchmen] in-band hire guid={} re-rolled gear to match spec",
                        henchGuid.GetCounter());
                }
            }

            // A TANK/HEALER hire must actually BE that spec. A pool char picked for the
            // slot can spawn DPS: a warrior with no readable talents is shown (and hired)
            // as tank by the class default, then fights Arms (Kevin: "arms warrior with
            // the tank role"); an out-of-band Randomize can also roll the wrong tree. Force
            // the role's tree so the henchman performs the job. DPS roles take any DPS spec
            // (DesiredTalentTabForRole returns -1 → no force), so a manually-picked DPS is
            // never re-specced. Re-rolls gear for the new spec + rebuilds the role rotation;
            // the SanitizeHenchmanSpells below then strips any off-spec leftovers.
            if (useRole == "tank" || useRole == "healer")
            {
                int const wantTab = DesiredTalentTabForRole(cls, useRole);
                if (wantTab >= 0 && DominantTalentTabLive(hen) != wantTab)
                {
                    int const specNo = int(sPlayerbotAIConfig.randomClassSpecIndex[cls][uint32(wantTab)]);
                    PlayerbotFactory::InitTalentsBySpecNo(hen, specNo, /*reset=*/true);
                    hen->SendTalentsInfoData(false);
                    PlayerbotFactory(hen, hen->GetLevel()).InitEquipment(false, false);
                    hen->SaveToDB(false, false);
                    WowPsParty::SetHenchmanRole(henchGuid, useRole);
                    WowPsParty::RotationCacheSet(henchGuid.GetCounter(),
                        WowPsParty::ParseRotationString(
                            DefaultRotationForClass(cls, useRole, DominantTalentTabLive(hen))));
                    WowPsParty::TargetModeCacheSet(henchGuid.GetCounter(),
                        useRole == "tank" ? "loose" : "master");
                    LOG_INFO("module",
                        "[WowPsParty Henchmen] guid={} forced to {} spec (tree {}) to match its hired role",
                        henchGuid.GetCounter(), useRole, wantTab);
                }
            }

            // Start the henchman with clean bags (the bot-pool char may carry
            // junk; Randomize above only refreshes EQUIPPED gear). Keeps ammo /
            // reagents / shards + equipped gear; henchmen only.
            ClearHenchmanInventory(hen);

            // Strip spells/ranks the bot kept from a former higher level (talent
            // abilities whose talent it no longer has, over-level ranks) so a
            // down-leveled or previously-bloated pool pick fights at its real
            // level. Deterministic here for both in-band and re-leveled hires;
            // the upkeep tick re-checks as a safety net.
            SanitizeHenchmanSpellsIfNeeded(hen);

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
                if (!g->AddMember(hen))
                {
                    // Group genuinely full (a 40-member raid, or a party that was
                    // never converted to a raid) — never leave a spawned henchman
                    // orphaned outside the group. Undo the hire: despawn + refund.
                    WowPsParty::DismissHenchmanByGuid(henchGuid);
                    if (refundAmt)
                        lead->ModifyMoney(int32(refundAmt));
                    if (lead->GetSession())
                        ChatHandler(lead->GetSession()).PSendSysMessage(
                            "|cffff5555[WowPsParty]|r Group is full — couldn't add the henchman. Refunded.");
                    LOG_WARN("module",
                        "[WowPsParty Henchmen] AddMember FAILED hench_guid={} (group full) — despawned + refunded {}",
                        henchGuid.GetCounter(), cost);
                    return;
                }
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

    // Destroy a HENCHMAN's loose BAG items, keeping only what it needs to keep
    // functioning. HENCHMEN ONLY — hard-guarded by IsHenchman so it can NEVER run
    // on the human player or an enrolled alt-bot (those have henchman=false in the
    // directive). Equipped gear (weapon/armor/wand) and the bag containers
    // themselves are never touched — we only sweep the GENERAL inventory (backpack
    // + the slots INSIDE equipped bags), never the equip slots or bag slots. Keeps
    // henchman bags from filling with bot-pool junk and leaves a clean slate on
    // dismiss, while a hunter keeps its ammo and a caster its reagents/shards.
    void ClearHenchmanInventory(Player* hen)
    {
        if (!hen) return;
        if (!WowPsParty::IsHenchman(hen->GetGUID())) return;   // SAFETY: henchmen only

        auto const keep = [](Item* it) -> bool
        {
            ItemTemplate const* t = it ? it->GetTemplate() : nullptr;
            if (!t) return false;
            if (t->Class == ITEM_CLASS_PROJECTILE) return true;  // arrows / bullets
            if (t->Class == ITEM_CLASS_REAGENT)    return true;  // spell reagents
            if (t->ItemId == 6265)                 return true;  // Soul Shard (pet/HS/SS)
            return false;
        };

        uint32 destroyed = 0;
        // Backpack general slots.
        for (uint8 s = INVENTORY_SLOT_ITEM_START; s < INVENTORY_SLOT_ITEM_END; ++s)
            if (Item* it = hen->GetItemByPos(INVENTORY_SLOT_BAG_0, s))
                if (!keep(it)) { hen->DestroyItem(INVENTORY_SLOT_BAG_0, s, true); ++destroyed; }
        // Slots inside each equipped bag (the bag container itself is left alone).
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            Bag* container = hen->GetBagByPos(bag);
            if (!container) continue;
            for (uint32 s = 0; s < container->GetBagSize(); ++s)
                if (Item* it = container->GetItemByPos(uint8(s)))
                    if (!keep(it)) { hen->DestroyItem(bag, uint8(s), true); ++destroyed; }
        }
        if (destroyed)
        {
            hen->SaveToDB(false, false);
            LOG_INFO("module",
                "[WowPsParty Henchmen] cleared {} bag item(s) from hench_guid={} "
                "(kept ammo/reagents/shards + equipped gear)",
                destroyed, hen->GetGUID().GetCounter());
        }
    }

    // Wipe a dismissed HENCHMAN's saved loadout (rotation / target mode / lead /
    // wait-threat / safe-pull / talents) so the next player to hire that random-pool
    // bot gets the impersonal DEFAULT, not the previous hirer's manual tweaks.
    // Henchmen aren't personal — their customisations must not stick between hires.
    //
    // SAFETY — this must NEVER touch a hero/alt's loadout, so it is DOUBLE-guarded:
    //   (1) the directive must mark the guid a henchman (heroes are henchman=false);
    //   (2) the guid must NOT be an enrolled account_party alt — a hero is ALWAYS
    //       enrolled and a henchman (a random-pool bot) NEVER is, so a row in
    //       account_party means "this is a hero, abort and keep its rotation".
    // If EITHER guard fails, nothing is deleted. Call BEFORE RemoveFollower so the
    // directive (guard 1) still exists.
    void ClearHenchmanLoadout(ObjectGuid henchGuid)
    {
        if (!WowPsParty::IsHenchman(henchGuid)) return;          // guard 1: directive
        uint32 const guid = henchGuid.GetCounter();
        if (CharacterDatabase.Query(
                "SELECT 1 FROM `account_party` WHERE `guid` = {}", guid))
        {
            LOG_ERROR("module",
                "[WowPsParty Henchmen] REFUSED to clear loadout for guid={}: it is an "
                "enrolled party alt, not a henchman — keeping its rotation intact", guid);
            return;                                              // guard 2: not enrolled
        }
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append("DELETE FROM `party_loadout` WHERE `guid` = {}", guid);
        CharacterDatabase.DirectCommitTransaction(tx);   // sync: gone before any re-hire reads it
        LOG_INFO("module",
            "[WowPsParty Henchmen] cleared saved loadout for dismissed hench_guid={}", guid);
    }

    void DismissHenchman(Player* requester, uint32 henchGuid)
    {
        if (!requester || !requester->GetSession()) return;
        ObjectGuid const g = ObjectGuid::Create<HighGuid::Player>(henchGuid);
        if (!WowPsParty::IsHenchman(g)) return;   // only dismiss henchmen
        // Record BEFORE RemoveFollower so the TellMaster guard still silences the
        // framework's farewell whisper fired from LogoutPlayerBot below (by then the
        // henchman registration is gone and IsHenchman would read false).
        WowPsParty::MarkHenchmanRecentlyDismissed(g);
        // Clear its bags (keep ammo/reagents/shards) AND its saved loadout BEFORE
        // RemoveFollower drops the directive — both guards read that directive, so
        // it must still exist or they silently no-op.
        if (Player* hen = ObjectAccessor::FindConnectedPlayer(g))
            ClearHenchmanInventory(hen);
        ClearHenchmanLoadout(g);   // reset saved rotation/toggles so it never follows the guid to the next hirer
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
        // Record BEFORE RemoveFollower so the TellMaster guard still silences the
        // framework's farewell whisper. Here the logout (and its goodbye) is deferred
        // 200ms, well after the henchman registration is torn down — the memo's TTL
        // covers that window.
        WowPsParty::MarkHenchmanRecentlyDismissed(henchGuid);
        // Clear its bags (keep ammo/reagents/shards) BEFORE RemoveFollower drops
        // the directive — the clear's IsHenchman guard needs it. The clear is pure
        // bag-item destruction; only the LOGOUT below needs the 200ms defer.
        if (Player* hen = ObjectAccessor::FindConnectedPlayer(henchGuid))
            ClearHenchmanInventory(hen);
        ClearHenchmanLoadout(henchGuid);   // reset saved rotation/toggles (henchmen aren't personal)
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
                if (hen->GetGroup()) hen->RemoveFromGroup();   // bags already cleared above
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

    // ===== "Fill party randomly" (hire-screen button) =======================
    // Hire random-pool henchmen to complete the requester's party to a balanced
    // 1-tank / 1-healer / 3-dps of 5, choosing bot roles from the roles the CURRENT
    // party members have set. Billed as ONE transaction with the same 15% discount
    // as the LFG party-fill (each underlying hire skips its own charge).
    //
    // Cross-account correctness (the "Innervate" gotcha): a second human in the WoW
    // group is on a DIFFERENT account, so their party role lives in THEIR
    // account_party row — not the requester's. Counting roles from only the
    // requester's account would misread that player as 'dps' (the same class of bug
    // that once made role-targeted casts like Beacon/Innervate skip the other
    // human). So every present member's role is resolved from the account that OWNS
    // them: the in-memory follow directive first (covers a henchman + a leader),
    // else a COALESCE over the owning account's account_party / party_loadout.
    bool FillPartyRandomly(Player* requester, std::string& outMsg)
    {
        if (!requester || !requester->GetSession())
        { outMsg = "No session."; return false; }
        if (!IsEnabled())
        { outMsg = "Party features are disabled."; return false; }

        // Present party = our account roster (leader + our bots) folded with the WoW
        // group (a second human on another account + their bots), deduped.
        std::vector<ObjectGuid> roster;
        GetPartyGuidsFor(requester->GetGUID(), roster);
        auto addUnique = [&](ObjectGuid g)
        {
            if (g && std::find(roster.begin(), roster.end(), g) == roster.end())
                roster.push_back(g);
        };
        addUnique(requester->GetGUID());
        if (Group* grp = requester->GetGroup())
            for (GroupReference* it = grp->GetFirstMember(); it; it = it->next())
                if (Player* m = it->GetSource())
                    addUnique(m->GetGUID());

        // Keep only members actually occupying a slot right now (in-world, same map).
        std::vector<ObjectGuid> present;
        for (ObjectGuid const& g : roster)
            if (Player* p = ObjectAccessor::FindConnectedPlayer(g))
                if (p->IsInWorld() && p->GetMapId() == requester->GetMapId())
                    present.push_back(g);

        uint8 const partySize = uint8(present.size());
        if (partySize >= 5)
        { outMsg = "Your party is already full."; return false; }
        uint8 const freeSlots = uint8(5 - partySize);

        // Owning-account role lookup (fallback for a member with no in-memory
        // directive — a grouped human who hasn't set up a party of their own).
        std::string ids;
        for (ObjectGuid const& g : present)
        { if (!ids.empty()) ids += ','; ids += std::to_string(g.GetCounter()); }
        std::unordered_map<uint32, std::string> dbRole;
        if (!ids.empty())
            if (QueryResult q = CharacterDatabase.Query(
                    "SELECT c.`guid`, COALESCE(ap.`role`, NULLIF(pl.`role`, ''), 'dps') "
                    "FROM `characters` c "
                    "LEFT JOIN `account_party` ap ON ap.`guid` = c.`guid` AND ap.`account` = c.`account` "
                    "LEFT JOIN `party_loadout` pl ON pl.`guid` = c.`guid` "
                    "WHERE c.`guid` IN ({})", ids))
                do { Field* f = q->Fetch(); dbRole[f[0].Get<uint32>()] = f[1].Get<std::string>(); }
                while (q->NextRow());

        uint8 haveTank = 0, haveHeal = 0;   // dps is implied (everything else)
        for (ObjectGuid const& g : present)
        {
            std::string role = RoleForGuid(g);              // directive / leader role
            if (role.empty())
            {
                auto it = dbRole.find(g.GetCounter());
                role = (it != dbRole.end()) ? it->second : std::string("dps");
            }
            if (role == "tank")        ++haveTank;
            else if (role == "healer") ++haveHeal;
        }

        // Fill priority: guarantee a tank, then a healer, rest dps — up to freeSlots.
        uint8 needTank = std::min<uint8>((haveTank == 0) ? 1 : 0, freeSlots);
        uint8 needHeal = std::min<uint8>((haveHeal == 0) ? 1 : 0, uint8(freeSlots - needTank));
        uint8 needDps  = uint8(freeSlots - needTank - needHeal);

        // Bucket the (already RAND()-ordered) candidate pool by role.
        std::vector<HenchmanCandidate> const cands = BuildHenchmanCandidates(requester);
        std::vector<HenchmanCandidate> tanks, heals, dps;
        for (auto const& c : cands)
        {
            if (c.role == "tank")        tanks.push_back(c);
            else if (c.role == "healer") heals.push_back(c);
            else                         dps.push_back(c);
        }

        // Float one candidate of each distinct class to the front so multi-hires
        // (the dps) come out varied when the pool allows; the pool is already
        // randomised, so this only diversifies, it doesn't re-order deterministically.
        auto orderForVariety = [](std::vector<HenchmanCandidate> const& bucket) -> std::vector<HenchmanCandidate>
        {
            std::vector<HenchmanCandidate> out;
            bool usedCls[16] = { false };
            for (auto const& c : bucket)
                if (c.cls < 16 && !usedCls[c.cls]) { usedCls[c.cls] = true; out.push_back(c); }
            for (auto const& c : bucket)
                if (std::find_if(out.begin(), out.end(),
                        [&](HenchmanCandidate const& x) { return x.guid == c.guid; }) == out.end())
                    out.push_back(c);
            return out;
        };
        std::vector<HenchmanCandidate> const oT = orderForVariety(tanks);
        std::vector<HenchmanCandidate> const oH = orderForVariety(heals);
        std::vector<HenchmanCandidate> const oD = orderForVariety(dps);

        // Best-effort: if the pool can't supply a needed tank/healer, roll that slot
        // into an extra dps so the free slots still get filled.
        uint8 const wantT = std::min<uint8>(needTank, uint8(oT.size()));
        uint8 const wantH = std::min<uint8>(needHeal, uint8(oH.size()));
        uint8 const shortfall = uint8((needTank - wantT) + (needHeal - wantH));
        uint8 const wantD = std::min<uint8>(uint8(needDps + shortfall), uint8(oD.size()));
        if (uint8(wantT + wantH + wantD) == 0)
        { outMsg = "No adventurers are available to fill your party right now."; return false; }

        auto goldStr = [](uint32 copper) -> std::string
        {
            uint32 const g = copper / 10000, s = (copper % 10000) / 100, c = copper % 100;
            std::string o;
            if (g) o += std::to_string(g) + "g ";
            if (g || s) o += std::to_string(s) + "s ";
            o += std::to_string(c) + "c";
            return o;
        };

        // Pre-flight affordability against the intended (max) discounted cost.
        auto sumCost = [](std::vector<HenchmanCandidate> const& pool, uint8 n) -> uint64
        { uint64 s = 0; for (uint8 i = 0; i < n && i < pool.size(); ++i) s += HenchmanHireCost(pool[i].level); return s; };
        uint64 const gross   = sumCost(oT, wantT) + sumCost(oH, wantH) + sumCost(oD, wantD);
        uint32 const maxCost = uint32((gross * 85 + 50) / 100);   // 15% off, round half-up (matches LFG fill)
        if (requester->GetMoney() < maxCost)
        { outMsg = "Not enough gold to fill your party (need " + goldStr(maxCost) + ")."; return false; }

        // Hire (each skips its own charge); bill the discounted ACTUAL total, so a
        // partial fill (pool raced out) only charges for what was hired.
        uint64 spent = 0;
        auto doHire = [&](std::vector<HenchmanCandidate> const& pool, char const* role, uint8 count) -> uint8
        {
            uint8 got = 0;
            for (auto const& c : pool)
            {
                if (got >= count) break;
                std::string m;
                if (HireHenchman(requester, c.guid, role, m, /*skipCharge=*/true))
                { ++got; spent += HenchmanHireCost(c.level); }
            }
            return got;
        };
        uint8 const gotT = doHire(oT, "tank",   wantT);
        uint8 const gotH = doHire(oH, "healer", wantH);
        uint8 const gotD = doHire(oD, "dps",    wantD);
        if (uint8(gotT + gotH + gotD) == 0)
        { outMsg = "No adventurers were available to fill your party."; return false; }

        // Bill the discounted ACTUAL total, but never above the pre-flighted
        // maxCost — a hire race can substitute a pricier candidate for a costed
        // one, and the affordability check only cleared maxCost.
        uint32 const charge = std::min<uint32>(uint32((spent * 85 + 50) / 100), maxCost);
        requester->ModifyMoney(-int32(charge));

        std::string list;
        auto part = [&](uint8 n, char const* label)
        { if (!n) return; if (!list.empty()) list += ", "; list += std::to_string(uint32(n)) + " " + label; };
        part(gotT, "tank"); part(gotH, "healer"); part(gotD, "dps");
        outMsg = "Filled your party — hired " + list + " for " + goldStr(charge) + " (15% off).";

        LOG_INFO("module",
            "[WowPsParty Fill] guid={} partySize={} hired t={}/h={}/d={} charge={}",
            requester->GetGUID().GetCounter(), uint32(partySize),
            uint32(gotT), uint32(gotH), uint32(gotD), charge);
        return true;
    }

    // ===== Hired alts =======================================================
    // The player's OWN account characters, hired as temporary follower bots. They
    // run our follow + rotation engine like any party bot, but — unlike henchmen —
    // their character is NEVER mutated: no gear/level/inventory change on hire, no
    // bag-clear or loadout-wipe on dismiss. They keep whatever they had equipped,
    // their saved rotation, and any loot they pick up. Free to hire.

    // Despawn a hired alt identified only by guid — used by the group-removal hook
    // so leaving the party (addon Dismiss or the stock WoW UI) stops it following
    // and logs it out, SAVED AS-IS. Mirrors DismissHenchmanByGuid but performs NO
    // bag/loadout clearing. Drops the directive now, defers the logout one tick.
    void DismissHiredAltByGuid(ObjectGuid altGuid)
    {
        if (!WowPsParty::IsHiredAlt(altGuid)) return;
        ObjectGuid const leaderGuid = WowPsParty::GetLeaderFor(altGuid);
        WowPsParty::MarkHenchmanRecentlyDismissed(altGuid);  // silence the framework farewell whisper
        WowPsParty::RemoveFollower(altGuid);
        WowPsParty::RotationCacheClear(altGuid.GetCounter());   // in-memory only; DB loadout kept
        LOG_INFO("module",
            "[WowPsParty Alts] hired alt left party — dismissing alt_guid={}",
            altGuid.GetCounter());
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader) return;
        leader->m_Events.AddEventAtOffset([leaderGuid, altGuid]()
        {
            Player* l = ObjectAccessor::FindConnectedPlayer(leaderGuid);
            if (!l) return;
            if (Player* alt = ObjectAccessor::FindConnectedPlayer(altGuid))
            {
                if (alt->GetGroup()) alt->RemoveFromGroup();
                alt->SaveToDB(false, false);   // persist any loot before the bot logs out
            }
            if (PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(l))
                mgr->LogoutPlayerBot(altGuid);
            UpdateGroupLootForHenchmen(l);
        }, std::chrono::milliseconds(200));
    }

    void DismissHiredAlt(Player* requester, uint32 altGuid)
    {
        if (!requester || !requester->GetSession()) return;
        ObjectGuid const g = ObjectGuid::Create<HighGuid::Player>(altGuid);
        if (!WowPsParty::IsHiredAlt(g)) return;   // only dismiss hired alts
        WowPsParty::MarkHenchmanRecentlyDismissed(g);
        WowPsParty::RemoveFollower(g);
        WowPsParty::RotationCacheClear(altGuid);   // in-memory only; the saved loadout in the DB stays
        if (Player* alt = ObjectAccessor::FindConnectedPlayer(g))
        {
            if (alt->GetGroup()) alt->RemoveFromGroup();
            alt->SaveToDB(false, false);   // persist any loot before logout
        }
        if (PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(requester))
            mgr->LogoutPlayerBot(g);   // returns the char to offline, keeping its bags/gear
        UpdateGroupLootForHenchmen(requester);
        LOG_INFO("module", "[WowPsParty Alts] DISMISS alt_guid={}", altGuid);
    }

    void DismissAllHiredAlts(Player* requester)
    {
        if (!requester) return;
        std::vector<ObjectGuid> alts;
        {
            std::vector<ObjectGuid> guids;
            WowPsParty::GetPartyGuidsFor(requester->GetGUID(), guids);
            for (ObjectGuid const& gg : guids)
                if (WowPsParty::IsHiredAlt(gg)) alts.push_back(gg);
        }
        for (ObjectGuid const& gg : alts)
            DismissHiredAlt(requester, gg.GetCounter());
    }

    // Every non-enrolled own-account character (minus the active session char),
    // flagged whether it's currently hired. Offline un-hired rows are hireable;
    // a hired row (online as a bot) is shown so it can be dismissed.
    std::vector<AltCandidate> BuildAltCandidates(Player* requester)
    {
        std::vector<AltCandidate> out;
        if (!requester || !requester->GetSession()) return out;
        uint32 const account    = requester->GetSession()->GetAccountId();
        uint32 const activeGuid = requester->GetGUID().GetCounter();

        QueryResult q = CharacterDatabase.Query(
            "SELECT c.`guid`, c.`name`, c.`class`, c.`level`, c.`online` "
            "FROM `characters` c "
            "LEFT JOIN `account_party` ap ON ap.`guid` = c.`guid` "
            "WHERE c.`account` = {} AND ap.`guid` IS NULL "
            "ORDER BY (c.`roster_order` IS NULL) ASC, c.`roster_order` ASC, c.`name` ASC",
            account);
        if (!q) return out;
        do
        {
            Field* f = q->Fetch();
            uint32 const guid = f[0].Get<uint32>();
            if (guid == activeGuid) continue;   // can't hire the character you're playing
            uint8 const cls    = f[2].Get<uint8>();
            bool  const online = f[4].Get<uint8>() != 0;
            ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
            bool const hired   = WowPsParty::IsHiredAlt(og);
            // Hireable rows are offline; a currently-hired alt is online (as a bot)
            // and listed so it can be dismissed. Skip an online char that isn't our
            // hired alt (shouldn't happen on a single-session account — defensive).
            if (online && !hired) continue;

            AltCandidate c;
            c.guid  = guid;
            c.name  = f[1].Get<std::string>();
            c.cls   = cls;
            c.level = f[3].Get<uint8>();
            c.hired = hired;
            InferHenchmanRoleAndSpec(guid, cls, ClassDefaultRole(cls), c.role, c.spec);
            out.push_back(std::move(c));
        } while (q->NextRow());
        return out;
    }

    bool HireAlt(Player* requester, uint32 altGuid, std::string& outMsg)
    {
        if (!requester || !requester->GetSession())
        { outMsg = "No session."; return false; }
        uint32 const account = requester->GetSession()->GetAccountId();

        if (altGuid == requester->GetGUID().GetCounter())
        { outMsg = "You can't hire the character you're playing."; return false; }

        // Validate: own-account char, exists, offline, not enrolled.
        QueryResult q = CharacterDatabase.Query(
            "SELECT c.`level`, c.`online`, c.`account`, c.`class`, "
            "EXISTS(SELECT 1 FROM `account_party` ap WHERE ap.`guid` = c.`guid`) "
            "FROM `characters` c WHERE c.`guid` = {}", altGuid);
        if (!q) { outMsg = "Character not found."; return false; }
        Field* f = q->Fetch();
        uint8 const level        = f[0].Get<uint8>();
        bool  const online       = f[1].Get<uint8>() != 0;
        uint32 const charAccount = f[2].Get<uint32>();
        uint8 const cls          = f[3].Get<uint8>();
        bool  const enrolled     = f[4].Get<uint32>() != 0;
        if (charAccount != account) { outMsg = "That isn't one of your characters."; return false; }
        if (enrolled) { outMsg = "That character is already in your party."; return false; }
        if (online)   { outMsg = "That character is busy — log it out first."; return false; }

        // Party-space cap. Hired alts, henchmen and enrolled heroes all count as
        // followers toward the leader+4 / raid-of-40 cap (same gate as HireHenchman).
        Group* const reqGroup  = requester->GetGroup();
        bool   const inRaid    = reqGroup && reqGroup->isRaidGroup();
        uint32 const companionCap = inRaid ? 39u : 4u;
        uint32 const followers    = WowPsParty::CountFollowersFor(requester->GetGUID());
        if (followers >= companionCap
            || (reqGroup && reqGroup->GetMembersCount() >= (inRaid ? 40u : 5u)))
        {
            outMsg = inRaid
                ? "Your raid is full."
                : "Your party is full (4 companions). Convert your group to a raid to add more.";
            return false;
        }

        PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(requester);
        if (!mgr) { outMsg = "Bot manager not ready — try again in a moment."; return false; }

        // Role from the alt's OWN talents — drives targeting mode + lead-tank pick.
        std::string const role = InferHenchmanRole(altGuid, cls, ClassDefaultRole(cls));

        ObjectGuid const altObjGuid = ObjectGuid::Create<HighGuid::Player>(altGuid);
        // Register BEFORE spawning so the patched AddPlayerBot permission check
        // (WowPsParty_IsManagedBotSpawn_Trampoline) lets the bot in.
        WowPsParty::AddHiredAltDirective(account, altObjGuid, requester->GetGUID(), role);

        // Load the alt's PERSISTED rotation + loadout into the runtime caches — the
        // SAME set OnActiveLogin loads for an enrolled alt. Crucially, nothing is
        // reset to a class default (the henchman hire path does): the alt runs the
        // exact rotation/toggles the player saved for it before. The COMMON shared
        // rotation is account-wide (already loaded at login) and applies on top.
        RotationCacheRefreshFromDB(altGuid);
        TargetModeRefreshFromDB(altGuid);
        LeadDungeonRefreshFromDB(altGuid);
        WaitTankThreatRefreshFromDB(altGuid);
        SafePullRefreshFromDB(altGuid);
        PullCountRefreshFromDB(altGuid);
        LeadDistRefreshFromDB(altGuid);
        EngageRangeRefreshFromDB(altGuid);
        AnchorTankRefreshFromDB(altGuid);
        FollowPathRefreshFromDB(altGuid);

        mgr->AddPlayerBot(altObjGuid, account);

        // Spawn is async (login query holder). After a short delay: if it arrived,
        // make sure it's grouped + set the loot mode; if it never arrived, drop the
        // directive so we never leak one. Free hire — nothing to refund.
        ObjectGuid const leaderGuid = requester->GetGUID();
        requester->m_Events.AddEventAtOffset([leaderGuid, altObjGuid]()
        {
            Player* lead = ObjectAccessor::FindConnectedPlayer(leaderGuid);
            Player* alt  = ObjectAccessor::FindConnectedPlayer(altObjGuid);
            if (!alt || !alt->IsInWorld())
            {
                WowPsParty::RemoveFollower(altObjGuid);
                if (lead && lead->GetSession())
                    ChatHandler(lead->GetSession()).PSendSysMessage(
                        "|cffff5555[WowPsParty]|r Your alt didn't arrive — try again.");
                LOG_WARN("module",
                    "[WowPsParty Alts] spawn no-show alt_guid={} (bot not in world after delay)",
                    altObjGuid.GetCounter());
                return;
            }
            if (!lead) return;

            // mod-playerbots auto-groups an own-account bot with its master, so this
            // is usually a no-op; it's the safety net for the case it didn't.
            Group* g = lead->GetGroup();
            if (!g)
            {
                g = new Group();
                if (!g->Create(lead)) { delete g; return; }
                sGroupMgr->AddGroup(g);
            }
            if (!g->IsMember(altObjGuid))
            {
                if (alt->GetGroup())
                {
                    struct RegroupGuard {
                        ObjectGuid gg;
                        ~RegroupGuard() { WowPsParty::SetHenchmanRegrouping(gg, false); }
                    } guard{altObjGuid};
                    WowPsParty::SetHenchmanRegrouping(altObjGuid, true);
                    alt->RemoveFromGroup();
                }
                if (!g->AddMember(alt))
                {
                    WowPsParty::DismissHiredAltByGuid(altObjGuid);
                    if (lead->GetSession())
                        ChatHandler(lead->GetSession()).PSendSysMessage(
                            "|cffff5555[WowPsParty]|r Group is full — couldn't add the alt.");
                    return;
                }
            }
            UpdateGroupLootForHenchmen(lead);
            if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(alt))
            {
                ai->ChangeStrategy("+loot",   BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);
            }
        }, std::chrono::seconds(8));

        LOG_INFO("module",
            "[WowPsParty Alts] HIRE account={} alt_guid={} role={} level={}",
            account, altGuid, role, uint32(level));
        outMsg = "Hired!";
        return true;
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

        // Restore a previously-assigned role: party_loadout.role outlives a kick
        // (which only deletes the account_party row), so a char set to Tank, kicked,
        // and re-invited comes back as Tank instead of resetting to dps.
        std::string savedRole = "dps";
        if (QueryResult roleQ = CharacterDatabase.Query(
                "SELECT `role` FROM `party_loadout` WHERE `guid` = {}", targetGuid))
        {
            std::string const r = roleQ->Fetch()[0].Get<std::string>();
            if (r == "tank" || r == "healer" || r == "dps")
                savedRole = r;
        }

        // Transactional insert: account_party row + characters.party_slot column.
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append(
            "INSERT INTO `account_party` (`account`, `slot`, `guid`, `role`, `is_active_on_login`) "
            "VALUES ({}, {}, {}, '{}', {})",
            requestorAccount, nextSlot, targetGuid, savedRole,
            (nextSlot == 0 ? 1u : 0u));
        tx->Append(
            "UPDATE `characters` SET `party_slot` = {} WHERE `guid` = {}",
            nextSlot, targetGuid);
        // SYNCHRONOUS commit: MGMT_INVITE calls SetActiveFollowers (which
        // rebuilds follow directives from account_party) immediately after this
        // returns. An async commit isn't visible to that synchronous re-query
        // yet, so the just-enrolled member was missing from the directive set —
        // it then spawned WITHOUT a directive and ran mod-playerbots' default AI
        // (e.g. a priest self-casting Power Word: Shield) instead of its rotation.
        CharacterDatabase.DirectCommitTransaction(tx);

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
        // SYNCHRONOUS commit so the SetActiveFollowers rebuild that the kick
        // handler runs right after sees the row already gone (same async-commit
        // visibility race the enroll path hit).
        CharacterDatabase.DirectCommitTransaction(tx);

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
            // Load the account's COMMON shared rotation once (it's account-wide, not
            // per-bot) — prepended to every bot's rules in TickRotation.
            WowPsParty::SharedRotationRefreshFromDB(account);

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
                WaitTankThreatRefreshFromDB(guid);
                SafePullRefreshFromDB(guid);
                PullCountRefreshFromDB(guid);
                LeadDistRefreshFromDB(guid);
                EngageRangeRefreshFromDB(guid);
                AnchorTankRefreshFromDB(guid);
                FollowPathRefreshFromDB(guid);
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
                {
                    if (slot.guid == leader->GetGUID()) continue;
                    if (enrolled.count(slot.guid.GetCounter())) continue;  // our own alt
                    // Only purge OFFLINE stragglers — a stale henchman from a past
                    // session that the saved group still lists. A CONNECTED non-
                    // enrolled member is either a henchman hired this session or a
                    // real player who grouped up with us (a friend on another
                    // account); never kick those, or co-op play breaks.
                    if (ObjectAccessor::FindConnectedPlayer(slot.guid)) continue;
                    toRemove.push_back(slot.guid);
                }
                for (ObjectGuid const& g : toRemove)
                {
                    // RemoveMember calls BroadcastGroupUpdate immediately and
                    // DISBANDS + deletes the group once it drops below 2 members,
                    // so the captured `existing` can dangle mid-loop — calling
                    // RemoveMember on the freed object crashes (use-after-free in
                    // BroadcastGroupUpdate walking freed member slots). Re-fetch
                    // from the leader each pass and stop the instant the group is
                    // gone. (Exposed by raid-size parties: many stale offline
                    // henchmen to purge => the disband can land before the last.)
                    Group* grp = leader->GetGroup();
                    if (!grp)
                        break;
                    grp->RemoveMember(g);
                    LOG_INFO("module",
                        "[WowPsParty] login purge: removed stale offline member guid={} "
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
            // (No login quest-reconcile here. An earlier version force-"completed"
            // any quest a PEER had already rewarded — which destroyed the active
            // questline of a low-level char catching up through content the
            // higher-level alts had long finished, e.g. Nisseanderz losing his
            // Goldshire quests. The live turn-in mirror's ForceCompleteTurnIn
            // fallback already prevents the original full-bags stuck-quest bug at
            // the source; a blanket login reconcile is too blunt and was removed.)
        }, std::chrono::seconds(6));

        LOG_INFO("module",
                 "[WowPsParty] OnActiveLogin: account={} active_guid={} -- bot spawn deferred by 1s",
                 account, activeGuid);
    }

}
