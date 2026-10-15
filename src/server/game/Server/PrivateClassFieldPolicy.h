#ifndef WOWPS_PRIVATE_CLASS_FIELD_POLICY_H
#define WOWPS_PRIVATE_CLASS_FIELD_POLICY_H

#include "PrivateClassPolicy.h"

namespace PrivateClasses
{
    // Race/gender/power bytes may change, but raw debugger writes must never
    // convert a character into or out of a private class, even for its owner.
    constexpr bool CanEditIdentityField(uint32_t before, uint32_t after)
    {
        auto const oldClass = uint8_t(before >> 8);
        auto const newClass = uint8_t(after >> 8);
        return oldClass == newClass || (!IsCustom(oldClass) && !IsCustom(newClass));
    }
}

#endif
