#ifndef PRIVATE_CLASS_POLICY_H
#define PRIVATE_CLASS_POLICY_H

#include <cstdint>

// Pure policy shared by entry-point checks and focused tests. No GM bypass.
namespace PrivateClasses
{
    constexpr bool IsCustom(uint8_t cls) { return cls == 12 || cls == 13; }

    constexpr bool CanUse(bool enabled, uint32_t owner, uint32_t preview,
        uint32_t account, uint32_t selectedRealm, bool bot, uint32_t normalRealm, bool assetsConfirmed)
    {
        return assetsConfirmed && enabled && owner && preview && preview != normalRealm && owner == account &&
            preview == selectedRealm && !bot;
    }

    constexpr uint8_t VisibleClass(uint8_t cls, bool customAssets)
    {
        return customAssets ? cls : cls == 12 ? 9 : cls == 13 ? 8 : cls;
    }
}

#endif
