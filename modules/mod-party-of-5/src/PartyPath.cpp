#include "PartyPath.h"
#include "PartyFollow.h"
#include "PartyMgr.h"

#include "Chat.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "WorldSession.h"

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace WowPsParty
{
    namespace
    {
        struct Vec3 { float x, y, z, o; };

        struct RecordingState
        {
            ObjectGuid       player;
            uint32           mapId      = 0;
            uint32           accumMs    = 0;
            std::vector<Vec3> buffer;
        };

        // playerGuidLow -> recording state
        static std::mutex                                           g_recMutex;
        static std::unordered_map<uint32, RecordingState>           g_recording;

        // Path cache: mapId -> ordered waypoint list. Loaded lazily; invalidated
        // when the path for a map is updated or cleared.
        static std::mutex                                           g_pathCacheMutex;
        static std::unordered_map<uint32, std::vector<Vec3>>        g_pathCache;
        static std::unordered_map<uint32, bool>                     g_pathCacheLoaded;

        // Per-bot tank-playback cursor: which waypoint index we last advanced
        // toward. Avoids thrashing MovePoint every tick.
        static std::mutex                                           g_tankProgressMutex;
        static std::unordered_map<uint32, uint32>                   g_tankCursor;

        constexpr uint32 SAMPLE_INTERVAL_MS = 250;   // 4 Hz — tight corners + jumps both captured
        constexpr float  DEDUPE_MIN_DIST    = 2.0f;
        constexpr float  TANK_LEASH         = 30.0f;
        constexpr int    TANK_LOOK_AHEAD    = 3;
        constexpr float  WAYPOINT_REACHED   = 3.5f;
        // Vertical step size that pathfinding can't handle (jumps, drops,
        // dropdowns through holes). When the next waypoint differs by more
        // than this in Z, the tank skips walking and just teleports.
        constexpr float  VERTICAL_STEP_TP   = 4.0f;

        static float Dist3D(float ax, float ay, float az, float bx, float by, float bz)
        {
            float dx = ax - bx, dy = ay - by, dz = az - bz;
            return std::sqrt(dx*dx + dy*dy + dz*dz);
        }

        static std::vector<Vec3> const& EnsurePath(uint32 mapId)
        {
            std::lock_guard<std::mutex> lock(g_pathCacheMutex);
            auto& loaded = g_pathCacheLoaded[mapId];
            if (loaded) return g_pathCache[mapId];

            auto& vec = g_pathCache[mapId];
            vec.clear();
            QueryResult q = CharacterDatabase.Query(
                "SELECT `x`,`y`,`z`,`orientation` FROM `dungeon_path` "
                "WHERE `map_id` = {} ORDER BY `sequence`", mapId);
            if (q)
            {
                do
                {
                    Field* f = q->Fetch();
                    vec.push_back({ f[0].Get<float>(), f[1].Get<float>(),
                                    f[2].Get<float>(), f[3].Get<float>() });
                } while (q->NextRow());
            }
            loaded = true;
            return vec;
        }

        static void InvalidatePathCache(uint32 mapId)
        {
            std::lock_guard<std::mutex> lock(g_pathCacheMutex);
            g_pathCache.erase(mapId);
            g_pathCacheLoaded[mapId] = false;
        }

        static void PersistPath(uint32 mapId, std::vector<Vec3> const& points, uint32 recordedBy)
        {
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append("DELETE FROM `dungeon_path` WHERE `map_id` = {}", mapId);
            for (uint32 i = 0; i < points.size(); ++i)
            {
                Vec3 const& p = points[i];
                tx->Append(
                    "INSERT INTO `dungeon_path` "
                    "(`map_id`,`sequence`,`x`,`y`,`z`,`orientation`,`recorded_by`) "
                    "VALUES ({}, {}, {}, {}, {}, {}, {})",
                    mapId, i, p.x, p.y, p.z, p.o, recordedBy);
            }
            CharacterDatabase.CommitTransaction(tx);
            InvalidatePathCache(mapId);
        }

        static void ApplyGhostMode(Player* p, bool on)
        {
            if (!p) return;
            // SetGameMaster makes the player invisible/untargetable to hostiles
            // and ignores aggro. SetSpeed takes a multiplier (m_speed_rate),
            // so passing 5.0 = 5× normal, 1.0 = baseline.
            p->SetGameMaster(on);
            p->SetGMVisible(!on);
            float const rate = on ? 5.0f : 1.0f;
            p->SetSpeed(MOVE_RUN,        rate);
            p->SetSpeed(MOVE_RUN_BACK,   rate);
            p->SetSpeed(MOVE_WALK,       rate);
            p->SetSpeed(MOVE_SWIM,       rate);
            p->SetSpeed(MOVE_SWIM_BACK,  rate);
            p->SetSpeed(MOVE_FLIGHT,     rate);
        }
    }

    bool TogglePathRecording(Player* player)
    {
        if (!player) return false;
        if (!player->GetMap() || !player->GetMap()->IsDungeon())
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff5555[WowPsParty]|r Path recording only works inside dungeons.");
            return false;
        }

        uint32 const guidLow = player->GetGUID().GetCounter();
        uint32 const mapId   = player->GetMapId();

        std::unique_lock<std::mutex> lock(g_recMutex);
        auto it = g_recording.find(guidLow);
        if (it == g_recording.end())
        {
            // Start
            RecordingState st;
            st.player = player->GetGUID();
            st.mapId  = mapId;
            st.buffer.push_back({ player->GetPositionX(), player->GetPositionY(),
                                   player->GetPositionZ(), player->GetOrientation() });
            g_recording[guidLow] = std::move(st);
            lock.unlock();
            ApplyGhostMode(player, true);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Path recording |cff66ff66ON|r — ghost mode "
                "+ 5x speed. Fly through the dungeon, then press the bound key again to save.");
            return true;
        }

        // Stop + persist
        RecordingState st = std::move(it->second);
        g_recording.erase(it);
        lock.unlock();

        if (!st.buffer.empty())
        {
            PersistPath(st.mapId, st.buffer, guidLow);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Saved {} waypoint(s) for map {}.",
                uint32(st.buffer.size()), st.mapId);
        }
        ApplyGhostMode(player, false);
        return false;
    }

    bool IsRecordingPath(ObjectGuid playerGuid)
    {
        std::lock_guard<std::mutex> lock(g_recMutex);
        return g_recording.find(playerGuid.GetCounter()) != g_recording.end();
    }

    void TickPathRecording(uint32 diffMs)
    {
        std::vector<uint32> toSample;
        {
            std::lock_guard<std::mutex> lock(g_recMutex);
            for (auto& kv : g_recording)
            {
                kv.second.accumMs += diffMs;
                if (kv.second.accumMs >= SAMPLE_INTERVAL_MS)
                {
                    kv.second.accumMs = 0;
                    toSample.push_back(kv.first);
                }
            }
        }
        for (uint32 guidLow : toSample)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(guidLow));
            if (!p || !p->IsInWorld()) continue;

            std::lock_guard<std::mutex> lock(g_recMutex);
            auto it = g_recording.find(guidLow);
            if (it == g_recording.end()) continue;
            RecordingState& st = it->second;

            // Dedupe: skip if we haven't moved meaningfully since the last point.
            if (!st.buffer.empty())
            {
                Vec3 const& last = st.buffer.back();
                float const d = Dist3D(p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(),
                                       last.x, last.y, last.z);
                if (d < DEDUPE_MIN_DIST) continue;
            }
            st.buffer.push_back({ p->GetPositionX(), p->GetPositionY(),
                                   p->GetPositionZ(), p->GetOrientation() });
        }
    }

    void ClearPath(uint32 mapId)
    {
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append("DELETE FROM `dungeon_path` WHERE `map_id` = {}", mapId);
        CharacterDatabase.CommitTransaction(tx);
        InvalidatePathCache(mapId);
    }

    uint32 GetPathWaypointCount(uint32 mapId)
    {
        return uint32(EnsurePath(mapId).size());
    }

    void TankFollowPath(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld()) return;
        if (!bot->GetSession()) return;
        if (bot->HasUnitFlag(UNIT_FLAG_POSSESSED)) return;
        if (bot->IsNonMeleeSpellCast(false, false, true)) return;
        if (IsFollowerHeld(bot->GetGUID())) return;
        if (bot->IsInCombat()) return;  // engagement system handles fighting

        ObjectGuid const leaderGuid = GetLeaderFor(bot->GetGUID());
        if (!leaderGuid) return;
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader || !leader->IsInWorld()) return;
        if (leader->GetMapId() != bot->GetMapId()) return;
        if (!leader->GetMap() || !leader->GetMap()->IsDungeon()) return;
        if (leader->IsInCombat()) return;  // let AssistTarget take over while engaged

        // Don't lead if the path doesn't exist for this dungeon.
        auto const& path = EnsurePath(bot->GetMapId());
        if (path.size() < 2) return;

        // Leash check.
        if (bot->GetDistance(leader) > TANK_LEASH) return;

        // Find leader's nearest waypoint (the "cursor"). Linear scan — typical
        // dungeon path is ~50-200 points, this runs once per tick per bot.
        uint32 nearestIdx = 0;
        float  nearestD   = std::numeric_limits<float>::max();
        for (uint32 i = 0; i < path.size(); ++i)
        {
            float const d = Dist3D(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                                   path[i].x, path[i].y, path[i].z);
            if (d < nearestD) { nearestD = d; nearestIdx = i; }
        }

        // Target = cursor + lookahead, clamped to the path's end.
        uint32 const targetIdx = std::min(nearestIdx + TANK_LOOK_AHEAD,
                                          uint32(path.size()) - 1);
        Vec3 const& wp = path[targetIdx];

        // Already at the target waypoint? nothing to do.
        float const distToWp = Dist3D(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                       wp.x, wp.y, wp.z);
        if (distToWp <= WAYPOINT_REACHED) return;

        // Avoid re-issuing MovePoint to the same waypoint every tick.
        uint32 const botGuidLow = bot->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> lock(g_tankProgressMutex);
            auto& cur = g_tankCursor[botGuidLow];
            if (cur == targetIdx + 1) return;  // already moving toward this one
            cur = targetIdx + 1;
        }

        // Vertical step too large for the navmesh? (jumps in BFD, dropdowns
        // in Stockades, the giant fall in LBRS …) Teleport instead of
        // trying to path. User-facing this looks like the tank "blinks"
        // through the obstacle, which Kevin signed off on.
        float const dz = std::fabs(wp.z - bot->GetPositionZ());
        if (dz > VERTICAL_STEP_TP)
        {
            bot->NearTeleportTo(wp.x, wp.y, wp.z, wp.o);
            return;
        }

        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(0, wp.x, wp.y, wp.z);
    }
}
