/*
 * WowPs Party-of-5 mod — PartyMgr singleton
 *
 * Authoritative store + lifecycle manager for the per-account 5-character party.
 * - DB shape lives in `account_party` (account, slot 0..4, guid, is_active_on_login)
 *   plus `characters.party_slot` (denormalized for fast login-time lookup).
 * - On active login: looks up the rest of the account's party and asks mod-playerbots
 *   to spawn each remaining member as a server-controlled bot in the active player's
 *   party, and loads each member's saved `party_loadout` row (rotation + behaviour
 *   toggles) into the runtime caches via RefreshMemberLoadoutCaches. The follow and
 *   rotation engines read ONLY those caches, so the hire and roster-invite paths run
 *   the same load — a member spawned without it fights with no rules.
 * - On active logout: mod-playerbots handles cascade cleanup when the master goes
 *   away (PlayerbotMgr is destructed with its owning Player), so we deliberately
 *   don't double-free.
 */

#ifndef WOWPSPARTY_PARTYMGR_H
#define WOWPSPARTY_PARTYMGR_H

#include "Define.h"
#include "ObjectGuid.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

class Player;
class Item;
struct Loot;
struct LootItem;
struct ItemTemplate;

namespace WowPsParty
{
    constexpr uint8 PARTY_SIZE = 5;

    struct PartyMember
    {
        uint32 guid   = 0;
        uint8  slot   = 0;        // 0..4 within the owning account's party
        std::string name;         // populated by GetParty() for display
        uint8  classId = 0;       // populated by GetParty() for display
        uint8  level   = 0;       // populated by GetParty() for display
        bool   online  = false;   // populated by GetParty() — actively in world right now
    };

    enum class EnrollResult
    {
        Ok,
        AlreadyEnrolled,        // target is already in this account's party
        TakenByAnotherAccount,  // target guid is in another account's party (shouldn't happen — UNIQUE constraint)
        PartyFull,              // 5 slots already used
        TargetNotFound,         // no character by that name on the requestor's account
        DatabaseError,
    };

    // Session-independent bootstrap (defined in PartyAddonProtocol.cpp).
    // Enrolls every char on the account that isn't already enrolled, into
    // free party slots (up to 5 total). Returns number enrolled. `messages`
    // is filled with human-readable lines per step. Same code path the WPSP
    // BOOTSTRAP_PARTY handler invokes — exposed here so admin/test commands
    // can drive it without a live player session.
    uint32 BootstrapPartyForAccount(uint32 account, std::vector<std::string>& messages);

    // Tell the real client watching `changed`'s party that the shared bag view is
    // stale. The addon decides whether its inventory frame is open before asking
    // for the full inventory stream.
    void NotifyPartyInventoryChanged(Player* changed);

    // Per-account feature toggles. The same install can run full Party-of-5
    // (all ON, the default) or normal solo play (companions off, normal bags,
    // no shared progression). Persisted in `party_account_settings`.
    struct PartySettings
    {
        bool spawnCompanions   = true;  // spawn the 4 enrolled alts as bots
        bool sharedInventory   = true;  // CLIENT: B opens the merged party grid
        bool sharedGear        = true;  // CLIENT: C opens the party gear panel
        bool sharedProgression = true;  // SERVER: mirror XP / gold / loot / quests
        uint32 questXpRate     = 200;   // % XP from quest turn-ins (clamped 100-500, default x2)
        uint32 killXpRate      = 200;   // % XP from kills           (clamped 100-500, default x2)
        bool lfgAutofillOptOut = false; // player chose "Don't ask again" to the LFG party-fill offer
    };

    // Allowed bounds for the per-account XP multipliers (percent).
    static constexpr uint32 XP_RATE_MIN = 100;
    static constexpr uint32 XP_RATE_MAX = 500;

    // Cached read; all-ON default for an account with no row yet.
    PartySettings GetAccountSettings(uint32 account);
    // Persist one toggle. key ∈ {spawn_companions, shared_inventory,
    // shared_gear, shared_progression}. Updates the DB and the cache.
    void SetAccountSetting(uint32 account, std::string const& key, bool value);
    // Persist a per-account XP rate (percent, clamped to [XP_RATE_MIN, XP_RATE_MAX]).
    // quest=true sets the quest-turn-in rate, false the kill rate. DB + cache.
    void SetAccountXpRate(uint32 account, bool quest, uint32 rate);
    void AccountSettingsRefreshFromDB(uint32 account);
    void EnsureSettingsTable();   // CREATE TABLE IF NOT EXISTS — call on startup
    void EnsureRosterOrderColumn();  // ADD characters.roster_order if missing — call on startup

