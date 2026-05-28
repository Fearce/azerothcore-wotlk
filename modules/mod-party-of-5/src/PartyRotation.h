/*
 * WowPs Party-of-5 mod — server-side rotation DSL (Phase 4 MVP)
 *
 * Player-configurable rule list per party member, stored in party_loadout.
 * Format (semicolon-separated rules, pipe-separated fields):
 *
 *     condition|action|priority;condition|action|priority;...
 *
 * Supported conditions:
 *     always
 *     in_combat | out_of_combat
 *     self_health<N | self_health>N      (N = percent 0-100)
 *     self_mana<N   | self_mana>N
 *     target_health<N | target_health>N
 *     has_target | no_target
 *     party_lowest_health<N              true if any party member is below N%
 *     party_lowest_health>N
 *
 * Supported actions:
 *     cast:<spell name>            cast on the bot's current target
 *     cast_self:<spell name>       cast on the bot itself
 *     buff_self:<spell name>       cast on self, but skip if the spell's
 *                                  aura is already active (Seals, Aspects,
 *                                  Armors, etc.)
 *     cast_party_lowest:<spell>    cast on lowest-HP party member (single-cast heal)
 *     cast_party_lowest_hot:<sp>   cast on lowest-HP party member, but only if
 *                                  they DON'T already have an aura from <sp>
 *                                  (prevents HoT clipping)
 *     cast_party_missing:<spell>   cast on first party member missing the
 *                                  spell's own aura (buff distribution —
 *                                  Mark of the Wild, Power Word: Fortitude, etc.)
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
