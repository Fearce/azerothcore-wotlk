#include "D2xClasses.h"

#include "Containers.h"
#include "CreatureAI.h"
#include "Log.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"

#include <vector>
#include <memory>

using namespace d2x;

namespace
{
    constexpr float CASTER_DISTANCE = 20.0f;
    constexpr uint32 BOLT_PAUSE_MIN_MS = 500;
    constexpr uint32 BOLT_PAUSE_MAX_MS = 1500;
    constexpr uint32 BOLT_RETRY_MS = 2000;

    // A spell a minion can keep hurling at its victim: damage on one enemy at range, with nothing that
    // summons, heals or reaches anyone else. A revived monster brings its template's spells along.
    bool IsBolt(SpellInfo const* spell)
    {
        if (!spell || spell->IsPassive() || spell->IsChanneled() || spell->GetMaxRange(false) < CASTER_DISTANCE)
            return false;

        bool damages = false;
        for (SpellEffectInfo const& effect : spell->Effects)
        {
            if (!effect.Effect)
                continue;
            if (effect.TargetA.GetTarget() != TARGET_UNIT_TARGET_ENEMY || effect.TargetB.GetTarget())
                return false;
            if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE ||
                (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE))
                damages = true;
            else if (effect.Effect != SPELL_EFFECT_APPLY_AURA)
                return false;
        }
        return damages;
    }

    // Each skeletal mage casts one element, as in Diablo II.
    uint32 PickBolt(Creature const* creature)
    {
        std::vector<uint32> bolts;
        for (uint32 spell : creature->m_spells)
            if (spell && IsBolt(sSpellMgr->GetSpellInfo(spell)))
                bolts.push_back(spell);
        return bolts.empty() ? 0 : Acore::Containers::SelectRandomContainerElement(bolts);
    }
}

// Skeletons, skeletal mages and revived monsters. They fight what their owner fights and defend the owner,
// but never pull monsters on their own.
class npc_d2x_minion : public CreatureAI
{
public:
    explicit npc_d2x_minion(Creature* creature) : CreatureAI(creature), _bolt(PickBolt(creature)) { }

    bool CanAIAttack(Unit const* target) const override
    {
        Unit* owner = me->GetOwner();
        return !owner || target->IsInCombatWith(owner) || target->IsInCombatWith(me);
    }

    // CanAIAttack can refuse the owner's target until the owner's spell lands, so engage directly.
    void OwnerAttacked(Unit* target) override
    {
        Engage(target);
    }

    void OwnerAttackedBy(Unit* attacker) override
    {
        if (!me->GetVictim())
            Engage(attacker);
    }

    void AttackStart(Unit* target) override
    {
        if (_bolt)
            AttackStartCaster(target, CASTER_DISTANCE);
        else
            CreatureAI::AttackStart(target);
    }

    void UpdateAI(uint32 diff) override
    {
        // A summon can arrive after its owner started attacking. It must not
        // depend on receiving the one-shot OwnerAttacked notification.
        if (!me->GetVictim())
            if (Unit* owner = me->GetOwner())
                Engage(owner->GetVictim());
        if (!UpdateVictim())
        {
            // Chasing a victim no attack has reached yet: start the fight so UpdateVictim keeps it.
            if (Unit* victim = me->GetVictim(); victim && me->IsValidAttackTarget(victim))
                me->EngageWithTarget(victim);
            return;
        }

        _boltPause = _boltPause > diff ? _boltPause - diff : 0;
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;
        if (_bolt && !_boltPause && CastBolt())
            return;
        DoMeleeAttackIfReady();
    }

private:
    void Engage(Unit* target)
    {
        if (target && me->IsAlive() && !me->HasReactState(REACT_PASSIVE) && me->IsValidAttackTarget(target))
            AttackStart(target);
    }

    bool CastBolt()
    {
        Unit* victim = me->GetVictim();
        if (!me->IsWithinDistInMap(victim, sSpellMgr->AssertSpellInfo(_bolt)->GetMaxRange(false)) ||
            !me->IsWithinLOSInMap(victim))
            return false;

        // Out of mana or on cooldown: melee meanwhile and try again shortly.
        bool const cast = DoCast(victim, _bolt) == SPELL_CAST_OK;
        _boltPause = cast ? urand(BOLT_PAUSE_MIN_MS, BOLT_PAUSE_MAX_MS) : BOLT_RETRY_MS;
        return cast;
    }

    uint32 const _bolt;
    uint32 _boltPause = 0;
};

bool d2x::AttachRevivedAI(Creature* creature)
{
    auto ai = std::make_unique<npc_d2x_minion>(creature);
    bool const attached = creature->AIM_Initialize(ai.get());
    if (attached)
        ai.release();
    LOG_INFO("module", "[WowPs PrivateMods] revive-ai={} entry={} reason=explicit-summon-initialization", attached, creature->GetEntry());
    return attached;
}

// Revive keeps the monster's own entry, so its ScriptName and AIName would drive it; this takes over first.
class d2x_revived_minions : public AllCreatureScript
{
public:
    d2x_revived_minions() : AllCreatureScript("d2x_revived_minions") { }

    CreatureAI* GetCreatureAI(Creature* creature) const override
    {
        return IsRevived(creature) ? new npc_d2x_minion(creature) : nullptr;
    }
};

void AddSC_d2x_minions()
{
    RegisterCreatureAI(npc_d2x_minion);
    new d2x_revived_minions();
}
