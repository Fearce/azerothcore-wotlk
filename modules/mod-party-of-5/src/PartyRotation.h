/*
 * WowPs Party-of-5 mod — server-side rotation DSL (Phase 4 MVP)
 *
 * Player-configurable rule list per party member, stored in party_loadout.
 * Format (semicolon-separated rules, pipe-separated fields):
 *
 *     condition|action|priority[|flags];...
 *
 * Conditions can be AND-chained with '&' (every clause must hold).
 * Any single clause may be prefixed with '!' to negate it (generic NOT),
 * e.g. "!target_is_boss" or "!self_has_aura:Bloodlust".
 *
 * Supported conditions (selection — see PartyRotation.cpp for the full set):
 *     always
 *     in_combat | out_of_combat
 *     am_tank | am_dps | am_healer       the BOT's own party role (fan a rule out by
 *                                 job, e.g. am_dps so only DPS chase a caster to kick)
 *     is_moving | is_not_moving
 *     self_health<N|>N  self_mana  self_rage  self_energy  self_power (% 0-100)
 *     self_combo<N|>N                     combo points, RAW 0-5
 *     target_health<N | target_health>N
 *     has_target | no_target
 *     target_moving | target_not_moving
 *     target_is_player | target_is_npc
 *     target_is_boss | target_is_elite | target_is_rare | target_is_normal
 *     target_type_{beast,dragonkin,demon,elemental,giant,undead,humanoid}
 *     target_casting | target_channeling | target_interruptible
 *     target_casting:<spell> | target_channeling:<spell>   fire only while the target
 *                                 is mid-cast of THAT spell (display name, any rank, or
 *                                 numeric id) — e.g. target_casting:Dark Smash. Empty arg
 *                                 = any cast. Pair with move_out_of_los or a kick/stun.
 *     self_casting | self_channeling     the bot's OWN cast/channel state
 *     target_ttd<N | target_ttd>N         estimated seconds-to-die
 *     target_has_aura:<spell> | target_missing_aura:<spell>
 *     self_has_aura:<spell> | self_missing_aura:<spell>
 *     target_aura_remain:<spell><op>N  self_aura_remain  (seconds left)
 *     target_aura_stacks:<spell><op>N  self_aura_stacks
 *     spell_ready:<spell>  spell_cd_remain:<spell><op>N
 *     enemies_within:<R><op>N  enemies_in_melee  enemies_in_range
 *     enemies_clustered:<R><op>N  most enemies within R yd of each other
 *                                 (gate for Blizzard/Flamestrike-style AoE)
 *     party_injured_clustered:<R><op>N  most INJURED allies (<90% HP, in cast
 *                                 range) within R yd of each other — gate for
 *                                 Chain Heal / Wild Growth / Prayer of Healing
 *     master_dist<N | master_dist>N      yards from the party leader
 *     stance_is_none                     warrior in no stance (apply one)
 *     pet_exists | pet_missing | pet_dead | pet_health<N|>N
 *     party_lowest_health<N|>N  healer_mana  tank_health
 *     party_has_{disease,poison,magic,curse,dead}
 *     self_totem_active:<element|totem name>   shaman: that totem up — arg is an
 *                                 element (fire/earth/water/air = its one slot) OR a
 *                                 totem SPELL NAME ("Mana Tide Totem", any slot)
 *     self_totem_missing:<element|totem name>  inverse of the above
 *     self_totem_count<N | self_totem_count>N    shaman: # totems up (0-4)
 *
 * Supported actions:
 *     cast:<spell name>            cast on the bot's current target
 *     cast_self:<spell name>       cast on the bot itself
 *     cast_pet:<spell name>        cast on the bot's pet (Mend Pet)
 *     buff_self:<spell name>       cast on self, skip if the aura is active
 *     cast_party_lowest:<spell>    cast on lowest-HP party member (heal)
 *     cast_party_lowest_hot:<sp>   as above, skip if they already have <sp>
 *     cast_party_missing:<spell>   buff first member missing the spell's aura
 *     cast_class_missing / cast_role_missing / cure_party / rez_party /
 *     cast_loose_enemy / drink / eat / hold_position
 *     move_out_of_los              run behind cover so the current target can't see
 *                                  (or land its cast on) the bot; auto-returns to the
 *                                  fight when the gating condition clears. Pair with
 *                                  target_casting:<spell>, e.g.
 *                                  "target_casting:Dark Smash | move_out_of_los | 200".
 *     cast_scan:<spell>            cast <spell> on the FIRST party-engaged enemy (not
 *                                  just the victim) matching the rule's target_* gates,
 *                                  WITHOUT retargeting — off-target stuns/kicks. <spell>
 *                                  may be "available_stun"/"available_interrupt".
 *     interrupt_caster:<spell>     RANGED off-target interrupt: kick the first party-
 *                                  engaged enemy mid an INTERRUPTIBLE cast that the bot
 *                                  can reach in place, without moving/retargeting. <spell>
 *                                  empty or "available_interrupt" = this class's ready
 *                                  interrupt (Counterspell/Wind Shear/Silencing Shot/…).
 *                                  Pair with am_dps to pick who answers.
 *     interrupt_caster_melee:<sp>  as above for a MELEE interrupt: RUN to the nearest far
 *                                  caster (capped, party-engaged only — never into a fresh
 *                                  pack), kick it, then return to the bot's own target.
 *     use_item:<item name>         use a consumable/trinket from shared bags
 *     pull:<spell name>            ranged opener (Throw/Shoot) for the tank
 *     shoot                        fire the equipped physical ranged weapon
 *                                  (gun/bow/crossbow = Shoot, thrown = Throw) —
 *                                  free, no rage/mana; pair with out_of_combat
 *                                  for a ranged-weapon pull opener
 *     wand                         fire the equipped wand (free caster filler)
 *  Ground-targeted AoE spells (Blizzard, Flamestrike, Rain of Fire) used via
 *  cast:<spell> auto-aim at the densest enemy cluster, not the current target.
 *
 * Tank pull config (read by the engagement layer, not fired per tick):
 *     pull_count:N   lead tank opens on a cluster of up to N mobs (default 3, 1 = single).
 *                    Only the INITIAL pull from a rested party; cluster-aware (won't drag
 *                    a mob that brings the pack past N), LoS- and same-Z-gated. While the
 *                    tank gathers, the party holds fire so it can stack them.
 *
 * Optional 4th field `flags` (comma-separated). Recognised:
 *     clip   — allow this cast to interrupt the bot's own cast/channel.
 *
 * Highest priority matching rule wins per tick.
 */

