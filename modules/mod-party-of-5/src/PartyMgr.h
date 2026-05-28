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

    enum class SwapResult
    {
        Ok,
        InvalidSlot,            // slot out of range or empty
        TargetNotInWorld,       // the bot for the target slot isn't currently loaded
        TargetIsDead,
        AlreadyControllingTarget,
        InBattleground,         // swaps disabled while in PvP queue / battleground / arena
        VehicleSetupFailed,     // the CreateVehicleKit/EnterVehicle dance failed at the server
    };

    // Session-independent bootstrap (defined in PartyAddonProtocol.cpp).
    // Enrolls every char on the account that isn't already enrolled, into
    // free party slots (up to 5 total). Returns number enrolled. `messages`
    // is filled with human-readable lines per step. Same code path the WPSP
    // BOOTSTRAP_PARTY handler invokes — exposed here so admin/test commands
    // can drive it without a live player session.
    uint32 BootstrapPartyForAccount(uint32 account, std::vector<std::string>& messages);

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

        // Swap the player's active control to the party member at the given slot.
        // Uses CHARM_TYPE_POSSESS (same primitive Mind Control uses): camera,
        // mover, and client control all follow the target unit automatically.
        // Currently-held charms are dropped first so successive swaps work.
        SwapResult SwapTo(Player* requestor, uint8 targetSlot);

        // Drop the current possess and return control to the session's own
        // character. Recovery path when a swap leaves the player in a bad state
        // (e.g., they accidentally cast a spell from the original spellbook —
        // see the action-bar UX wrinkle that the Phase 3 addon is meant to fix).
        // Returns true if a charm was released, false if nothing to release.
        bool Unswap(Player* requestor);

        // True character-switch via logout/login chain. Avoids all the
        // CHARM_TYPE_POSSESS limitations (charmer-can't-move, possess
        // breaking on charmer motion, etc.) by actually re-binding the
        // session to a different Player object. ~2-3s loading screen per
        // swap is the cost; gain is rock-solid control transfer + the
        // new "main char" actually moves because it IS the session player.
        SwapResult SwapToViaRelogin(Player* requestor, uint8 targetSlot);

    private:
        PartyMgr() = default;
        PartyMgr(PartyMgr const&) = delete;
        PartyMgr& operator=(PartyMgr const&) = delete;
    };
}

#define sPartyMgr WowPsParty::PartyMgr::Instance()

#endif
