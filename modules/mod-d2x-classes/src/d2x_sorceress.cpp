#include "D2xClasses.h"

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "TemporarySummon.h"

#include <algorithm>
#include <cmath>
#include <list>

using namespace d2x;

namespace
{
    // Fire Wall: segments on each side of the centre one, and the distance between segment centres.
    constexpr int32 FIRE_WALL_SIDE_SEGMENTS = 2;
    constexpr float FIRE_WALL_SPACING = 3.0f;
    // Frozen Orb sheds frost this many times on its way, evenly spaced, before it bursts.
    constexpr uint32 FROZEN_ORB_PULSES = 4;
}

// Static Field takes its share of each enemy's current health, no more than its damage and never below
// its health floor.
class spell_d2x_static_field : public SpellScript
{
    PrepareSpellScript(spell_d2x_static_field);

    void LimitDamage()
    {
        Unit* target = GetHitUnit();
        if (!target || GetHitDamage() <= 0)
            return;

        Unit* caster = GetCaster();
        int32 const share = caster->CalculateSpellDamage(target, GetSpellInfo(), EFFECT_1);
        int32 const floorPct = caster->CalculateSpellDamage(target, GetSpellInfo(), EFFECT_2);
        int64 const health = target->GetHealth();
        int64 const floor = CalculatePct(int64(target->GetMaxHealth()), floorPct);
        int64 const limit = health > floor ? std::min(CalculatePct(health, share), health - floor) : 0;
        SetHitDamage(int32(std::min<int64>(GetHitDamage(), limit)));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_d2x_static_field::LimitDamage);
    }
};

// Fire Wall: a row of burning segments across the target point, square to the caster's line to it.
class spell_d2x_fire_wall : public SpellScript
{
    PrepareSpellScript(spell_d2x_fire_wall);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ sor::FIRE_WALL_WALL });
    }

    void HandleWall(SpellEffIndex /*effIndex*/)
    {
        WorldLocation const* centre = GetHitDest();
        if (!centre)
            return;

        Unit* caster = GetCaster();
        float const across = caster->GetAbsoluteAngle(centre) + float(M_PI) / 2;
        for (int32 step = -FIRE_WALL_SIDE_SEGMENTS; step <= FIRE_WALL_SIDE_SEGMENTS; ++step)
        {
            float const x = centre->GetPositionX() + step * FIRE_WALL_SPACING * std::cos(across);
            float const y = centre->GetPositionY() + step * FIRE_WALL_SPACING * std::sin(across);
            float z = centre->GetPositionZ();
            caster->UpdateGroundPositionZ(x, y, z);
            caster->CastSpell(x, y, z, sor::FIRE_WALL_WALL, true);
        }
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_d2x_fire_wall::HandleWall, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Thunder Storm strikes enemies that are fighting the sorceress or hostile to her, never neutral wildlife.
// The core then picks one of them at random.
class spell_d2x_thunder_storm : public SpellScript
{
    PrepareSpellScript(spell_d2x_thunder_storm);

    void KeepFoes(std::list<WorldObject*>& targets)
    {
        Unit* caster = GetCaster();
        targets.remove_if([caster](WorldObject* object)
        {
            Unit* unit = object->ToUnit();
            return !unit || unit->IsCritter() || !(unit->IsInCombatWith(caster) || unit->IsHostileTo(caster));
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_d2x_thunder_storm::KeepFoes, EFFECT_0,
                                                                  TARGET_UNIT_SRC_AREA_ENEMY);
    }
};

// Energy Shield is an endless mana shield that takes only its share of each hit.
class spell_d2x_energy_shield : public AuraScript
{
    PrepareAuraScript(spell_d2x_energy_shield);

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        AuraEffect const* share = GetEffect(EFFECT_1);
        absorbAmount = share ? CalculatePct(dmgInfo.GetDamage(), share->GetAmount()) : 0;
    }

    void Register() override
    {
        OnEffectManaShield += AuraEffectManaShieldFn(spell_d2x_energy_shield::Absorb, EFFECT_0);
    }
};

// Casting Hydra again replaces the hydras already out.
class spell_d2x_hydra : public SpellScript
{
    PrepareSpellScript(spell_d2x_hydra);

    void ReplaceHydras()
    {
        std::list<Creature*> hydras;
        GetCaster()->GetAllMinionsByEntry(hydras, sor::NPC_HYDRA);
        for (Creature* hydra : hydras)
            hydra->ToTempSummon()->UnSummon();
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_d2x_hydra::ReplaceHydras);
    }
};

// Frozen Orb sheds frost on its way: evenly spaced pulses between the caster and the burst point, each
// timed to when the orb passes it.
class spell_d2x_frozen_orb : public SpellScript
{
    PrepareSpellScript(spell_d2x_frozen_orb);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo->Speed > 0.0f && ValidateSpellInfo({ sor::FROZEN_ORB_PULSE });
    }

    void ShedFrost()
    {
        WorldLocation const* burst = GetExplTargetDest();
        if (!burst)
            return;

        Unit* caster = GetCaster();
        Position const start = caster->GetPosition();
        float const flightMs = caster->GetExactDist(burst) / GetSpellInfo()->Speed * IN_MILLISECONDS;
        for (uint32 pulse = 1; pulse <= FROZEN_ORB_PULSES; ++pulse)
        {
            float const along = float(pulse) / (FROZEN_ORB_PULSES + 1);
            Position const point(start.GetPositionX() + along * (burst->GetPositionX() - start.GetPositionX()),
                                 start.GetPositionY() + along * (burst->GetPositionY() - start.GetPositionY()),
                                 start.GetPositionZ() + along * (burst->GetPositionZ() - start.GetPositionZ()));
            ShedFrostAt(caster, point, Milliseconds(uint32(along * flightMs)));
        }
    }

    // The event belongs to the caster, so it dies with the caster; a change of map makes the point
    // meaningless.
    static void ShedFrostAt(Unit* caster, Position const& point, Milliseconds delay)
    {
        uint32 const mapId = caster->GetMapId();
        caster->m_Events.AddEventAtOffset([caster, point, mapId]()
        {
            if (!caster->IsInWorld() || caster->GetMapId() != mapId)
                return;

            float z = point.GetPositionZ();
            caster->UpdateGroundPositionZ(point.GetPositionX(), point.GetPositionY(), z);
            caster->CastSpell(point.GetPositionX(), point.GetPositionY(), z, sor::FROZEN_ORB_PULSE, true);
        }, delay);
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_d2x_frozen_orb::ShedFrost);
    }
};

void AddSC_d2x_sorceress_spells()
{
    RegisterSpellScript(spell_d2x_static_field);
    RegisterSpellScript(spell_d2x_fire_wall);
    RegisterSpellScript(spell_d2x_thunder_storm);
    RegisterSpellScript(spell_d2x_energy_shield);
    RegisterSpellScript(spell_d2x_hydra);
    RegisterSpellScript(spell_d2x_frozen_orb);
}
