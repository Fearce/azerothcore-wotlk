#ifndef D2X_CLASSES_H
#define D2X_CLASSES_H

#include "D2xIds.h"

#include <span>

class Creature;
class Guardian;
class Player;
class Unit;

namespace d2x
{
    // The Death Knight's "Corpse Exploded" transform: a corpse wearing it has been used.
    constexpr uint32 SPELL_CORPSE_RUBBLE = 51270;

    bool IsD2Class(Player const* player);
    std::span<SkillIds const> SkillsOf(Player const* player);
    std::span<TalentPrereq const> ExtraPrereqsOf(Player const* player);
    std::span<uint32 const> TaughtSpellsOf(Player const* player);
    // Whether the active spec holds any rank of the skill behind `talentId`.
    bool HasSkill(Player const* player, uint32 talentId);

    // A creature the class spells summon, or a monster Revive brought back for a player.
    bool IsClassMinion(Creature const* creature);
    bool IsRevived(Creature const* creature);
    bool AttachRevivedAI(Creature* creature);

    // Sets a minion's health, armor and damage from its template and the owner's skills. Absolute, so it
    // can run again whenever the owner's talents change; the minion keeps its share of health.
    void ScaleMinion(Player* owner, Guardian* minion);
    void RescaleMinions(Player* owner);
    // Unsummons the oldest minions `owner` summoned with `summonSpell` until `keep` remain.
    void KeepNewestMinions(Unit* owner, uint32 summonSpell, uint32 keep);

    // A spell's own condition on the corpses it may use, on top of IsUsableCorpse.
    using CorpseFilter = bool (*)(Creature const* corpse);

    // The caster's selected corpse when it qualifies, else the nearest one that does.
    Creature* FindCorpse(Unit* caster, float range, CorpseFilter accept = nullptr);
    bool IsUsableCorpse(Unit* caster, Creature* corpse, float range, CorpseFilter accept = nullptr);
    void ConsumeCorpse(Unit* caster, Creature* corpse);
}

#endif