    // ----- Loot blacklist ----------------------------------------------------
    // Item entries the account's party refuses. Every path that puts DROPPED loot
    // into the shared bags consults it, so a listed item never reaches a companion:
    // the kill auto-loot pass, the henchman's own corpse loot, and the spread that
    // redirects a hand-looted item to whichever hero has room. Persisted in
    // `party_loot_blacklist`, cached per account like the toggles above.
    bool LootBlacklisted(uint32 account, uint32 itemEntry);
    // Both return false when the row was already in (or already absent from) the list.
    bool AddLootBlacklist(uint32 account, uint32 itemEntry);
    bool RemoveLootBlacklist(uint32 account, uint32 itemEntry);
    // Ascending item entries — the order the panel lists them in.
    std::vector<uint32> GetLootBlacklist(uint32 account);
    void LootBlacklistRefreshFromDB(uint32 account);
    void EnsureLootBlacklistTable();  // CREATE TABLE IF NOT EXISTS — call on startup

    // Retire one drop from a body — into the party bags, or, for a blacklisted
    // entry, into nothing. Both mean "the party is done with this slot", so both
    // owe the corpse's is_looted + unlootedCount tally the same bookkeeping, and a
    // slot left un-retired keeps Loot::isLooted() false forever: the body never
    // loses UNIT_DYNFLAG_LOOTABLE, AllLootRemovedFromCorpse never fires, and the
    // party's skinner is refused the corpse for as long as a human stands near it.
    // Defined in PartyHooks.cpp and shared with the henchman's own corpse loot in
    // PartyFollow.cpp so the two cannot drift on the core's was-this-drop-counted
    // predicate. (Definition carries the full rationale.)
    void MarkPartyTookLootItem(Loot& loot, LootItem& li, ItemTemplate const& tmpl);

    // Load one party member's persisted `party_loadout` row (rotation + every
    // behaviour toggle) into the runtime caches. Combat reads ONLY those caches,
    // so every path that brings a saved member into an active party — login
    // spawn, alt hire, roster invite — has to run this or the member fights with
    // no rules at all while its rules sit safely on disk.
    void RefreshMemberLoadoutCaches(uint32 guidLow);

    // ----- Henchmen --------------------------------------------------------
    // GW1-style hireable bot companions, drawn from the random-bot pool. They
    // use default mod-playerbots combat AI but our follow / leash / tank-lead.
    // Temporary: released on dismiss or logout.
    struct HenchmanCandidate
    {
        uint32      guid  = 0;
        std::string name;
        uint8       cls   = 0;
        uint8       level = 0;
        std::string role;       // tank / healer / dps
        std::string spec;       // short spec abbrev (e.g. "Holy"/"Resto"); "" if none
    };

    // Up to ~11 candidates (3 tank / 2 healer / 6 dps) from offline random-pool
    // chars near the requester's level (±4, or 80 if the player is 80). At least
    // THREE distinct tank classes are guaranteed when the pool can supply them.
    std::vector<HenchmanCandidate> BuildHenchmanCandidates(Player* requester);

    // Scale every henchman in `leader`'s group into the level range shared by the
    // selected LFG `dungeons` (effective range = highest minlevel .. lowest maxlevel
    // across the set). A henchman below the range is up-leveled to the floor; above
    // it, down-leveled to the ceiling; in range it is left alone. Lets a party of
    // mixed-level henchmen queue any dungeon the human leader can, fighting at the
    // dungeon's level. No-op when the dungeons share no overlapping range.
    void ScaleHenchmenForDungeons(Player* leader, std::set<uint32> const& dungeons);

    // Copper cost to hire a henchman of the given level.
    uint32 HenchmanHireCost(uint8 level);

