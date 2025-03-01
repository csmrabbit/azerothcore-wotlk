/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "InstanceScript.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include "the_black_morass.h"

const Position PortalLocation[4] =
{
    { -2030.8318f, 7024.9443f, 23.071817f, 3.14159f },
    { -1961.7335f, 7029.5280f, 21.811401f, 2.12931f },
    { -1887.6950f, 7106.5570f, 22.049500f, 4.95673f },
    { -1930.9106f, 7183.5970f, 23.007639f, 3.59537f }
};

ObjectData const creatureData[1] =
{
    { NPC_MEDIVH, DATA_MEDIVH }
};

class instance_the_black_morass : public InstanceMapScript
{
public:
    instance_the_black_morass() : InstanceMapScript("instance_the_black_morass", 269) { }

    InstanceScript* GetInstanceScript(InstanceMap* map) const override
    {
        return new instance_the_black_morass_InstanceMapScript(map);
    }

    struct instance_the_black_morass_InstanceMapScript : public InstanceScript
    {
        instance_the_black_morass_InstanceMapScript(Map* map) : InstanceScript(map)
        {
            SetHeaders(DataHeader);
            SetBossNumber(EncounterCount);
            LoadObjectData(creatureData, nullptr);
            _currentRift = 0;
            _shieldPercent = 100;
            _encounterNPCs.clear();
        }

        void CleanupInstance()
        {
            _currentRift = 0;
            _shieldPercent = 100;

            _availableRiftPositions.clear();
            _scheduler.CancelAll();

            for (Position const& pos : PortalLocation)
            {
                _availableRiftPositions.push_back(pos);
            }

            instance->LoadGrid(-2023.0f, 7121.0f);
            if (Creature* medivh = GetCreature(DATA_MEDIVH))
            {
                medivh->DespawnOrUnsummon(0ms, 3s);
            }
        }

