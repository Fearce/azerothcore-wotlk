#include "D2xClasses.h"

#include "CellImpl.h"
#include "Containers.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "TemporarySummon.h"

#include <algorithm>
#include <list>

using namespace d2x;

namespace
{
    // How far a confused monster looks for another monster to attack.
    constexpr float CONFUSE_REACH = 15.0f;

    // Monsters that keep a threat list: no players, nothing a player owns, no critters, totems or triggers.
    std::list<Creature*> MonstersNear(Unit* center, float range)
    {
        std::list<Creature*> monsters;
        Acore::AnyUnitInObjectRangeCheck check(center, range);
        Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(center, monsters, check);
        Cell::VisitObjects(center, searcher, range);
        monsters.remove_if([center](Creature* monster)
        {
            return monster == center || monster->IsCharmedOwnedByPlayerOrPlayer() || monster->IsCritter() ||
                   monster->IsTotem() || monster->IsTrigger() || !monster->CanHaveThreatList();
        });
        return monsters;
    }

    // The monster attacks `target` above everything on its threat list until the fixate is cleared.
    void Fixate(Creature* monster, Unit* target)
    {
        monster->GetThreatMgr().AddThreat(target, 0.0f, nullptr, true, true);
        monster->GetThreatMgr().FixateTarget(target);
    }

    // Revive leaves bosses, rares and anyone offering a service in the ground.
    bool CanRevive(Creature const* corpse)
    {
        CreatureTemplate const* info = corpse->GetCreatureTemplate();
        return (info->rank == CREATURE_ELITE_NORMAL || info->rank == CREATURE_ELITE_ELITE) && !info->npcflag &&
               !corpse->isWorldBoss() && !corpse->IsDungeonBoss();
    }
}

// A corpse skill: its main effect is a dummy on the caster whose radius is how far it looks for a corpse.
class CorpseSkillScript : public SpellScript
{
protected:
    Creature* NearbyCorpse(CorpseFilter accept = nullptr)
    {
        return FindCorpse(GetCaster(), GetSpellInfo()->Effects[EFFECT_0].CalcRadius(GetCaster()), accept);
    }

    SpellCastResult RequireCorpse(CorpseFilter accept = nullptr)
    {
        if (NearbyCorpse(accept))
            return SPELL_CAST_OK;

        SetCustomCastResultMessage(SPELL_CUSTOM_ERROR_NO_NEARBY_CORPSES);
        return SPELL_FAILED_CUSTOM_ERROR;
    }
};

// Raise Skeleton and Raise Skeletal Mage.
class spell_d2x_raise : public CorpseSkillScript
{
    PrepareSpellScript(spell_d2x_raise);

public:
    explicit spell_d2x_raise(uint32 summonSpell) : _summonSpell(summonSpell) { }

private:
    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ _summonSpell, SPELL_CORPSE_RUBBLE });
    }

    SpellCastResult CheckCorpse()
    {
        return RequireCorpse();
    }

    // The effect value is how many of this minion the caster keeps.
    void HandleRaise(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Creature* corpse = NearbyCorpse();
        if (!corpse)
            return;

        KeepNewestMinions(caster, _summonSpell, std::max(GetEffectValue(), 1) - 1);
        ConsumeCorpse(caster, corpse);
        caster->CastSpell(corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ(), _summonSpell, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_d2x_raise::CheckCorpse);
        OnEffectHitTarget += SpellEffectFn(spell_d2x_raise::HandleRaise, EFFECT_0, SPELL_EFFECT_DUMMY);
    }

    uint32 _summonSpell;
};

// Corpse Explosion and Poison Explosion: the corpse becomes the centre of the burst spell.
class spell_d2x_corpse_burst : public CorpseSkillScript
{
    PrepareSpellScript(spell_d2x_corpse_burst);

public:
    explicit spell_d2x_corpse_burst(uint32 burstSpell) : _burstSpell(burstSpell) { }

private:
    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ _burstSpell, SPELL_CORPSE_RUBBLE });
    }

    SpellCastResult CheckCorpse()
    {
        return RequireCorpse();
    }

    void HandleBurst(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Creature* corpse = NearbyCorpse();
        if (!corpse)
            return;

        ConsumeCorpse(caster, corpse);
        caster->CastSpell(corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ(), _burstSpell, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_d2x_corpse_burst::CheckCorpse);
        OnEffectHitTarget += SpellEffectFn(spell_d2x_corpse_burst::HandleBurst, EFFECT_0, SPELL_EFFECT_DUMMY);
    }

    uint32 _burstSpell;
};

