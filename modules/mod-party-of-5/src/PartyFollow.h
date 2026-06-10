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
#include <vector>

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

    // Henchmen: hired bot companions drawn from the random-bot pool. They get
    // our follow ticker / leash / tank-lead but DEFAULT mod-playerbots combat
    // AI. Tracked as a follow directive flagged henchman=true.
    void   AddHenchmanDirective(uint32 account, ObjectGuid henchGuid,
                                ObjectGuid leaderGuid, std::string const& role);
    void   SetHenchmanRole(ObjectGuid followerGuid, std::string const& role);  // refresh after re-spec
    void   RemoveFollower(ObjectGuid followerGuid);   // dismiss / logout
    bool   IsHenchman(ObjectGuid guid);
    uint32 CountHenchmenFor(ObjectGuid leaderGuid);
    uint32 CountFollowersFor(ObjectGuid leaderGuid);   // alts + henchmen
    int    FormationIndexFor(ObjectGuid follower, ObjectGuid leaderGuid);
    bool   IsLeadTank(ObjectGuid botGuid);   // role-based; works for henchmen
    bool   PartyHasLiveTank(ObjectGuid memberGuid);  // live tank-role member → ranged stand at range, don't kite
    // True while a lead tank is mid ranged-pull (holding at throwing range so the
    // pack closes on it). repositionToCast reads this to refuse chasing a melee
    // ability into the pack during the pull.
    bool   IsTankPulling(ObjectGuid tankGuid);
    // Suppress the group-removal dismiss hook while a (re-)hire moves a henchman
    // between groups (so pulling it out of a stale LFG group doesn't dismiss it).
    void   SetHenchmanRegrouping(ObjectGuid henchGuid, bool on);
    bool   IsHenchmanRegrouping(ObjectGuid henchGuid);
    // The follow directive's role ("tank"/"healer"/"dps") for a follower
    // (alt or henchman), or "" if it has no directive. Lets the rotation
    // editor's "Generate" button match the role the bot actually runs.
    std::string RoleForGuid(ObjectGuid botGuid);

    // Wires up the per-tick re-asserter. Called once from
    // PartyBootstrapWorldScript::OnStartup. Idempotent; subsequent
    // calls are no-ops.
    void InstallFollowTicker();

    // Returns true if the given bot has an active follow directive
    // (i.e., is a non-leader party member). Used by the patched
    // PlayerbotAI::UpdateAI to pause the AI tick while out-of-combat
    // so mod-playerbots' other actions don't fight our follow ticker.
    bool BotHasActiveFollowDirective(ObjectGuid guid);

    // Mark a tank as actively leading the dungeon path for the next
    // durationMs. While this is set, the follow ticker leaves the tank to
    // TankFollowPath — no MoveFollow re-assert and, crucially, no "stuck"
    // catch-up teleport (which otherwise snaps a leading-but-idle tank onto a
    // stopped leader). Called every tick from TankFollowPath while leading.
    void MarkTankLeading(ObjectGuid tankGuid, uint32 durationMs);

    // Fill `out` with the whole party (leader + all follower bots) that the
    // given member belongs to, from our in-memory follow directives. Use this
    // instead of bot->GetGroup() for party-target logic — the WoW Group can
    // form incompletely and leave bots blind to the leader. `member` is
    // included; empty if the member isn't a tracked follower.
    void GetPartyGuidsFor(ObjectGuid member, std::vector<ObjectGuid>& out);

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

    // Force a freshly-revived / stuck bot back into a movable state. A bot has
    // no game client to round-trip the unroot, so a movement-blocking unit-
    // state (UNIT_STATE_NOT_MOVE bits / MOVEMENTFLAG_ROOT) can survive a revive
    // and freeze the Follow/Chase MovementGenerators. Call after any revive or
    // when a bot is detected frozen. Safe only out of combat (no legit CC).
    void ForceMovableState(Player* p);

    // "Come Hither" recall: bring every follower bound to `leader` to the leader's
    // position and HOLD them there for holdMs (the hold suppresses both the follow
    // ticker and the combat assist, so they don't immediately path back). Used by
    // the keybind to drag the party out of ground effects; normal movement resumes
    // when the hold expires.
    void RecallFollowers(Player* leader, uint32 holdMs);

    // True while a "Come Hither" recall is holding this follower. TickRotation
    // checks it to PAUSE the rotation so a ranged DPS runs to the leader instead
    // of hard-casting in place. (Separate from IsFollowerHeld so drink/hold, which
    // re-assert via the rotation, aren't paused.)
    bool IsBeingRecalled(ObjectGuid followerGuid);

    // Out-of-combat gathering. If `bot` is one of the player's alts (not a
    // henchman) and was trained in Mining or Herbalism, harvest a nearby node
    // (within 30y, within the bot's skill) while travelling with the party,
    // then resume following. Training the profession is the only opt-in — no
    // toggle. Called every AI tick; fast-exits for bots without a gather skill.
    void TickGathering(Player* bot);

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
