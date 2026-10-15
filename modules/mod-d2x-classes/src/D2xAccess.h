#ifndef D2X_ACCESS_H
#define D2X_ACCESS_H

#include <cstdint>

namespace d2x::access
{
    // A teammate may receive this one triggered heal, without learning or casting
    // any custom ability. The live adapter verifies the originating aura owner.
    constexpr bool CanApplyLifeTapHeal(uint32_t spell, uint32_t sourceAura,
        bool participatingSource, bool playerControlledReceiver, bool selfTarget)
    {
        return spell == 110262 && sourceAura == 110256 && participatingSource &&
            playerControlledReceiver && selfTarget;
    }
}

#endif