#ifndef WOWPSPARTY_PARTYROTATION_H
#define WOWPSPARTY_PARTYROTATION_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;
class Unit;

namespace WowPsParty
{
    struct RotationRule
    {
        std::string condition;
        std::string action;
        int         priority = 0;
        // Optional 4th DSL field: comma-separated action modifiers. Currently
        // recognised: "clip" — allow this cast to interrupt the bot's own
        // in-progress cast/channel (otherwise a rule is skipped while the bot
        // is mid-cast). Empty for the common case.
        std::string flags;
    };

    // Parse a rotation string (semicolon-separated rules) into RotationRule list.
    // Invalid entries are silently dropped; returns whatever parsed cleanly.
    std::vector<RotationRule> ParseRotationString(std::string const& dsl);

    // Re-serialise rules back to the DSL form.
    std::string SerialiseRotationRules(std::vector<RotationRule> const& rules);

    // Dominant talent tree (tabpage 0/1/2) of a live bot, by points spent;
    // cached per-bot. Lets the follow/positioning layer tell a melee hybrid spec
    // (enhancement shaman / feral druid = tree 1) from a ranged one. 0 if no
    // talents yet.
    uint8 PrimaryTalentTree(Player* bot);

    // The lead tank's ranged pull ability id (Heroic Throw, Avenger's Shield,
    // Icy Touch, Faerie Fire, ...), or 0. Lets the engagement layer hold an
    // ability-only tank (no ranged weapon) at range instead of charging in.
    uint32 TankRangedPullSpell(Player* bot);

    // Fire the bot's equipped physical ranged weapon at `target` (gun/bow/crossbow
    // = Shoot 3018 auto-repeat, thrown = Throw 2764). Free (no rage/mana) — the
    // reliable pull opener for a fresh tank with ~0 rage. False if no physical
    // ranged weapon / out of range / rejected (e.g. no ammo), so the caller can
    // fall back to an ability. Used by the `shoot` verb and the lead-tank pull.
    bool FireRangedWeaponShot(Player* bot, Unit* target);

    // The stand-off distance the lead tank should hold at to fire its opener,
    // derived from that opener's actual range (so a 30y ability pulls from ~26y, a
    // 20y one from ~16y). The engagement layer uses this instead of a fixed range.
    float TankPullHoldRange(Player* bot);

    // True if the party is genuinely engaging ON FOOT — the leader is off its
    // mount, or a member is in combat AND dismounted (knocked off by damage). Used
    // by the mount guard AND the follow ticker to tell a real fight from a mounted
    // fly-by (everyone still riding past incidental aggro).
    bool PartyEngagedDismounted(Player* bot);

    // True if the unit is actively drinking/eating: seated with a food
    // (MOD_REGEN) or drink (MOD_POWER_REGEN) aura. Matched by aura TYPE, not a
    // specific spell id, so it catches a real player's higher-rank water/food —
    // the tank-pull resting check uses it on the human leader, who never has the
    // rank-1 Drink 430 / Food 433 the bots cast on themselves.
    bool BotIsConsuming(Player* bot);