    // Canonical starter rotation DSL for a class id (1=Warr…11=Druid). Shared
    // by the `.party preset` command and henchman hire so henchmen run our
    // rotation engine + combat AI (positioning, LoS approach) with sensible
    // class spells, instead of the default playerbot AI. Empty for unknown.
    // `tree` is the dominant talent tab (0/1/2) the rotation is BAKED for, so a
    // multi-DPS-spec class (mage/warlock/hunter/shaman/druid/warrior/DK/rogue)
    // gets a clean spec-specific list instead of one cross-school list gated by
    // runtime `primary_tree:N`. -1 = unknown spec (pre-10 / no talents / the
    // generic `.party preset`) → a simple basic rotation for the class.
    std::string DefaultRotationForClass(uint8 cls, std::string const& role = "", int tree = -1);

    // Dominant talent tree (0/1/2) for baking a per-spec default rotation, or -1
    // when the char has no talents yet (low level). Prefers a connected bot's
    // live talents (authoritative right after a re-spec); falls back to the
    // character_talent table for an offline candidate. Public so the addon
    // protocol's Generate / apply-default paths can pick the right spec.
    int DominantTreeForGuid(uint32 guid);

    // Keep a managed bot's consumables topped up: hunters (and anyone with a
    // bow/gun) get level-appropriate ammo and never run dry; rogues get
    // level-appropriate poisons applied to their weapons. Throttled internally,
    // safe to call every rotation tick. Our bots hard-return out of
    // mod-playerbots' UpdateAI, so its own ammo/imbue maintenance never runs for
    // them — this is the replacement.
    void MaintainBotConsumables(Player* bot);

    // Destroy a bot's surplus Soul Shards, leaving exactly one. `preferKeep`, if
    // non-null, is the shard to spare — pass the item a store-hook just created
    // so its caller keeps a live pointer; otherwise the first shard found stays.
    // Bot warlocks only (the playerbot AI drains a shard per kill and never
    // spends them, flooding the non-stacking shards across bag slots).
    void TrimSoulShardsToOne(Player* bot, Item* preferKeep);

    // Hire `candidateGuid` for `requester`. Validates gold + party space,
    // deducts the fee, spawns the bot and registers the follow directive.
    // `skipCharge` skips the per-hire gold check + deduct AND the no-show/full
    // refund (the caller has already charged a single summed price — used by the
    // LFG party-fill, which charges the discounted total once up front).
    bool HireHenchman(Player* requester, uint32 candidateGuid, std::string const& role,
                      std::string& outMsg, bool skipCharge = false);

    // "Fill party randomly": hire random-pool henchmen to complete the requester's
    // party to a balanced 1-tank / 1-healer / 3-dps of 5, choosing bot roles from
    // the roles the CURRENT party members (across accounts) have set. Billed as one
    // transaction with the same 15% discount as the LFG party-fill. Sets `outMsg`
    // to a player-facing result either way; returns true if any bot was hired.
    bool FillPartyRandomly(Player* requester, std::string& outMsg);

    // Release one henchman (or all of the account's). Logs the bot out so it
    // returns to the random pool.
    void DismissHenchman(Player* requester, uint32 henchGuid);
    void DismissAllHenchmen(Player* requester);
    // Dismiss by guid (group-removal hook → uninvite = despawn).
    void DismissHenchmanByGuid(ObjectGuid henchGuid);

    // ----- Hired alts ------------------------------------------------------
    // The player's OWN account characters, NOT enrolled in account_party, hired
    // as temporary follower bots. Free; they keep their already-equipped gear,
    // talents, bags and previously-saved rotation — nothing mutates their loadout
    // — and loot to their own bags. Hidden from the party gear / talent / inventory
    // panels (those key off enrollment). Released (saved as-is) on dismiss / logout.
    struct AltCandidate
    {
        uint32      guid  = 0;
        std::string name;
        uint8       cls   = 0;
        uint8       level = 0;
        std::string role;   // tank / healer / dps (from its own talents)
        std::string spec;   // short spec abbrev; "" if none
        bool        hired = false;   // currently spawned as a hired-alt follower
    };

    // Every non-enrolled own-account character (excluding the active session char),
    // each flagged whether it is currently hired. Drives the Hire-Alts window: an
    // un-hired offline row gets a Hire button, a hired row gets a Dismiss button.
    std::vector<AltCandidate> BuildAltCandidates(Player* requester);

