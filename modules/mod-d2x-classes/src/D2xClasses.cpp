#include "D2xClasses.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectMgr.h"
#include "Log.h"
#include "Pet.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"

#include <algorithm>
#include <list>
#include <vector>

namespace d2x
{
namespace
{
    constexpr uint32 FIRST_CLASS_CREATURE = 950000;
    constexpr uint32 LAST_CLASS_CREATURE = 950999;
    // Diablo II revives a monster with double health. Its template multipliers count up to this cap, so a
    // revived elite stays a strong minion instead of a raid boss.
    constexpr float REVIVED_HEALTH = 2.0f;
    constexpr float REVIVED_TEMPLATE_CAP = 5.0f;

    struct Golem
    {
        uint32 creature;
        uint32 spell;
    };

    constexpr std::array<Golem, 4> GOLEMS =
    {{
        { nec::NPC_CLAY_GOLEM, nec::CLAY_GOLEM },
        { nec::NPC_BLOOD_GOLEM, nec::BLOOD_GOLEM },
        { nec::NPC_IRON_GOLEM, nec::IRON_GOLEM },
        { nec::NPC_FIRE_GOLEM, nec::FIRE_GOLEM },
    }};

    struct Strength
    {
        float health;
        float damage;
        float armor;
    };

    // creature_classlevelstats leaves some later-expansion columns lower than earlier ones.
    template<typename T>
    float BestUpTo(T const (&values)[MAX_EXPANSIONS], uint32 expansion)
    {
        uint32 const last = std::min<uint32>(expansion, MAX_EXPANSIONS - 1);
        return float(*std::max_element(values, values + last + 1));
    }

    Golem const* GolemOf(Creature const* creature)
    {
        auto golem = std::ranges::find(GOLEMS, creature->GetEntry(), &Golem::creature);
        return golem == GOLEMS.end() ? nullptr : &*golem;
    }

    // The golem skill's "percent of base health and damage", with the owner's ranks applied.
    float GolemStrength(Player const* owner, Golem const& golem)
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(golem.spell);
        return spell ? owner->CalculateSpellDamage(nullptr, spell, EFFECT_1) / 100.0f : 1.0f;
    }

    Strength StrengthOf(Player const* owner, Creature const* minion)
    {
        CreatureTemplate const* info = minion->GetCreatureTemplate();
        if (IsRevived(minion))
            return { REVIVED_HEALTH * std::min(info->ModHealth, REVIVED_TEMPLATE_CAP),
                     std::min(info->DamageModifier, REVIVED_TEMPLATE_CAP),
                     std::min(info->ModArmor, REVIVED_TEMPLATE_CAP) };

        Golem const* golem = GolemOf(minion);
        float const skill = golem ? GolemStrength(owner, *golem) : 1.0f;
        return { info->ModHealth * skill, info->DamageModifier * skill, info->ModArmor };
    }

    // What a creature of the minion's level, class and multipliers has, as Creature::SelectLevel and
    // Creature::CalculateMinMaxDamage would give it.
    void ApplyBaseStats(Player const* owner, Guardian* minion)
    {
        CreatureTemplate const* info = minion->GetCreatureTemplate();
        CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(minion->GetLevel(), info->unit_class);
        Strength const strength = StrengthOf(owner, minion);

        float const health = std::max(1.0f, BestUpTo(stats->BaseHealth, info->expansion) * strength.health);
        minion->SetCreateHealth(uint32(health));
        minion->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, health);
        minion->SetStatFlatModifier(UNIT_MOD_ARMOR, BASE_VALUE, stats->BaseArmor * strength.armor);

        float const attackSeconds = minion->GetAttackTime(BASE_ATTACK) / 1000.0f;
        float const weapon = BestUpTo(stats->BaseDamage, info->expansion);
        float const power = stats->AttackPower / 14.0f;
        minion->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, (weapon + power) * strength.damage * attackSeconds);
        minion->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, (1.5f * weapon + power) * strength.damage * attackSeconds);
        minion->UpdateAllStats();
    }

    void SetEffectAmount(Aura* aura, SpellEffIndex effect, int32 amount)
    {
        if (AuraEffect* aurEff = aura->GetEffect(effect))
            aurEff->ChangeAmount(amount);
    }

    // The masteries are dummy auras on the owner; "minion bonus" carries their sum onto the minion.
    void ApplyMasteries(Player* owner, Guardian* minion)
    {
        bool const golem = GolemOf(minion) != nullptr;
        int32 const damage = owner->GetTotalAuraModifierByMiscValue(SPELL_AURA_DUMMY,
            golem ? nec::GOLEM_MASTERY_EFFECT_2 : nec::SKELETON_MASTERY_EFFECT_1);
        int32 const health = owner->GetTotalAuraModifierByMiscValue(SPELL_AURA_DUMMY,
            golem ? nec::GOLEM_MASTERY_EFFECT_1 : nec::SKELETON_MASTERY_EFFECT_1);
        int32 const resist = owner->GetTotalAuraModifierByMiscValue(SPELL_AURA_DUMMY, nec::SUMMON_RESIST_EFFECT_1);

        minion->RemoveAurasDueToSpell(nec::MINION_BONUS);
        if (!damage && !health && !resist)
            return;

        if (Aura* bonus = owner->AddAura(nec::MINION_BONUS, minion))
        {
            SetEffectAmount(bonus, EFFECT_0, damage);
            SetEffectAmount(bonus, EFFECT_1, health);
            SetEffectAmount(bonus, EFFECT_2, -resist);
        }
    }

    bool WasPlayerOwned(Creature const* creature)
    {
        return creature->IsControlledByPlayer() || creature->GetCreatorGUID().IsPlayer() ||
               creature->GetOwnerGUID().IsPlayer();
    }

    bool HasFleshAndBone(Creature const* creature)
    {
        return !(creature->GetCreatureTypeMask() & CREATURE_TYPEMASK_MECHANICAL_OR_ELEMENTAL) &&
               !creature->IsCritter() && !creature->IsTotem() && !creature->IsTrigger() && !creature->IsVehicle();
    }
}