// Revive summons the slain monster's own entry for the spell's duration; the module scales it like any
// other minion and gives it the minion AI.
class spell_d2x_revive : public CorpseSkillScript
{
    PrepareSpellScript(spell_d2x_revive);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_CORPSE_RUBBLE }) && sSummonPropertiesStore.LookupEntry(SUMMON_PROPERTIES_GUARDIAN);
    }

    SpellCastResult CheckCorpse()
    {
        return RequireCorpse(CanRevive);
    }

    void HandleRevive(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Creature* corpse = NearbyCorpse(CanRevive);
        if (!corpse)
            return;

        KeepNewestMinions(caster, nec::REVIVE, std::max(GetEffectValue(), 1) - 1);
        TempSummon* revived = caster->GetMap()->SummonCreature(corpse->GetEntry(), *corpse,
            sSummonPropertiesStore.LookupEntry(SUMMON_PROPERTIES_GUARDIAN), Duration(), caster, nec::REVIVE);
        if (!revived)
            return;

        // Its first death already paid out loot and experience.
        revived->SetLootRewardDisabled(true);
        LookLike(revived, corpse);
        // Event monsters can carry immunities that would keep them out of the fight.
        revived->RemoveUnitFlag(UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_NON_ATTACKABLE);
        if (!AttachRevivedAI(revived))
        {
            revived->UnSummon();
            return;
        }
        ConsumeCorpse(caster, corpse);
        Follow(caster, revived);
    }

    uint32 Duration()
    {
        int32 duration = GetSpellInfo()->GetDuration();
        if (Player* modOwner = GetCaster()->GetSpellModOwner())
            modOwner->ApplySpellMod(GetSpellInfo()->Id, SPELLMOD_DURATION, duration);
        return uint32(std::max(duration, 0));
    }

    static void LookLike(TempSummon* revived, Creature* corpse)
    {
        uint32 const display = corpse->GetNativeDisplayId();
        revived->SetNativeDisplayId(display);
        revived->SetDisplayId(display, corpse->GetObjectScale());
        revived->LoadEquipment(corpse->GetCurrentEquipmentId(), true);
    }

    // What Spell::SummonGuardian does for the minions a spell effect summons.
    static void Follow(Unit* caster, TempSummon* revived)
    {
        if (!revived->HasUnitTypeMask(UNIT_MASK_MINION))
            return;

        Minion* minion = static_cast<Minion*>(revived);
        minion->SetFollowAngle(caster->GetAbsoluteAngle(minion));
        minion->GetMotionMaster()->Clear(false);
        minion->GetMotionMaster()->MoveFollow(caster, PET_FOLLOW_DIST, minion->GetFollowAngle(), MOTION_SLOT_ACTIVE);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_d2x_revive::CheckCorpse);
        OnEffectHitTarget += SpellEffectFn(spell_d2x_revive::HandleRevive, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Each melee blow a cursed unit lands hurts it for a share of the damage.
class spell_d2x_iron_maiden : public AuraScript
{
    PrepareAuraScript(spell_d2x_iron_maiden);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ nec::IRON_MAIDEN_REFLECT });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo* damage = eventInfo.GetDamageInfo();
        Unit* caster = GetCaster();
        if (!damage || !damage->GetDamage() || !caster)
            return;

        int32 const amount = CalculatePct(damage->GetDamage(), aurEff->GetAmount());
        caster->CastCustomSpell(GetTarget(), nec::IRON_MAIDEN_REFLECT, &amount, nullptr, nullptr, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_d2x_iron_maiden::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Players and their minions that strike a cursed unit in melee heal for a share of the damage.
class spell_d2x_life_tap : public AuraScript
{
    PrepareAuraScript(spell_d2x_life_tap);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ nec::LIFE_TAP_HEAL });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo* damage = eventInfo.GetDamageInfo();
        Unit* attacker = eventInfo.GetActor();
        if (!damage || !damage->GetDamage() || !attacker || !attacker->IsAlive() || !attacker->IsControlledByPlayer())
            return;

        // Preserve curse provenance so the access gate can authorize a teammate's
        // triggered self-heal without granting that teammate a custom spell.
        int32 const amount = CalculatePct(damage->GetDamage(), aurEff->GetAmount());
        attacker->CastCustomSpell(attacker, nec::LIFE_TAP_HEAL, &amount, nullptr, nullptr, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_d2x_life_tap::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// The blood golem's blows heal it and its owner.
class spell_d2x_blood_golem : public AuraScript
{
    PrepareAuraScript(spell_d2x_blood_golem);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ nec::BLOOD_GOLEM_HEAL });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo* damage = eventInfo.GetDamageInfo();
        if (!damage || !damage->GetDamage())
            return;

        Unit* golem = GetTarget();
        int32 const amount = CalculatePct(damage->GetDamage(), aurEff->GetAmount());
        golem->CastCustomSpell(golem, nec::BLOOD_GOLEM_HEAL, &amount, nullptr, nullptr, true);
        if (Unit* owner = golem->GetOwner(); owner && owner->IsAlive())
            golem->CastCustomSpell(owner, nec::BLOOD_GOLEM_HEAL, &amount, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_d2x_blood_golem::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Confuse curses monsters only: its faction change on a player would hand that player to the server.
class spell_d2x_confuse : public SpellScript
{
    PrepareSpellScript(spell_d2x_confuse);

    void KeepMonsters(std::list<WorldObject*>& targets)
    {
        targets.remove_if([](WorldObject* target)
        {
            Unit* unit = target->ToUnit();
            return !unit || unit->IsCharmedOwnedByPlayerOrPlayer();
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_d2x_confuse::KeepMonsters, EFFECT_0,
                                                                  TARGET_UNIT_DEST_AREA_ENEMY);
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_d2x_confuse::KeepMonsters, EFFECT_1,
                                                                  TARGET_UNIT_DEST_AREA_ENEMY);
    }
};

// A confused monster attacks a random monster near it, and another when that one falls.
class spell_d2x_confuse_aura : public AuraScript
{
    PrepareAuraScript(spell_d2x_confuse_aura);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        PickVictim();
    }

    void HandleTick(AuraEffect const* /*aurEff*/)
    {
        Unit* victim = GetTarget()->GetThreatMgr().GetFixateTarget();
        if (!victim || !victim->IsAlive() || !GetTarget()->IsValidAttackTarget(victim))
            PickVictim();
    }

    // Runs after the faction is restored, so former allies stop fighting.
    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* confused = GetTarget();
        if (Unit* victim = confused->GetThreatMgr().GetFixateTarget(); victim && victim->GetGUID() == _victim)
            confused->GetThreatMgr().ClearFixate();
        confused->GetCombatManager().RevalidateCombat();
    }

    void PickVictim()
    {
        Creature* confused = GetTarget()->ToCreature();
        if (!confused)
            return;

        std::list<Creature*> monsters = MonstersNear(confused, CONFUSE_REACH);
        monsters.remove_if([confused](Creature* monster)
        {
            return !confused->IsValidAttackTarget(monster) || !confused->IsWithinLOSInMap(monster);
        });
        if (monsters.empty())
            return;

        Creature* victim = Acore::Containers::SelectRandomContainerElement(monsters);
        Fixate(confused, victim);
        _victim = victim->GetGUID();
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_d2x_confuse_aura::HandleApply, EFFECT_0, SPELL_AURA_MOD_FACTION,
                                              AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_d2x_confuse_aura::HandleTick, EFFECT_1,
                                                 SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_d2x_confuse_aura::HandleRemove, EFFECT_0,
                                                SPELL_AURA_MOD_FACTION, AURA_EFFECT_HANDLE_REAL);
    }

    ObjectGuid _victim;
};

