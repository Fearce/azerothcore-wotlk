#include "D2xClasses.h"

#include "Chat.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellScript.h"

#include <algorithm>
#include <list>

using namespace d2x;

namespace
{
    // Half the width of the path Bone Spear and Lightning cut through their target.
    constexpr float LINE_HALF_WIDTH = 2.5f;
}

// Bone Armor: the shield adds spell power times its coefficient, as the stock absorb shields do. Growth
// ranks raise the whole shield; damage synergies raise the base value and the coefficient separately.
class spell_d2x_absorb : public AuraScript
{
    PrepareAuraScript(spell_d2x_absorb);

    void CalculateAmount(AuraEffect const* aurEff, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        Unit* caster = GetCaster();
        if (!caster)
            return;

        SpellInfo const* spell = GetSpellInfo();
        Player* modOwner = caster->GetSpellModOwner();
        float coeff = spell->Effects[aurEff->GetEffIndex()].BonusMultiplier * 100.0f;
        if (modOwner)
            modOwner->ApplySpellMod(spell->Id, SPELLMOD_BONUS_MULTIPLIER, coeff);

        float bonus = coeff / 100.0f * caster->SpellBaseDamageBonusDone(spell->GetSchoolMask()) *
                      caster->CalculateLevelPenalty(spell);
        if (modOwner)
            modOwner->ApplySpellMod(spell->Id, SPELLMOD_ALL_EFFECTS, bonus);
        amount += int32(bonus);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_d2x_absorb::CalculateAmount, EFFECT_0,
                                                     SPELL_AURA_SCHOOL_ABSORB);
    }
};

// Bone Spear and Lightning: the area effect strikes only the enemies on the line from the caster through
// the target, out to its radius.
class spell_d2x_line : public SpellScript
{
    PrepareSpellScript(spell_d2x_line);

    void KeepLine(std::list<WorldObject*>& targets)
    {
        Unit* target = GetExplTargetUnit();
        if (!target)
        {
            targets.clear();
            return;
        }

        Position line = GetCaster()->GetPosition();
        line.SetOrientation(GetCaster()->GetAbsoluteAngle(target));
        targets.remove_if([&](WorldObject* object)
        {
            return object != target && !line.HasInLine(object, object->GetObjectSize(), LINE_HALF_WIDTH);
        });

        // Combat reach lets the cast reach a target just past the area's radius.
        if (std::ranges::find(targets, target) == targets.end())
            targets.push_back(target);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_d2x_line::KeepLine, EFFECT_0,
                                                                  TARGET_UNIT_SRC_AREA_ENEMY);
    }
};

// Absolution: both classes reset their own talents, since no trainer of theirs exists to do it.
class spell_d2x_absolution : public SpellScript
{
    PrepareSpellScript(spell_d2x_absolution);

    SpellCastResult CheckTalents()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return SPELL_FAILED_DONT_REPORT;

        std::span<SkillIds const> const skills = SkillsOf(player);
        if (std::ranges::any_of(skills, [player](SkillIds const& skill) { return HasSkill(player, skill.talent); }))
            return SPELL_CAST_OK;

        ChatHandler(player->GetSession()).SendNotification(std::string_view("You have no talents to forget."));
        return SPELL_FAILED_DONT_REPORT;
    }

    void HandleReset(SpellEffIndex /*effIndex*/)
    {
        if (Player* player = GetCaster()->ToPlayer())
        {
            player->resetTalents(true);
            player->SendTalentsInfoData(false);
        }
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_d2x_absolution::CheckTalents);
        OnEffectHitTarget += SpellEffectFn(spell_d2x_absolution::HandleReset, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

void AddSC_d2x_shared_spells()
{
    RegisterSpellScript(spell_d2x_absorb);
    RegisterSpellScript(spell_d2x_line);
    RegisterSpellScript(spell_d2x_absolution);
}
