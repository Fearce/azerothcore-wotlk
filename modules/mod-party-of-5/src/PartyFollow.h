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
class Unit;

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

    // Hired ALTS: the player's OWN account characters (not enrolled in account_party)
    // spawned as follower bots. They behave like henchmen for AI / follow / corpse-loot
    // and survive SetActiveFollowers like henchmen (they live OUTSIDE account_party), but
    // unlike henchmen they keep their own gear / talents / bags / saved rotation — NOTHING
    // mutates their loadout (no ammo, poison, shard-trim, re-level, sanitize, bag-clear).
    // They're hidden from the party talent / gear / inventory panels (those key off
    // account_party enrollment) and loot to their OWN bags, never the shared party grid.
    void   AddHiredAltDirective(uint32 account, ObjectGuid altGuid,
                                ObjectGuid leaderGuid, std::string const& role);
    bool   IsHiredAlt(ObjectGuid guid);
    uint32 CountHiredAltsFor(ObjectGuid leaderGuid);
    uint32 CountFollowersFor(ObjectGuid leaderGuid);   // alts + henchmen
    int    FormationIndexFor(ObjectGuid follower, ObjectGuid leaderGuid);
    bool   IsLeadTank(ObjectGuid botGuid);   // role-based; works for henchmen
    bool   PartyHasLiveTank(ObjectGuid memberGuid);  // live tank-role member → ranged stand at range, don't kite
    // True while a lead tank is mid ranged-pull (holding at throwing range so the
    // pack closes on it). repositionToCast reads this to refuse chasing a melee
    // ability into the pack during the pull.
    bool   IsTankPulling(ObjectGuid tankGuid);
    // True while a lead tank is running an active maintain-N BODY-PULL gather (walking the
    // pack in, building no threat). The rotation engine reads it to keep a bear in form
    // mid-pull and to hold DPS offense for the whole gather.
    bool   TankGatherActive(uint32 tankLow);
    // True while a lead tank is running a CAREFUL PULL: a pull it judged dangerous to fight
    // at the pack's location (too many neighbouring mobs would social/proximity-aggro), so
    // it aggros the opener and drags it back to a safe spot near the party instead of
    // charging in. Like the gather it owns the feet and suppresses rotation for the drag.
    bool   TankCarefulPullActive(uint32 tankLow);
    // Suppress the group-removal dismiss hook while a (re-)hire moves a henchman
    // between groups (so pulling it out of a stale LFG group doesn't dismiss it).
    void   SetHenchmanRegrouping(ObjectGuid henchGuid, bool on);
    bool   IsHenchmanRegrouping(ObjectGuid henchGuid);
    // Record / query a henchman dismissed in the last few seconds, so the
    // TellMaster silence guard suppresses the framework's farewell whisper that
    // fires from the logout path after the henchman registration is already gone.
    void   MarkHenchmanRecentlyDismissed(ObjectGuid henchGuid);
    bool   WasHenchmanRecentlyDismissed(ObjectGuid henchGuid);
    // The follow directive's role ("tank"/"healer"/"dps") for a follower
    // (alt or henchman), or "" if it has no directive. Lets the rotation
    // editor's "Generate" button match the role the bot actually runs.
    std::string RoleForGuid(ObjectGuid botGuid);

    // Refresh the cached leader role for an account from a character's stored role
    // (account_party.role if enrolled, else party_loadout.role). Lets a solo /
    // un-enrolled controlled character carry a tank/healer/dps role.
    void SetLeaderRoleForChar(uint32 account, ObjectGuid guid);

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

    // The account's hired henchman guid-counters — so the client can tell a
    // managed henchman from a second human sharing the WoW group.
    void GetHenchmanGuidsForAccount(uint32 account, std::vector<uint32>& out);

    // Every tracked follower (hired henchmen AND enrolled alts) across all
    // leaders. Used by the arena/BG sweep to find managed party bots that got
    // orphaned in a finished battleground (they have no playerbot BG-leave AI).
    void GetAllFollowers(std::vector<ObjectGuid>& out);

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

    // Per-DPS "wait for the human tank to build threat before engaging" toggle.
    // The DEFAULT is per-type: henchmen WAIT (so a dumb helper never rips aggro),
    // heroes BLAST as they used to; an explicit Rotation-Editor choice overrides.
    // val: 0 = blast, 1 = wait, -1 = clear the override (back to per-type default).
    void WaitTankThreatCacheSet(uint32 guidLow, int val);
    void WaitTankThreatRefreshFromDB(uint32 guidLow);
    bool GetWaitTankThreat(ObjectGuid guid);

    // Healer threat-hold. True while a healer should SKIP its direct heals because
    // its human tank-leader is still gathering a pull (heal threat rips a fresh
    // pull off the tank). Gated behind the per-bot wait-tank-threat toggle; the
    // underlying gather window ends if the tank drops low, so a dying tank is still
    // healed. Consulted by TickRotation to suppress the cast_party_lowest verbs.
    bool HealerShouldHoldHeal(Player* bot);

    // True for the whole body-pull/gather phase of the lead tank — the tank MOVES only and
    // its rotation must be suppressed. Consulted by TickRotation (early-returns on it).
    bool TankIsBodyPulling(Player* bot);

    // True while `bot` (a non-tank) is waiting for a human tank-lead to take threat.
    // MaintainBotPet uses it to heel a hunter/warlock pet during the hold instead of
    // letting it charge the pull before the tank has aggro.
    bool BotWaitsForHumanTank(Player* bot);

    // Like BotWaitsForHumanTank but for ANY party tank — a bot/henchman lead tank OR a
    // human tank-leader. True when this non-tank bot is under the same threat throttle
    // AssistTarget applies (a tank leads and this bot hasn't opted out of waiting).
    // MaintainBotPet reads it to leash the pet so it respects the throttle.
    bool BotUnderTankThreatRegime(Player* bot);

    // The nearest mob the party TANK already holds a real engage lead on that `bot` is
    // still under the threat cap on — the only mob its pet may attack while the owner is
    // holding/throttled/ground-AoEing under a tank lead. nullptr if none (pet heels).
    Unit* PickPetThrottleTarget(Player* bot);

    // True when a mob is RUNNING AWAY: spell-fear / low-health family panic flee
    // (UNIT_STATE_FLEEING) OR flee-to-get-assistance (it runs to a nearby friendly pack for
    // help — the classic "social-aggro the next pack" case). Covers every flee movement
    // generator. Used by the tank pull-spot anchor and the DPS slow-the-runner peel.
    bool IsUnitFleeing(Unit* u);

    // True while THIS bot's lead tank is still body-pulling / locking a pull — the rotation
    // engine holds the bot's offense for the whole window (mirrors the movement pull-hold).
    bool PartyPullHoldActive(Player* bot);

    // Per-TANK "safe pull" toggle. Default ON for every tank (the ranged
    // tag-and-step-back opener); an explicit Rotation-Editor choice can switch a
    // tank to barging straight into melee instead.
    // val: 0 = barge, 1 = safe pull, -1 = clear the override (back to default ON).
    void SafePullCacheSet(uint32 guidLow, int val);
    void SafePullRefreshFromDB(uint32 guidLow);
    bool GetSafePull(ObjectGuid guid);

    // Per-NON-TANK "anchor on tank" toggle. Default OFF everywhere (no per-type
    // default). When ON, a non-tank bot formation-follows the party TANK instead
    // of the human leader while the leader isn't the tank — melee reach the front
    // fast when the leader is ranged. Non-combat formation follow only.
    void AnchorTankCacheSet(uint32 guidLow, int val);
    void AnchorTankRefreshFromDB(uint32 guidLow);
    bool BotAnchorOnTank(ObjectGuid guid);

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

    // Manual "pull one more" (keybind). Arm the LEAD TANK of `leader`'s party to
    // run to + body-pull the single NEAREST out-of-combat mob for holdMs, then
    // resume normal AI. Re-armed on each keypress, so a mob more than one window
    // away requires spamming the bind until the tank reaches it (mirrors
    // RecallFollowers). The window clears the instant the mob enters combat (pull
    // achieved), dies, or lapses. Messages the caller if there's no tank/mob.
    void PullNearestExtra(Player* leader, uint32 holdMs);

    // Per-AI-tick driver for an armed pull-more. MUST run LAST in the bot tick so
    // its MoveChase wins over rotation/AssistTarget. No-op unless this bot is the
    // lead tank with a live pull-more window.
    void TickTankPullMore(Player* bot);

    // stop_attacking hold. While set, the bot suppresses ALL offence — AssistTarget
    // won't engage/auto-attack and the rotation drops every offensive verb — but heals,
    // buffs and movement still run. The `stop_attacking` rotation action re-arms it each
    // tick its condition holds (e.g. party_has_aura:Mirrored Soul), so it lapses shortly
    // after the condition clears and the bot resumes DPS.
    void MarkOffensiveHold(ObjectGuid followerGuid, uint32 holdMs);
    bool IsOffensiveHeld(ObjectGuid guid);

    // Cleanse hold. While set, the bot suppresses its dispel/cure (cure_party) — heals,
    // buffs, DPS and movement still run. The `stop_cleansing` rotation action re-arms it
    // each tick its condition holds, so ONE Common-tab rule (e.g. gated on
    // party_aura_clustered:Mutating Injection<21) stops the WHOLE party from cleansing a
    // dispel-explodes debuff until it's safe, with no per-character cure edits.
    void MarkCleanseHold(ObjectGuid followerGuid, uint32 holdMs);
    bool IsCleanseHeld(ObjectGuid guid);

    // Cast hold ("stop & hold casting"). While set, the bot declines every spell with a
    // cast time OR channel — instants still fire — so it can ride out an enemy silence /
    // school-lockout (e.g. Disrupting Shout) on instants only instead of eating a wasted
    // hard-cast. The `stop_hold_cast` rotation action re-arms it each tick its condition
    // holds and also cancels the in-progress hard cast. Enforced in the rotation's cast
    // path (faceAndCast/faceAndCastAt), which already knows a spell's cast/channel time.
    void MarkCastHold(ObjectGuid followerGuid, uint32 holdMs);
    bool IsCastHeld(ObjectGuid guid);

    // Vehicle behaviour (Oculus drakes, etc.). Both are gated behind a vehicle scenario by
    // their callers, so normal follow/rotation is untouched otherwise.
    //  - TickBotVehicleMovement: from the follow ticker — board/acquire a vehicle when the
    //    leader takes one, fly after the leader, exit when they do. True = owned this tick
    //    (caller skips the normal ground follow).
    //  - TickBotVehicleAbilities: from TickRotation — fire the vehicle's own abilities
    //    instead of the bot's (dead) normal spells. Always returns true (rotation skipped).
    bool TickBotVehicleMovement(Player* bot, Player* leader);
    bool TickBotVehicleAbilities(Player* bot);

    // Post the party-chat "Lockpicking skill up! Now X/Y." line (the same
    // announcement the in-world Mining/Herbalism/Skinning gather paths use)
    // after a rogue hero's Lockpicking actually rises. Call right after a
    // successful UpdateGatherSkill from the lockbox/chest/door pick paths.
    // No-op for any skill the announcer doesn't handle; self-throttles to
    // once per reached value so it never re-spams. Lives here so the addon
    // protocol's lockbox handler (a separate TU) can reuse it.
    void AnnounceGatherSkillUp(Player* bot, uint32 skill);

    // Out-of-combat gathering. If `bot` is one of the player's alts (not a
    // henchman) and was trained in Mining or Herbalism, harvest a nearby node
    // (within 30y, within the bot's skill) while travelling with the party,
    // then resume following. Training the profession is the only opt-in — no
    // toggle. Called every AI tick; fast-exits for bots without a gather skill.
    void TickGathering(Player* bot);

    // Out-of-combat corpse looting for HIRED HENCHMEN ONLY. An idle henchman
    // walks to the nearest corpse its own WoW group killed and loots it through
    // the engine's native loot path — exactly like a player right-clicking it.
    // Money is split across the party by the built-in group-loot handler (NO
    // manual money modification — that is party-of-5 behaviour and would dupe the
    // gold); items follow the group's loot rules. Enrolled alts and the human are
    // untouched. Called every AI tick after TickGathering; fast-exits for
    // non-henchmen.
    void TickHenchmanLoot(Player* bot);

    // Battleground entry for HIRED HENCHMEN + enrolled heroes. Their default AI
    // is gated out, so when the human queues the party for a BG they never click
    // "Enter Battle". This accepts a pending BG invite for the bot (ports it in)
    // so the whole party enters the battleground with the human. Called every AI
    // tick; fast-exits unless the bot has a live BG invite. Random fill bots use
    // their own AI and are never managed party bots, so they don't come through here.
    void TickAcceptBgInvite(Player* bot);

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