    // Hire `altGuid` (one of `requester`'s own offline, non-enrolled characters)
    // as a follower bot. Validates ownership + party space; spawns it AS-IS (no
    // gear / level / inventory change) and loads its persisted rotation + loadout.
    bool HireAlt(Player* requester, uint32 altGuid, std::string& outMsg);

    // Release a hired alt (one, all, or by guid). Saves the character exactly as it
    // is (keeping any loot); does NOT clear its bags or its saved loadout.
    void DismissHiredAlt(Player* requester, uint32 altGuid);
    void DismissAllHiredAlts(Player* requester);
    void DismissHiredAltByGuid(ObjectGuid altGuid);
    // Destroy a HENCHMAN's loose bag items (keeps ammo/reagents/shards + equipped
    // gear). Henchman-guarded; no-op on the player or an enrolled alt-bot. Called
    // on hire (start clean) and dismiss (leave clean) so henchman bags never fill.
    void ClearHenchmanInventory(Player* hen);

    class PartyMgr
    {
    public:
        static PartyMgr& Instance();

        // Adds targetGuid to requestor's account_party at the next free slot.
        // If targetGuid == 0, enrolls the requestor's currently-logged-in character.
        // targetName is for log/sysmessage display only.
        EnrollResult Enroll(Player* requestor, uint32 targetGuid, std::string const& targetName);

        // Removes requestor's currently-logged-in character from its account's party.
        // Returns true if a row was removed, false if the character wasn't enrolled.
        bool Leave(Player* requestor);

        // Returns every party member row for the given account, joined with the
        // characters table for name/class/level, with online status filled in.
        std::vector<PartyMember> GetParty(uint32 accountId);

        // Fast guid → slot lookup for hooks (PLAYERHOOK_ON_LOGIN etc.).
        // Reads from the DB; cheap enough to do per-login without a cache.
        std::optional<uint8> GetSlotForGuid(uint32 guid);

        // On active-character login: figure out the account's party, and ask
        // mod-playerbots to spawn the other members as bots in the active player's
        // session. Called from PartyBootstrap's PlayerScript::OnPlayerLogin hook.
        void OnActiveLogin(Player* active);

    private:
        PartyMgr() = default;
        PartyMgr(PartyMgr const&) = delete;
        PartyMgr& operator=(PartyMgr const&) = delete;
    };

    // Teach `p` every class-trainer spell it qualifies for at its current
    // level (CanTeachSpell enforces level/skill/prereqs). Returns the number
    // newly learned. Used by the .party learnall command AND the on-level-up
    // hook so the party never has to visit a trainer. Talents are untouched.
    uint32 LearnAllClassSpells(Player* p);

    // Teach `p` every WEAPON-MASTER proficiency its class+race can use (Axes,
    // Swords, Bows, Staves, …). These come from weapon-master NPCs, not the class
    // trainer, so LearnAllClassSpells never grants them — without this a fresh alt
    // has to visit a weapon master to use anything but its starter weapons. Gated
    // by IsSpellFitByClassAndRace (same check the weapon master uses), idempotent.
    uint32 LearnAllWeaponSkills(Player* p);

    // Teach `p` the class-QUEST abilities it qualifies for at its current level —
    // druid forms, warlock pet summons, paladin mounts. These come from one-off
    // class quests, not the trainer, so LearnAllClassSpells never grants them and
    // a bot (which never quests) would sit at max level with no Bear Form. Idempotent
    // (HasSpell-gated). Returns the number newly learned. Called from the bot tick,
    // the on-ding hook, .party learnall, and active-char login.
    uint32 LearnClassQuestSkills(Player* p);

    // Module on/off + the LFG party-fill offer toggle (defined in PartyBootstrap.cpp).
    bool IsEnabled();
    bool IsLfgAutofillEnabled();

    // Drop any pending LFG party-fill offer for a player (gossip awaiting a choice)
    // and despawn its recruiter NPC. Called from the logout hook so a disconnect
    // mid-offer leaks nothing. Defined in PartyLfgFill.cpp.
    void LfgFill_OnLogout(ObjectGuid guid);
}

#define sPartyMgr WowPsParty::PartyMgr::Instance()

#endif
