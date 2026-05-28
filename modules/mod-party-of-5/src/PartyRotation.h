/*
 * WowPs Party-of-5 mod — server-side rotation DSL (Phase 4 MVP)
 *
 * Player-configurable rule list per party member, stored in party_loadout.
 * Format (semicolon-separated rules, pipe-separated fields):
 *
 *     condition|action|priority[|flags];...
 *
 * Conditions can be AND-chained with '&' (every clause must hold).
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
 *
 * Optional 4th field `flags` (comma-separated). Recognised:
 *     clip   — allow this cast to interrupt the bot's own cast/channel.
 *
 * Highest priority matching rule wins per tick.
 */

#ifndef WOWPSPARTY_PARTYROTATION_H
#define WOWPSPARTY_PARTYROTATION_H

#include "Define.h"

#include <string>
#include <vector>

class Player;

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

    // Cache management. Called by PartyMgr when a bot logs in / loadout changes.
    void RotationCacheSet(uint32 guid, std::vector<RotationRule> rules);
    void RotationCacheClear(uint32 guid);
    void RotationCacheRefreshFromDB(uint32 guid);

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
