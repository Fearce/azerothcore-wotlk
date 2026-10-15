#ifndef PRIVATE_CLASS_SPELL_POLICY_H
#define PRIVATE_CLASS_SPELL_POLICY_H

#include "PrivateClassPolicy.h"

namespace PrivateClasses
{
    // Learning/admin grants require participation. Receiving an effect from an
    // authorized caster is separate and must remain possible for stock players.
    constexpr bool CanAcquireSpell(uint32_t spell, uint8_t ownerClass, bool assetsConfirmed)
    {
        return !IsCustomSpell(spell) || (IsCustom(ownerClass) && assetsConfirmed);
    }
}

#endif
