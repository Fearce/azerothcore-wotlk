/*
 * WowPs Party-of-5 mod — PartyMgr singleton
 *
 * Authoritative store + lifecycle manager for the per-account 5-character party.
 * - DB shape lives in `account_party` (account, slot 0..4, guid, is_active_on_login)
 *   plus `characters.party_slot` (denormalized for fast login-time lookup).
 * - On active login: looks up the rest of the account's party and asks mod-playerbots
 *   to spawn each remaining member as a server-controlled bot in the active player's
 *   party. The bots inherit mod-playerbots' default AI (follow, combat, loot) — no
 *   per-member loadout yet (that's Phase 3).
 * - On active logout: mod-playerbots handles cascade cleanup when the master goes
 *   away (PlayerbotMgr is destructed with its owning Player), so we deliberately
 *   don't double-free.
 */

#ifndef WOWPSPARTY_PARTYMGR_H
#define WOWPSPARTY_PARTYMGR_H

#include "Define.h"
#include "ObjectGuid.h"

#include <optional>
#include <string>
#include <vector>

class Player;

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
    };

    // Up to 10 candidates (2 tank / 2 healer / 6 dps) from offline random-pool
    // chars near the requester's level (±2, or 80 if the player is 80).
    std::vector<HenchmanCandidate> BuildHenchmanCandidates(Player* requester);

    // Copper cost to hire a henchman of the given level.
    uint32 HenchmanHireCost(uint8 level);

    // Canonical starter rotation DSL for a class id (1=Warr…11=Druid). Shared
    // by the `.party preset` command and henchman hire so henchmen run our
    // rotation engine + combat AI (positioning, LoS approach) with sensible
    // class spells, instead of the default playerbot AI. Empty for unknown.
    std::string DefaultRotationForClass(uint8 cls, std::string const& role = "");

    // Keep a managed bot's consumables topped up: hunters (and anyone with a
    // bow/gun) get level-appropriate ammo and never run dry; rogues get
    // level-appropriate poisons applied to their weapons. Throttled internally,
    // safe to call every rotation tick. Our bots hard-return out of
    // mod-playerbots' UpdateAI, so its own ammo/imbue maintenance never runs for
    // them — this is the replacement.
    void MaintainBotConsumables(Player* bot);

    // Hire `candidateGuid` for `requester`. Validates gold + party space,
    // deducts the fee, spawns the bot and registers the follow directive.
    bool HireHenchman(Player* requester, uint32 candidateGuid, std::string const& role,
                      std::string& outMsg);

    // Release one henchman (or all of the account's). Logs the bot out so it
    // returns to the random pool.
    void DismissHenchman(Player* requester, uint32 henchGuid);
    void DismissAllHenchmen(Player* requester);
    // Dismiss by guid (group-removal hook → uninvite = despawn).
    void DismissHenchmanByGuid(ObjectGuid henchGuid);

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
}

#define sPartyMgr WowPsParty::PartyMgr::Instance()

#endif
