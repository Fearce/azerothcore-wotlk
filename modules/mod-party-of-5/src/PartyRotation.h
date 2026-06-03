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
 *     target_ttd<N | target_ttd>N         estimated seconds-to-die
 *     target_has_aura:<spell> | target_missing_aura:<spell>
 *     self_has_aura:<spell> | self_missing_aura:<spell>
 *     target_aura_remain:<spell><op>N  self_aura_remain  (seconds left)
 *     target_aura_stacks:<spell><op>N  self_aura_stacks
 *     spell_ready:<spell>  spell_cd_remain:<spell><op>N
 *     enemies_within:<R><op>N  enemies_in_melee  enemies_in_range
 *     enemies_clustered:<R><op>N  most enemies within R yd of each other
 *                                 (gate for Blizzard/Flamestrike-style AoE)
 *     master_dist<N | master_dist>N      yards from the party leader
 *     stance_is_none                     warrior in no stance (apply one)
 *     pet_exists | pet_missing | pet_dead | pet_health<N|>N
 *     party_lowest_health<N|>N  healer_mana  tank_health
 *     party_has_{disease,poison,magic,curse,dead}
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

    // True when the rotation contains a keep_distance_* rule, i.e. the user has
    // opted the bot into rotation-driven positioning (kiting). AssistTarget then
    // stops chasing and lets the rotation own movement.
    bool BotIsKiting(ObjectGuid guid);

    // True when the rotation contains a close_to_enemy rule — the bot advances to
    // within N yards of the nearest party-engaged enemy. AssistTarget FULLY yields
    // target + movement to it (the rule finds its own enemy and drives its feet).
    bool BotIsAdvancing(ObjectGuid guid);

    // Returns true if the bot has at least one cached rule (cheap check, called
    // every UpdateAI tick).
    bool HasRotation(uint32 guid);

    // Evaluate the cached rotation rules for the given bot, fire the highest-
    // priority matching rule's action, and return true if anything fired.
    // Called from PlayerbotAI::UpdateAI (the [WowPsParty PATCH] block) BEFORE
    // the bot's normal AI tick — our rule wins, normal AI still runs after for
    // positioning/follow.
    bool TickRotation(Player* bot);
}

#endif
