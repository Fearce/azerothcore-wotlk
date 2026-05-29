/*
 * WowPs Party-of-5 — Dedicated follow system.
 *
 * Replaces mod-playerbots' FollowAction-based party follow which was
 * unreliable for our rapid-leader-change workload (every .party swap
 * changes the group leader and mod-playerbots' AI value cache for
 * "group leader" was getting stuck on the previous leader, causing bots
 * to glue to the original session player instead of the swap target).
 *
 * Architecture:
 *   - A global registry maps follower-guid -> leader-guid.
 *   - A WorldScript OnUpdate hook ticks every ~1 second.
 *   - On tick: walk the registry, for each entry call MoveFollow on the
 *     follower targeting the leader. Idempotent if already following.
 *   - On every SwapTo: SetActiveFollowers rebuilds the registry pointing
 *     every non-controlling party member at the new controlled body.
 *   - Combat / mid-charm / cross-map bots are skipped on each tick (they
 *     handle their own movement; we don't fight the combat AI).
 *
 * Bypasses mod-playerbots' AI cache entirely. Predictable, deterministic.
 */
#ifndef WOWPSPARTY_PARTYFOLLOW_H
#define WOWPSPARTY_PARTYFOLLOW_H

#include "ObjectGuid.h"

#include <string>

class Player;

namespace WowPsParty
{
    // Set the active "everyone in account_party follows X" directive. Clears
    // any previous directives for the same account first. Call this on every
    // swap with the swap target as leaderGuid; call with leaderGuid = the
    // session player's GUID on swap-back-to-self.
    void SetActiveFollowers(uint32 account, ObjectGuid leaderGuid);

    // Optional: clear directives for an account (e.g., on logout).
    void ClearFollowersForAccount(uint32 account);

    // Wires up the per-tick re-asserter. Called once from
    // PartyBootstrapWorldScript::OnStartup. Idempotent; subsequent
    // calls are no-ops.
    void InstallFollowTicker();

    // Queue a "quiet relogin" swap to be processed on the next world tick.
    // Avoids deleting the in-flight session player from inside its own
    // packet handler. The actual quiet logout + login chain runs in the
    // PartyFollow WorldScript::OnUpdate hook.
    void QueueQuietRelogin(uint32 sessionAccount, ObjectGuid targetGuid);

    // Returns true if the given bot has an active follow directive
    // (i.e., is a non-leader party member). Used by the patched
    // PlayerbotAI::UpdateAI to pause the AI tick while out-of-combat
    // so mod-playerbots' other actions don't fight our follow ticker.
    bool BotHasActiveFollowDirective(ObjectGuid guid);

    // Returns the leader guid that this follower is bound to, or an
    // empty ObjectGuid if no directive exists. Used by AssistTarget to
    // mirror the controlled body's target onto the rest of the party.
    ObjectGuid GetLeaderFor(ObjectGuid followerGuid);

    // Minimal "auto-attack the leader's victim + chase" assist used
    // when the user has NOT defined a rotation rule that matched on
    // this tick. We deliberately do NOT run mod-playerbots' default
    // strategy engine for party members — only what the user wrote in
    // the rotation editor casts spells. This keeps melee swings and
    // chase active so an empty rotation still produces useful idle
    // behaviour (= the unit auto-attacks the same thing you do).
    void AssistTarget(Player* bot);

    // Per-member target-selection mode. Stored in party_loadout.strategies_csv,
    // cached in memory, consumed by AssistTarget to decide which enemy the bot
    // attacks. Modes: "master" (default — whatever the controlled char targets,
    // with party-defense fallback), "tank" (focus-fire the party tank's target),
    // "nearest" (closest hostile), "loose" (nearest enemy that ISN'T attacking
    // this bot — for a tank picking up adds off the casters/healer).
    void TargetModeCacheSet(uint32 guidLow, std::string const& mode);
    void TargetModeRefreshFromDB(uint32 guidLow);
    std::string GetTargetMode(uint32 guidLow);

    // Per-tank "lead the recorded dungeon path + pull" toggle (default ON).
    void LeadDungeonCacheSet(uint32 guidLow, bool on);
    void LeadDungeonRefreshFromDB(uint32 guidLow);
    bool GetLeadInDungeon(uint32 guidLow);

    // Tell the follow ticker to LEAVE this bot alone for the next
    // `durationMs` milliseconds — used by the rotation engine's
    // `drink` / `hold_position` actions so a bot that's just been told
    // to sit and regen doesn't get yanked back into formation by the
    // 1Hz MoveFollow re-asserter. Re-asserting on every rotation tick
    // (~200-500ms) keeps the hold alive as long as the rule keeps
    // matching; once it stops matching, the hold expires and normal
    // formation movement resumes.
    void HoldFollower(ObjectGuid followerGuid, uint32 durationMs);
    bool IsFollowerHeld(ObjectGuid followerGuid);

    // Dungeon tank-lead engagement. If `bot` is the assigned tank for
    // its account, the leader is in a dungeon and not already engaging
    // a target, and there's a hostile creature within 40 yards of the
    // leader, the tank moves to attack it. 30-yard leash from the
    // leader prevents the tank from running off after a far mob; the
    // tank also won't pull if it's currently too far from the leader.
    //
    // Called every AI tick after AssistTarget, so dungeon pulls happen
    // organically as the leader walks forward and new mobs come into
    // sight range.
    void TankLeadEngagement(Player* bot);
}

#endif
