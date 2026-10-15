#include "D2xClasses.h"

#include "Chat.h"
#include "DBCStores.h"
#include "Player.h"
#include "Pet.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"

#include <algorithm>

using namespace d2x;

namespace
{
    // The class screen promises a talent point at every level from 1; stock points start at level 10.
    constexpr uint32 LEVELS_BEFORE_STOCK_POINTS = 9;

    void TeachClassSpells(Player* player)
    {
        for (uint32 spell : TaughtSpellsOf(player))
            if (!player->HasSpell(spell))
                player->learnSpell(spell);
    }

    // A skill's spellbook spell is known exactly while the active spec holds a rank of the skill.
    void SyncSpellbook(Player* player)
    {
        for (SkillIds const& skill : SkillsOf(player))
        {
            if (!skill.spell)
                continue;

            bool const held = HasSkill(player, skill.talent);
            if (held && !player->HasSpell(skill.spell))
                player->learnSpell(skill.spell);
            else if (!held && player->HasSpell(skill.spell))
                player->removeSpell(skill.spell, player->GetActiveSpecMask(), false);
        }
    }

    void ReportMissingPrereq(Player* player, uint32 prereqTalent)
    {
        std::span<SkillIds const> const skills = SkillsOf(player);
        auto skill = std::ranges::find(skills, prereqTalent, &SkillIds::talent);
        if (skill == skills.end())
            return;

        if (SpellInfo const* rank = sSpellMgr->GetSpellInfo(skill->ranks[0]))
            ChatHandler(player->GetSession()).SendNotification(
                Acore::StringFormat("Requires a point in {}.", rank->SpellName[LOCALE_enUS]));
    }
}

class d2x_player : public PlayerScript
{
public:
    d2x_player() : PlayerScript("d2x_player", {
        PLAYERHOOK_ON_CALCULATE_TALENTS_POINTS,
        PLAYERHOOK_CAN_LEARN_TALENT,
        PLAYERHOOK_ON_FREE_TALENT_POINTS_CHANGED,
        PLAYERHOOK_ON_AFTER_SPEC_SLOT_CHANGED,
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_AFTER_GUARDIAN_INIT_STATS_FOR_LEVEL,
    }) { }

    void OnPlayerCalculateTalentsPoints(Player const* player, uint32& talentPointsForLevel) override
    {
        if (IsD2Class(player))
            talentPointsForLevel += std::min<uint32>(player->GetLevel(), LEVELS_BEFORE_STOCK_POINTS);
    }

    // The core checks a talent's first prerequisite; the Diablo II trees have up to three.
    bool OnPlayerCanLearnTalent(Player* player, TalentEntry const* talent, uint32 /*rank*/) override
    {
        for (TalentPrereq const& prereq : ExtraPrereqsOf(player))
        {
            if (prereq.talent != talent->TalentID || HasSkill(player, prereq.prereq))
                continue;

            ReportMissingPrereq(player, prereq.prereq);
            return false;
        }
        return true;
    }

    // Fires after every talent learned through the talent frame, after a reset, on level-up and at the end
    // of a spec switch, each time with the talents already in place.
    void OnPlayerFreeTalentPointsChanged(Player* player, uint32 /*points*/) override
    {
        if (!player->IsInWorld() || !IsD2Class(player))
            return;

        SyncSpellbook(player);
        RescaleMinions(player);
        LOG_INFO("module", "[WowPs PrivateMods] class-sync={} level={} reason=talents-changed", player->getClass(), player->GetLevel());
    }

    void OnPlayerAfterSpecSlotChanged(Player* player, uint8 /*newSlot*/) override
    {
        if (IsD2Class(player))
            SyncSpellbook(player);
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!IsD2Class(player))
            return;

        TeachClassSpells(player);
        SyncSpellbook(player);
        RescaleMinions(player);
        LOG_INFO("module", "[WowPs PrivateMods] class-login={} level={} reason=spellbook-and-minions-synchronized", player->getClass(), player->GetLevel());
    }

    void OnPlayerAfterGuardianInitStatsForLevel(Player* player, Guardian* guardian) override
    {
        if (IsD2Class(player) && (IsClassMinion(guardian) || IsRevived(guardian)))
            ScaleMinion(player, guardian);
    }
};

class d2x_pet_world_entry : public PetScript
{
public:
    d2x_pet_world_entry() : PetScript("d2x_pet_world_entry", {PETHOOK_ON_PET_ADD_TO_WORLD}) { }

    void OnPetAddToWorld(Pet* pet) override
    {
        if (IsClassMinion(pet))
            if (Player* owner = pet->GetOwner(); owner && IsD2Class(owner))
                ScaleMinion(owner, pet);
    }
};

class d2x_minion_world_entry : public AllCreatureScript
{
public:
    d2x_minion_world_entry() : AllCreatureScript("d2x_minion_world_entry") { }

    void OnCreatureAddWorld(Creature* creature) override
    {
        if (!creature->IsGuardian() || (!IsClassMinion(creature) && !IsRevived(creature)))
            return;
        if (Unit* owner = creature->GetOwner())
            if (Player* player = owner->ToPlayer(); player && IsD2Class(player))
                ScaleMinion(player, static_cast<Guardian*>(creature));
    }
};

void AddSC_d2x_player()
{
    new d2x_player();
    new d2x_minion_world_entry();
    new d2x_pet_world_entry();
}