    // The distance a ranged DPS bot should hold its victim at so its WHOLE
    // single-target kit is in range — the shortest offensive `cast:` in its
    // rotation, clamped to [18, 28]y. AssistTarget uses it as the ranged hold cap
    // so a bot doesn't park at 30y unable to use its shorter-range abilities.
    float BotRangedCastHold(Player* bot);

    // Cache management. Called by PartyMgr when a bot logs in / loadout changes.
    void RotationCacheSet(uint32 guid, std::vector<RotationRule> rules);
    void RotationCacheClear(uint32 guid);
    void RotationCacheRefreshFromDB(uint32 guid);

    // COMMON shared rotation (per ACCOUNT) — prepended to every party bot's rules in
    // TickRotation. SET stores it; Get returns a copy; RefreshFromDB reloads it.
    void SharedRotationCacheSet(uint32 account, std::vector<RotationRule> rules);
    std::vector<RotationRule> GetSharedRotation(uint32 account);
    void SharedRotationRefreshFromDB(uint32 account);

    // True when the rotation contains a keep_distance_* rule, i.e. the user has
    // opted the bot into rotation-driven positioning (kiting). AssistTarget then
    // stops chasing and lets the rotation own movement.
    bool BotIsKiting(ObjectGuid guid);

    // Names listed in the bot's rotation "focus:" rule(s) — adds the party must
    // kill on sight (e.g. "Chaos Rift", "Frost Tomb"). Empty when none configured.
    // AssistTarget reads this to override target selection onto a matching enemy.
    void BotFocusNames(ObjectGuid guid, std::vector<std::string>& out);
    // Like BotFocusNames but for "focus_engaged:" rules — the override only fires on a
    // match the PARTY is already in combat with (won't chase an un-pulled same-named mob).
    void BotFocusEngagedNames(ObjectGuid guid, std::vector<std::string>& out);

    // A lead tank's configured INITIAL-pull target size. Default 3, clamped [1,8];
    // 1 = the classic single-mob pull. TankLeadEngagement reads this to body-pull a
    // cluster of up to N mobs on the opener (cluster-aware, LoS- and Z-gated).
    // Sourced from the first-class party_loadout.pull_count column (set in the
    // rotation editor); a legacy "pull_count:N" rotation directive is the fallback.
    uint32 BotInitialPullCount(ObjectGuid guid);
    // Per-bot pull_count cache (mirrors safe_pull). CacheSet: val in [1,8] sets it,
    // anything else clears it. RefreshFromDB reloads it from party_loadout.
    void PullCountCacheSet(uint32 guidLow, int val);
    void PullCountRefreshFromDB(uint32 guidLow);

    // A lead tank's configured dungeon lead distance (yards). Default 10,
    // clamped [5,40]; sourced from the party_loadout.lead_distance column (set
    // in the rotation editor). TankFollowPath / the lead-tank formation read it.
    uint32 BotLeadDistance(ObjectGuid guid);
    void LeadDistCacheSet(uint32 guidLow, int val);
    void LeadDistRefreshFromDB(uint32 guidLow);

    // A lead tank's configured INITIAL ENGAGE RANGE (yards): how far the auto-pull
    // opener (TankLeadEngagement) scans around the tank for the nearest hostile to
    // open on. Default 22, clamped [10,40]; sourced from the party_loadout.engage_range
    // column (set in the rotation editor slider).
    uint32 BotEngageRange(ObjectGuid guid);
    void EngageRangeCacheSet(uint32 guidLow, int val);
    void EngageRangeRefreshFromDB(uint32 guidLow);

    // Returns true if the bot has at least one cached rule (cheap check, called
    // every UpdateAI tick).
    bool HasRotation(uint32 guid);

    // Record that a player took damage from `spellId` (now), along with the source
    // unit's world position (hasSrc=false when the attacker was unknown). Called from
    // the ModifySpellDamageTaken UnitScript; read by the took_damage_from:<name>
    // condition and by the walk_away_from_source action (which flees the source pos).
    void RecordSpellDamageTaken(uint32 guidLow, uint32 spellId,
                                float srcX, float srcY, float srcZ, bool hasSrc);

    // Evaluate the cached rotation rules for the given bot, fire the highest-
    // priority matching rule's action, and return true if anything fired.
    // Called from PlayerbotAI::UpdateAI (the [WowPsParty PATCH] block) BEFORE
    // the bot's normal AI tick — our rule wins, normal AI still runs after for
    // positioning/follow.
    bool TickRotation(Player* bot);
}

#endif
