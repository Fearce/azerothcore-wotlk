/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CreatureScript.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "culling_of_stratholme.h"

enum Spells
{
    SPELL_CARRION_SWARM                         = 52720,
    SPELL_MIND_BLAST                            = 52722,
    SPELL_SLEEP                                 = 52721,
    SPELL_VAMPIRIC_TOUCH                        = 52723,

    // Cast on defeat: this is the ENCOUNTER_CREDIT_CAST_SPELL for the CoS
    // encounter (instance_encounters 296/300), so casting it completes the
    // dungeon -> loot lockout + LFG reward (Emblems of Frost).
    SPELL_MALGANIS_KILL_CREDIT                  = 58630,
};

enum Misc
{
    NPC_MALGANIS_KC_BUNNY                       = 31006, // "A Royal Escort" quest credit
};

enum Events
{
    EVENT_SPELL_CARRION_SWARM                   = 1,
    EVENT_SPELL_MIND_BLAST                      = 2,
    EVENT_SPELL_SLEEP                           = 3,
    EVENT_SPELL_VAMPIRIC_TOUCH                  = 4,
};

enum Yells
{
    SAY_AGGRO                                   = 2,
    SAY_KILL                                    = 3,
    SAY_SLAY                                    = 4,
    SAY_SLEEP                                   = 5,
    SAY_30HEALTH                                = 6,
    SAY_15HEALTH                                = 7,
    SAY_ESCAPE_SPEECH_1                         = 8,
    SAY_ESCAPE_SPEECH_2                         = 9,
    SAY_OUTRO                                   = 10
};

class boss_mal_ganis : public CreatureScript
{
public:
    boss_mal_ganis() : CreatureScript("boss_mal_ganis") { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return GetCullingOfStratholmeAI<boss_mal_ganisAI>(creature);
    }

    struct boss_mal_ganisAI : public ScriptedAI
    {
        boss_mal_ganisAI(Creature* c) : ScriptedAI(c)
        {
            finished = false;
        }

        EventMap events;
        bool finished;

        void Reset() override
        {
            me->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_KNOCK_BACK, true);
            me->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_KNOCK_BACK_DEST, true);
            events.Reset();
            if (finished)
            {
                Talk(SAY_OUTRO);
                me->DespawnOrUnsummon(20s);
            }
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            Talk(SAY_AGGRO);
            events.ScheduleEvent(EVENT_SPELL_CARRION_SWARM, 6s);
            events.ScheduleEvent(EVENT_SPELL_MIND_BLAST, 11s);
            events.ScheduleEvent(EVENT_SPELL_SLEEP, 20s);
            events.ScheduleEvent(EVENT_SPELL_VAMPIRIC_TOUCH, 15s);
        }

        void JustDied(Unit* killer) override
        {
            // Mal'Ganis is meant to survive the killing blow: DamageTaken
            // absorbs it and plays the escape event. On a high-burst group the
            // lethal hit can still slip past that absorb (an overkill batch, or
            // a DoT ticking after he is already low), leaving him simply dead.
            // When that happens his credit spell is never cast, so the encounter
            // never completes: no loot chest, no "A Royal Escort" credit and no
            // Emblems of Frost. Run the same completion from the death so the
            // reward is never lost. Gate on the final act so bursting the
            // city-intro copy of Mal'Ganis can't credit the dungeon early.
            if (!finished)
                if (InstanceScript* instance = me->GetInstanceScript())
                    if (instance->GetData(DATA_ARTHAS_EVENT) >= COS_PROGRESS_BEFORE_MALGANIS)
                        FinishEncounter(killer, false);
        }

        void KilledUnit(Unit*  /*victim*/) override
        {
            if (!urand(0, 1))
                return;

            Talk(SAY_SLAY);
        }

        void DamageTaken(Unit* who, uint32& damage, DamageEffectType, SpellSchoolMask) override
        {
            if (!finished && damage >= me->GetHealth())
            {
                damage = 0;
                me->SetRegeneratingHealth(false);
                me->SetImmuneToAll(true);
                me->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
                me->SetReactState(REACT_PASSIVE);
                FinishEncounter(who, true);
                EnterEvadeMode();
            }
        }

        // Grants everything tied to Mal'Ganis' defeat exactly once: the dungeon
        // encounter credit (loot lockout + LFG reward / Emblems of Frost), the
        // loot chest, Arthas' outro, and the "A Royal Escort" quest credit.
        // alive == true when he survives the blow and casts the credit spell
        // himself; alive == false is the death fail-safe, where a corpse can't
        // cast so the encounter state is updated directly instead.
        void FinishEncounter(Unit* who, bool alive)
        {
            if (finished)
                return;

            finished = true;

            if (InstanceScript* instance = me->GetInstanceScript())
            {
                if (Creature* arthas = ObjectAccessor::GetCreature(*me, instance->GetGuidData(DATA_ARTHAS)))
                    arthas->AI()->DoAction(ACTION_KILLED_MALGANIS);

                if (alive)
                    me->CastSpell(me, SPELL_MALGANIS_KILL_CREDIT, true);
                else
                    instance->instance->UpdateEncounterState(ENCOUNTER_CREDIT_CAST_SPELL, SPELL_MALGANIS_KILL_CREDIT, me);

                instance->instance->SummonGameObject(DUNGEON_MODE(GO_MALGANIS_CHEST_N, GO_MALGANIS_CHEST_H), 2288.35f, 1498.73f, 128.414f, -0.994837f, 0, 0, 0, 0, 7 * DAY * IN_MILLISECONDS);
            }

            if (who)
                if (Player* player = who->GetCharmerOrOwnerPlayerOrPlayerItself())
                    player->RewardPlayerAndGroupAtEvent(NPC_MALGANIS_KC_BUNNY, player);

            LOG_INFO("scripts.cos", "[CoS] Mal'Ganis defeated via {} path; encounter credit + chest + quest granted.",
                alive ? "escape" : "death-failsafe");
        }

        void UpdateAI(uint32 diff) override
        {
            if (!UpdateVictim())
                return;

            events.Update(diff);
            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;

            switch (events.ExecuteEvent())
            {
                case EVENT_SPELL_CARRION_SWARM:
                    me->CastSpell(me->GetVictim(), SPELL_CARRION_SWARM, false);
                    events.Repeat(7s);
                    break;
                case EVENT_SPELL_MIND_BLAST:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 50.0f, true))
                        me->CastSpell(target, SPELL_MIND_BLAST, false);
                    events.Repeat(6s);
                    break;
                case EVENT_SPELL_SLEEP:
                    Talk(SAY_SLEEP);
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 50.0f, true))
                        me->CastSpell(target, SPELL_SLEEP, false);
                    events.Repeat(17s);
                    break;
                case EVENT_SPELL_VAMPIRIC_TOUCH:
                    me->CastSpell(me, SPELL_VAMPIRIC_TOUCH, true);
                    events.Repeat(30s);
                    break;
            }

            DoMeleeAttackIfReady();
        }
    };
};

void AddSC_boss_mal_ganis()
{
    new boss_mal_ganis();
}