        bool SetBossState(uint32 type, EncounterState state) override
        {
            if (!InstanceScript::SetBossState(type, state))
            {
                return false;
            }

            if (state == DONE)
            {
                switch (type)
                {
                    case DATA_AEONUS:
                    {
                        if (Creature* medivh = GetCreature(DATA_MEDIVH))
                        {
                            medivh->AI()->DoAction(ACTION_OUTRO);
                        }

                        instance->DoForAllPlayers([&](Player* player)
                        {
                            if (player->GetQuestStatus(QUEST_OPENING_PORTAL) == QUEST_STATUS_INCOMPLETE)
                            {
                                player->AreaExploredOrEventHappens(QUEST_OPENING_PORTAL);
                            }

                            if (player->GetQuestStatus(QUEST_MASTER_TOUCH) == QUEST_STATUS_INCOMPLETE)
                            {
                                player->AreaExploredOrEventHappens(QUEST_MASTER_TOUCH);
                            }
                        });
                        break;
                    }
                    case DATA_CHRONO_LORD_DEJA:
                    case DATA_TEMPORUS:
                    {
                        _scheduler.RescheduleGroup(CONTEXT_GROUP_RIFTS, 2min + 30s);

                        for (ObjectGuid const& guid : _encounterNPCs)
                        {
                            if (Creature* creature = instance->GetCreature(guid))
                            {
                                switch (creature->GetEntry())
                                {
                                    case NPC_RIFT_KEEPER_WARLOCK:
                                    case NPC_RIFT_KEEPER_MAGE:
                                    case NPC_RIFT_LORD:
                                    case NPC_RIFT_LORD_2:
                                    case NPC_TIME_RIFT:
                                        creature->DespawnOrUnsummon();
                                    break;
                                default:
                                    break;
                                }
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
            }

            return true;
        }

        void OnPlayerEnter(Player* player) override
        {
            if (instance->GetPlayersCountExceptGMs() <= 1 && GetBossState(DATA_AEONUS) != DONE)
            {
                CleanupInstance();
            }

            player->SendUpdateWorldState(WORLD_STATE_BM, _currentRift > 0 ? 1 : 0);
            player->SendUpdateWorldState(WORLD_STATE_BM_SHIELD, _shieldPercent);
            player->SendUpdateWorldState(WORLD_STATE_BM_RIFT, _currentRift);
        }

        void ScheduleNextPortal(Milliseconds time)
        {
            _scheduler.CancelGroup(CONTEXT_GROUP_RIFTS);

            _scheduler.Schedule(time, [this](TaskContext context)
            {
                if (GetCreature(DATA_MEDIVH))
                {
                    Position spawnPos;
                    if (!_availableRiftPositions.empty())
                    {
                        spawnPos = Acore::Containers::SelectRandomContainerElement(_availableRiftPositions);
                        _availableRiftPositions.remove(spawnPos);

                        DoUpdateWorldState(WORLD_STATE_BM_RIFT, ++_currentRift);

                        if (Creature* rift = instance->SummonCreature(NPC_TIME_RIFT, spawnPos))
                        {
                            _scheduler.Schedule(6s, [this, rift](TaskContext)
                            {
                                SummonPortalKeeper(rift);
                            });
                        }

                        // Here we check if we have available rift spots.
                        if (_currentRift < 18)
                        {
                            if (!_availableRiftPositions.empty())
                            {
                                context.Repeat((_currentRift >= 13 ? 2min : 90s));
                            }
                            else
                            {
                                context.Repeat(4s);
                            }
                        }
                    }
                }

                context.SetGroup(CONTEXT_GROUP_RIFTS);
            });
        }

        void OnCreatureCreate(Creature* creature) override
        {
            switch (creature->GetEntry())
            {
                case NPC_TIME_RIFT:
                case NPC_CHRONO_LORD_DEJA:
                case NPC_INFINITE_CHRONO_LORD:
                case NPC_TEMPORUS:
                case NPC_INFINITE_TIMEREAVER:
                case NPC_AEONUS:
                case NPC_RIFT_KEEPER_WARLOCK:
                case NPC_RIFT_KEEPER_MAGE:
                case NPC_RIFT_LORD:
                case NPC_RIFT_LORD_2:
                case NPC_INFINITE_ASSASIN:
                case NPC_INFINITE_WHELP:
                case NPC_INFINITE_CRONOMANCER:
                case NPC_INFINITE_EXECUTIONER:
                case NPC_INFINITE_VANQUISHER:
                case NPC_DP_BEAM_STALKER:
                    _encounterNPCs.insert(creature->GetGUID());
                    break;
            }

            InstanceScript::OnCreatureCreate(creature);
        }

        void OnCreatureRemove(Creature* creature) override
        {
            switch (creature->GetEntry())
            {
                case NPC_TIME_RIFT:
                    if (_currentRift < 18)
                    {
                        if (!_availableRiftPositions.empty() && _availableRiftPositions.size() < 3)
                        {
                            ScheduleNextPortal((_currentRift >= 13 ? 2min : 90s));
                        }
                        else
                        {
                            ScheduleNextPortal(4s);
                        }
                    }

                    _availableRiftPositions.push_back(creature->GetHomePosition());
                    [[fallthrough]];
                case NPC_CHRONO_LORD_DEJA:
                case NPC_INFINITE_CHRONO_LORD:
                case NPC_TEMPORUS:
                case NPC_INFINITE_TIMEREAVER:
                case NPC_AEONUS:
                case NPC_RIFT_KEEPER_WARLOCK:
                case NPC_RIFT_KEEPER_MAGE:
                case NPC_RIFT_LORD:
                case NPC_RIFT_LORD_2:
                case NPC_INFINITE_ASSASIN:
                case NPC_INFINITE_WHELP:
                case NPC_INFINITE_CRONOMANCER:
                case NPC_INFINITE_EXECUTIONER:
                case NPC_INFINITE_VANQUISHER:
                    _encounterNPCs.erase(creature->GetGUID());
                    break;
            }

            InstanceScript::OnCreatureRemove(creature);
        }

        void SetData(uint32 type, uint32 data) override
        {
            switch (type)
            {
                case DATA_MEDIVH:
                {
                    DoUpdateWorldState(WORLD_STATE_BM, 1);
                    DoUpdateWorldState(WORLD_STATE_BM_SHIELD, _shieldPercent);
                    DoUpdateWorldState(WORLD_STATE_BM_RIFT, _currentRift);

                    ScheduleNextPortal(3s);

                    for (ObjectGuid const& guid : _encounterNPCs)
                    {
                        if (guid.GetEntry() == NPC_DP_BEAM_STALKER)
                        {
                            if (Creature* creature = instance->GetCreature(guid))
                            {
                                if (!creature->IsAlive())
                                {
                                    creature->Respawn(true);
                                }
                            }
                            break;
                        }
                    }

                    break;
                }
                case DATA_DAMAGE_SHIELD:
                {
                    if (_shieldPercent <= 0)
                    {
                        return;
                    }

                    _shieldPercent -= data;
                    if (_shieldPercent < 0)
                    {
                        _shieldPercent = 0;
                    }

                    DoUpdateWorldState(WORLD_STATE_BM_SHIELD, _shieldPercent);

                    if (!_shieldPercent)
                    {
                        if (Creature* medivh = GetCreature(DATA_MEDIVH))
                        {
                            if (medivh->IsAlive() && medivh->IsAIEnabled)
                            {
                                medivh->SetImmuneToNPC(true);
                                medivh->AI()->Talk(SAY_MEDIVH_DEATH);

                                for (ObjectGuid const& guid : _encounterNPCs)
                                {
                                    if (Creature* creature = instance->GetCreature(guid))
                                    {
                                        creature->InterruptNonMeleeSpells(true);
                                    }
                                }

                                // Step 1 - Medivh loses all auras.
                                _scheduler.Schedule(4s, [this](TaskContext)
                                {
                                    if (Creature* medivh = GetCreature(DATA_MEDIVH))
                                    {
                                        medivh->RemoveAllAuras();
                                    }

                                    // Step 2 - Medivh dies and visual effect NPCs are despawned.
                                    _scheduler.Schedule(500ms, [this](TaskContext)
                                    {
                                        if (Creature* medivh = GetCreature(DATA_MEDIVH))
                                        {
                                            medivh->KillSelf(false);

                                            GuidSet encounterNPCSCopy = _encounterNPCs;
                                            for (ObjectGuid const& guid : encounterNPCSCopy)
                                            {
                                                switch (guid.GetEntry())
                                                {
                                                    case NPC_TIME_RIFT:
                                                    case NPC_DP_EMITTER_STALKER:
                                                    case NPC_DP_CRYSTAL_STALKER:
                                                    case NPC_DP_BEAM_STALKER:
                                                        if (Creature* creature = instance->GetCreature(guid))
                                                        {
                                                            creature->DespawnOrUnsummon();
                                                        }
                                                        break;
                                                    default:
                                                        break;
                                                }
                                            }
                                        }

                                        // Step 3 - All summoned creatures despawn
                                        _scheduler.Schedule(2s, [this](TaskContext)
                                        {
                                            GuidSet encounterNPCSCopy = _encounterNPCs;
                                            for (ObjectGuid const& guid : encounterNPCSCopy)
                                            {
                                                if (Creature* creature = instance->GetCreature(guid))
                                                {
                                                    creature->CastSpell(creature, SPELL_TELEPORT_VISUAL, true);
                                                    creature->DespawnOrUnsummon(1200ms, 0s);
                                                }
                                            }

                                            _scheduler.CancelAll();
                                        });
                                    });
                                });
                            }
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }

        uint32 GetData(uint32 type) const override
        {
            switch (type)
            {
                case DATA_SHIELD_PERCENT:
                    return _shieldPercent;
                case DATA_RIFT_NUMBER:
                    return _currentRift;
            }
            return 0;
        }

        void SummonPortalKeeper(Creature* rift)
        {
            if (!rift)
            {
                return;
            }

            int32 entry = 0;
            switch (_currentRift)
            {
                case 6:
                    entry = GetBossState(DATA_CHRONO_LORD_DEJA) == DONE ? (instance->IsHeroic() ? NPC_INFINITE_CHRONO_LORD : -NPC_CHRONO_LORD_DEJA) : NPC_CHRONO_LORD_DEJA;
                    break;
                case 12:
                    entry = GetBossState(DATA_TEMPORUS) == DONE ? (instance->IsHeroic() ? NPC_INFINITE_TIMEREAVER : -NPC_TEMPORUS) : NPC_TEMPORUS;
                    break;
                case 18:
                    entry = NPC_AEONUS;
                    break;
                default:
                    entry = RAND(NPC_RIFT_KEEPER_WARLOCK, NPC_RIFT_KEEPER_MAGE, NPC_RIFT_LORD, NPC_RIFT_LORD_2);
                    break;
            }

            Position pos = rift->GetNearPosition(10.0f, 2 * M_PI * rand_norm());

            if (Creature* summon = rift->SummonCreature(std::abs(entry), pos, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 3 * MINUTE * IN_MILLISECONDS))
            {
                if (entry < 0)
                {
                    summon->SetLootMode(0);
                }

                if (summon->GetEntry() != NPC_AEONUS)
                {
                    rift->CastSpell(summon, SPELL_RIFT_CHANNEL, false);
                }
                else
                {
                    summon->SetReactState(REACT_DEFENSIVE);
                    _scheduler.CancelGroup(CONTEXT_GROUP_RIFTS);
                }
            }
        }

        void Update(uint32 diff) override
        {
            _scheduler.Update(diff);
        }

    protected:
        std::list<Position> _availableRiftPositions;
        GuidSet _encounterNPCs;
        uint8 _currentRift;
        int8 _shieldPercent;
        TaskScheduler _scheduler;
    };
};

void AddSC_instance_the_black_morass()
{
    new instance_the_black_morass();
}