// Attract changes its target's faction too, so it refuses players and what they own.
class spell_d2x_attract : public SpellScript
{
    PrepareSpellScript(spell_d2x_attract);

    SpellCastResult CheckTarget()
    {
        Unit* target = GetExplTargetUnit();
        return target && target->IsCharmedOwnedByPlayerOrPlayer() ? SPELL_FAILED_BAD_TARGETS : SPELL_CAST_OK;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_d2x_attract::CheckTarget);
    }
};

// Every tick, the monsters around the cursed one turn on it.
class spell_d2x_attract_aura : public AuraScript
{
    PrepareAuraScript(spell_d2x_attract_aura);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        PullMonsters();
    }

    void HandleTick(AuraEffect const* /*aurEff*/)
    {
        PullMonsters();
    }

    // Runs after the faction is restored, so former allies stop fighting.
    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* cursed = GetTarget();
        for (Creature* monster : MonstersNear(cursed, 2 * Reach()))
            if (monster->GetThreatMgr().GetFixateTarget() == cursed)
                monster->GetThreatMgr().ClearFixate();
        cursed->GetCombatManager().RevalidateCombat();
    }

    void PullMonsters()
    {
        Unit* cursed = GetTarget();
        for (Creature* monster : MonstersNear(cursed, Reach()))
            if (!monster->IsInEvadeMode() && monster->IsValidAttackTarget(cursed))
                Fixate(monster, cursed);
    }

    float Reach()
    {
        return GetSpellInfo()->Effects[EFFECT_1].CalcRadius(GetCaster());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_d2x_attract_aura::HandleApply, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY,
                                              AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_d2x_attract_aura::HandleTick, EFFECT_1,
                                                 SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_d2x_attract_aura::HandleRemove, EFFECT_0,
                                                SPELL_AURA_MOD_FACTION, AURA_EFFECT_HANDLE_REAL);
    }
};

void AddSC_d2x_necromancer_spells()
{
    RegisterSpellScriptWithArgs(spell_d2x_raise, "spell_d2x_raise_skeleton", nec::RAISE_SKELETON_SUMMON);
    RegisterSpellScriptWithArgs(spell_d2x_raise, "spell_d2x_raise_skeletal_mage", nec::RAISE_SKELETAL_MAGE_SUMMON);
    RegisterSpellScriptWithArgs(spell_d2x_corpse_burst, "spell_d2x_corpse_explosion", nec::CORPSE_EXPLOSION_BLAST);
    RegisterSpellScriptWithArgs(spell_d2x_corpse_burst, "spell_d2x_poison_explosion", nec::POISON_EXPLOSION_CLOUD);
    RegisterSpellScript(spell_d2x_revive);
    RegisterSpellScript(spell_d2x_iron_maiden);
    RegisterSpellScript(spell_d2x_life_tap);
    RegisterSpellScript(spell_d2x_blood_golem);
    RegisterSpellAndAuraScriptPair(spell_d2x_confuse, spell_d2x_confuse_aura);
    RegisterSpellAndAuraScriptPair(spell_d2x_attract, spell_d2x_attract_aura);
}
