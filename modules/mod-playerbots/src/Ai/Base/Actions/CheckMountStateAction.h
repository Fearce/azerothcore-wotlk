/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_CHECKMOUNTSTATEACTION_H
#define _PLAYERBOT_CHECKMOUNTSTATEACTION_H

#include <unordered_map>
#include <vector>

#include "UseItemAction.h"

const uint16 SPELL_TRAVEL_FORM = 783;
const uint16 SPELL_FLIGHT_FORM = 33943;
const uint16 SPELL_SWIFT_FLIGHT_FORM = 40120;

struct MountData
{
    bool swiftMount = false;
    // Outer map: index (0 for ground, 1 for flight), inner map: effect speed -> vector of spell IDs.
    std::map<uint32, std::map<int32, std::vector<uint32>>> allSpells;
    // Default mount speed.
    int32 maxSpeed = 59;
};

struct PreferredMountCache
{
    std::vector<uint32> groundMounts;
    std::vector<uint32> flightMounts;
};

class PlayerbotAI;
class SpellInfo;

class CheckMountStateAction : public UseItemAction
{
public:
    CheckMountStateAction(PlayerbotAI* botAI) : UseItemAction(botAI, "check mount state", true) {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override { return true; }
    bool Mount();

    static void CompleteDismount(Player* bot);

    // Remember the mount a human just rode manually so that this character prefers the
    // same one when it is later AI-driven (a party henchman/hero). Ground and flying are
    // tracked independently. No-op unless spellInfo is a usable mount. Persists one row
    // per character + type in playerbots_preferred_mounts and refreshes the in-memory
    // preferred-mount cache so it applies on the next mount without a relog.
    static void RecordManualMount(Player* player, SpellInfo const* spellInfo);

    // The spell of the mount a human last rode manually on this character for the given
    // type (0 = ground, 1 = flying), or 0 if none was recorded. Lets other mount pickers
    // (e.g. the party-of-5 follow mount) honour the same preference.
    static uint32 GetPreferredMount(uint32 guid, uint32 type);

private:
    // 0 = ground, 1 = flying, -1 = not a usable mount spell.
    static int32 ClassifyMountSpell(SpellInfo const* spellInfo);

    // Lazily load playerbots_preferred_mounts into mountCache once per run.
    static void EnsurePreferredMountCache();

    Player* master;
    ShapeshiftForm masterInShapeshiftForm;
    ShapeshiftForm botInShapeshiftForm;
    // World-thread-only, no lock: written by RecordManualMount (Spell::_cast) and read by
    // GetPreferredMount / TryPreferredMount (bot AI update), all on the world thread. Any
    // future off-thread caller must add synchronization — a concurrent rehash on write
    // would corrupt an in-flight read.
    static std::unordered_map<uint32, PreferredMountCache> mountCache;
    static bool preferredMountTableChecked;
    float CalculateDismountDistance() const;
    float CalculateMountDistance() const;
    void Dismount();
    bool ShouldFollowMasterMountState(Player* master, bool noAttackers, bool shouldMount) const;
    bool ShouldDismountForMaster(Player* master) const;
    int32 CalculateMasterMountSpeed(Player* master, const MountData& mountData) const;
    bool CheckForSwiftMount() const;
    std::map<uint32, std::map<int32, std::vector<uint32>>> GetAllMountSpells() const;
    bool TryForms(Player* master, int32 masterMountType, int32 masterSpeed) const;
    bool TryPreferredMount(Player* master) const;
    uint32 GetMountType(Player* master) const;
    bool TryRandomMountFiltered(const std::map<int32, std::vector<uint32>>& spells, int32 masterSpeed) const;
};

#endif