bool IsD2Class(Player const* player)
{
    return !SkillsOf(player).empty();
}

std::span<SkillIds const> SkillsOf(Player const* player)
{
    switch (player->getClass())
    {
        case CLASS_NECROMANCER: return nec::SKILLS;
        case CLASS_SORCERESS: return sor::SKILLS;
        default: return {};
    }
}

std::span<TalentPrereq const> ExtraPrereqsOf(Player const* player)
{
    switch (player->getClass())
    {
        case CLASS_NECROMANCER: return nec::EXTRA_PREREQS;
        case CLASS_SORCERESS: return sor::EXTRA_PREREQS;
        default: return {};
    }
}

std::span<uint32 const> TaughtSpellsOf(Player const* player)
{
    switch (player->getClass())
    {
        case CLASS_NECROMANCER: return nec::TAUGHT;
        case CLASS_SORCERESS: return sor::TAUGHT;
        default: return {};
    }
}

bool HasSkill(Player const* player, uint32 talentId)
{
    std::span<SkillIds const> const skills = SkillsOf(player);
    auto skill = std::ranges::find(skills, talentId, &SkillIds::talent);
    if (skill == skills.end())
        return false;

    uint8 const spec = player->GetActiveSpec();
    return std::ranges::any_of(skill->ranks, [&](uint32 rank) { return rank && player->HasTalent(rank, spec); });
}

bool IsClassMinion(Creature const* creature)
{
    uint32 const entry = creature->GetEntry();
    return entry >= FIRST_CLASS_CREATURE && entry <= LAST_CLASS_CREATURE;
}

bool IsRevived(Creature const* creature)
{
    return creature->GetUInt32Value(UNIT_CREATED_BY_SPELL) == nec::REVIVE && creature->GetOwnerGUID().IsPlayer();
}

void ScaleMinion(Player* owner, Guardian* minion)
{
    if (!minion->IsAlive())
        return;

    float const healthPct = minion->GetHealthPct();
    ApplyBaseStats(owner, minion);
    ApplyMasteries(owner, minion);
    minion->SetHealth(std::max<uint32>(1, CalculatePct(minion->GetMaxHealth(), healthPct)));

    // Recovered permanent pets can know a passive without carrying its aura.
    // Reapply the template passives after scaling and whenever they enter world.
    if (minion->IsPet() && GolemOf(minion))
        for (uint32 id : minion->GetCreatureTemplate()->spells)
            if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(id))
                if (spell->IsPassive() && !minion->HasAura(id))
                {
                    minion->CastSpell(minion, id, true);
                    LOG_INFO("module", "[WowPs PrivateMods] golem-passive={} entry={} reason=missing-persistent-aura", id, minion->GetEntry());
                }
}

void RescaleMinions(Player* owner)
{
    std::vector<Guardian*> minions;
    for (Unit* unit : owner->m_Controlled)
        if (unit->IsAlive() && unit->IsGuardian() && (IsClassMinion(unit->ToCreature()) || IsRevived(unit->ToCreature())))
            minions.push_back(static_cast<Guardian*>(unit));

    for (Guardian* minion : minions)
        ScaleMinion(owner, minion);
}

void KeepNewestMinions(Unit* owner, uint32 summonSpell, uint32 keep)
{
    std::vector<TempSummon*> minions;
    for (Unit* unit : owner->m_Controlled)
        if (unit->IsAlive() && unit->IsSummon() && unit->GetUInt32Value(UNIT_CREATED_BY_SPELL) == summonSpell)
            minions.push_back(unit->ToTempSummon());

    if (minions.size() <= keep)
        return;

    std::ranges::sort(minions, {}, [](TempSummon const* minion) { return minion->GetGUID().GetCounter(); });
    for (std::size_t oldest = 0; oldest < minions.size() - keep; ++oldest)
        minions[oldest]->UnSummon();
}

bool IsUsableCorpse(Unit* caster, Creature* corpse, float range, CorpseFilter accept)
{
    return corpse->getDeathState() == DeathState::Corpse && !IsClassMinion(corpse) && !WasPlayerOwned(corpse) &&
           HasFleshAndBone(corpse) && corpse->GetDisplayId() == corpse->GetNativeDisplayId() &&
           (!accept || accept(corpse)) && caster->IsWithinDistInMap(corpse, range) &&
           caster->IsWithinLOSInMap(corpse);
}

Creature* FindCorpse(Unit* caster, float range, CorpseFilter accept)
{
    if (Player* player = caster->ToPlayer())
        if (Unit* selected = player->GetSelectedUnit())
            if (Creature* corpse = selected->ToCreature(); corpse && IsUsableCorpse(caster, corpse, range, accept))
                return corpse;

    std::list<Creature*> corpses;
    caster->GetDeadCreatureListInGrid(corpses, range, true);

    Creature* nearest = nullptr;
    for (Creature* corpse : corpses)
        if (IsUsableCorpse(caster, corpse, range, accept) &&
            (!nearest || caster->GetExactDistSq(corpse) < caster->GetExactDistSq(nearest)))
            nearest = corpse;
    return nearest;
}

void ConsumeCorpse(Unit* caster, Creature* corpse)
{
    caster->CastSpell(corpse, SPELL_CORPSE_RUBBLE, true);
}
}
