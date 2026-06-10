#include "PartyPath.h"
#include "PartyFollow.h"
#include "PartyMgr.h"

#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "WorldSession.h"

#include <cmath>
#include <list>
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
            float            lastHeading = 0.0f;   // heading into the last recorded waypoint
            bool             hasHeading  = false;  // false until the 2nd point gives us a segment
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

        // Per-bot stall tracker for the path-playback blink. If the tank should
        // be walking toward its lookahead but isn't making ground, a door / baked
        // geometry / a recording gap has wedged it — blink it forward along the
        // recorded path to clear it (see TankFollowPath).
        struct TankStall { float x = 0.0f, y = 0.0f, z = 0.0f; uint32 lastMs = 0; uint32 ticks = 0; };
        static std::mutex                                           g_tankStallMutex;
        static std::unordered_map<uint32, TankStall>               g_tankStall;

        // Recording is DISTANCE + TURN based, NOT time based — so waypoint
        // density is independent of the 5x ghost speed (time sampling left ~5y
        // gaps at speed that cut corners and stalled the tank). Drop a point at
        // least every REC_SEGMENT_MAX yards on a straight, and an EXTRA one
        // whenever the heading turns by REC_TURN_RAD so tight corridors stay
        // crisp. REC_MIN_STEP suppresses points while standing still / jittering.
        constexpr float  REC_MIN_STEP       = 0.8f;
        constexpr float  REC_SEGMENT_MAX    = 3.0f;
        constexpr float  REC_TURN_RAD       = 0.26f;   // ~15 degrees
        constexpr float  REC_PI             = 3.14159265f;
        constexpr float  TANK_LEASH         = 45.0f;   // stop & wait past this from leader
        constexpr float  LEAD_DISTANCE      = 30.0f;   // aim this far ahead ALONG the path
        constexpr float  WAYPOINT_REACHED   = 3.5f;
        // Vertical step size that pathfinding can't handle (jumps, drops,
        // dropdowns through holes). Checked over the IMMEDIATE next stride
        // (~2y), so only a true cliff trips it — descending ramps (Deadmines)
        // have small per-stride Z change and are walked, not teleported.
        constexpr float  VERTICAL_STEP_TP   = 6.0f;
        // Path-playback stall->blink: if the tank moves less than this between
        // samples for STALL_LIMIT samples while it should be advancing, it's
        // wedged (door / baked geometry / gap) — blink forward BLINK_CLEAR_DIST
        // along the recorded path to clear it.
        constexpr uint32 STALL_SAMPLE_MS    = 400;
        constexpr uint32 STALL_LIMIT        = 3;      // ~1.2 s wedged before blinking
        constexpr float  STALL_MIN_MOVE     = 1.0f;   // moved less than this between samples = wedged
        constexpr float  BLINK_CLEAR_DIST   = 9.0f;   // teleport this far along the path past the wedge

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
            // forced=true so the speed-change packet is actually sent to the
            // controlling client (without it the player's own speed often
            // doesn't update). Enable flight + no-gravity too, so you can
            // genuinely fly through walls/gaps to lay the path fast.
            p->SetSpeed(MOVE_RUN,        rate, true);
            p->SetSpeed(MOVE_RUN_BACK,   rate, true);
            p->SetSpeed(MOVE_WALK,       rate, true);
            p->SetSpeed(MOVE_SWIM,       rate, true);
            p->SetSpeed(MOVE_SWIM_BACK,  rate, true);
            p->SetSpeed(MOVE_FLIGHT,     rate, true);
            p->SetCanFly(on);
            p->SetDisableGravity(on);
        }

        // Closed door near the recorder. Ghost mode's fly + ignore-aggro lets
        // you walk PAST enemies, but a shut door is solid client-side collision
        // you can't fly through — so a boss-gated door stopped the recording
        // dead. We're in the recorder's OWN dungeon instance, so swing nearby
        // closed doors open (collision drops with the open state) and they reset
        // when the instance does — letting the ghost walk through and lay the
        // rest of the path. Only GAMEOBJECT_TYPE_DOOR, only while recording.
        struct NearbyClosedDoorCheck
        {
            NearbyClosedDoorCheck(WorldObject const* src, float range)
                : _src(src), _range(range) {}
            bool operator()(GameObject* go) const
            {
                return go && go->isSpawned()
                    && go->GetGoType() == GAMEOBJECT_TYPE_DOOR
                    && go->GetGoState() == GO_STATE_READY        // closed
                    && _src->IsWithinDist(go, _range, false);
            }
            WorldObject const* _src;
            float _range;
        };

        static void OpenNearbyDoorsForRecorder(Player* p)
        {
            constexpr float DOOR_OPEN_RANGE = 12.0f;   // open with lead at 5x ghost speed
            std::list<GameObject*> doors;
            NearbyClosedDoorCheck check(p, DOOR_OPEN_RANGE);
            Acore::GameObjectListSearcher<NearbyClosedDoorCheck> searcher(p, doors, check);
            Cell::VisitObjects(p, searcher, DOOR_OPEN_RANGE);
            for (GameObject* go : doors)
            {
                go->SetGoState(GO_STATE_ACTIVE);   // swing open -> collision drops
                LOG_INFO("module",
                    "[WowPsParty Path] recorder opened door entry={} for guid={}",
                    go->GetEntry(), p->GetGUID().GetCounter());
            }
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

    // Logging out mid-recording would otherwise persist ghost mode (fly +
    // no-gravity + 5x speed) onto the character — they'd relog stuck in the
    // air with broken movement. Drop the recording (without saving the
    // partial buffer) and reset movement state on the way out.
    void CancelPathRecording(Player* player)
    {
        if (!player) return;
        uint32 const guidLow = player->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> lock(g_recMutex);
            auto it = g_recording.find(guidLow);
            if (it == g_recording.end()) return;
            g_recording.erase(it);
        }
        ApplyGhostMode(player, false);
        LOG_INFO("module",
            "[WowPsParty Path] recording cancelled on logout for guid={} "
            "(ghost mode cleared)", guidLow);
    }

    void TickPathRecording(uint32 /*diffMs*/)
    {
        // Sample EVERY tick — density is decided by DISTANCE + TURN, not time,
        // so the route stays crisp at 5x ghost speed (per-tick work is a few
        // float ops for the lone recorder).
        std::vector<uint32> recorders;
        {
            std::lock_guard<std::mutex> lock(g_recMutex);
            recorders.reserve(g_recording.size());
            for (auto const& kv : g_recording) recorders.push_back(kv.first);
        }
        for (uint32 guidLow : recorders)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(guidLow));
            if (!p || !p->IsInWorld()) continue;

            // Swing open any closed door the ghost is approaching so a boss-gated
            // door can't wall off the rest of the recording (instance-local).
            OpenNearbyDoorsForRecorder(p);

            std::lock_guard<std::mutex> lock(g_recMutex);
            auto it = g_recording.find(guidLow);
            if (it == g_recording.end()) continue;
            RecordingState& st = it->second;

            float const px = p->GetPositionX();
            float const py = p->GetPositionY();
            float const pz = p->GetPositionZ();

            if (st.buffer.empty())
            {
                st.buffer.push_back({ px, py, pz, p->GetOrientation() });
                continue;
            }

            Vec3 const& last = st.buffer.back();
            float const dx = px - last.x, dy = py - last.y;
            float const moved = std::sqrt(dx * dx + dy * dy);
            if (moved < REC_MIN_STEP) continue;   // standing still / jitter — no point

            // Heading from the last waypoint to here. A big change vs the heading
            // INTO the last waypoint means we've turned — drop a crisp corner
            // point even if we haven't covered a full segment yet.
            float const candHeading = std::atan2(dy, dx);
            bool turned = false;
            if (st.hasHeading)
            {
                float d = candHeading - st.lastHeading;
                while (d >  REC_PI) d -= 2.0f * REC_PI;
                while (d < -REC_PI) d += 2.0f * REC_PI;
                if (std::fabs(d) >= REC_TURN_RAD) turned = true;
            }

            if (moved >= REC_SEGMENT_MAX || turned)
            {
                st.buffer.push_back({ px, py, pz, p->GetOrientation() });
                st.lastHeading = candHeading;
                st.hasHeading  = true;
            }
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
        // Rate-limited diagnostic so we can see WHY the tank isn't leading.
        // One line per bot every ~3s. Grep "WowPsParty TankPath".
        auto tlog = [bot](char const* why)
        {
            static thread_local std::unordered_map<uint32, uint32> lastMs;
            uint32 const now = getMSTime();
            uint32& last = lastMs[bot->GetGUID().GetCounter()];
            if (now - last < 3000) return;
            last = now;
            LOG_INFO("module", "[WowPsParty TankPath] {} -> {}", bot->GetName(), why);
        };

        if (!bot || !bot->IsAlive() || !bot->IsInWorld()) return;
        if (!bot->GetSession()) return;
        if (bot->HasUnitFlag(UNIT_FLAG_POSSESSED)) { tlog("skip: possessed"); return; }
        if (bot->IsNonMeleeSpellCast(false, false, true)) { tlog("skip: casting"); return; }
        if (IsFollowerHeld(bot->GetGUID())) { tlog("skip: held"); return; }
        if (bot->IsInCombat()) { tlog("skip: bot in combat"); return; }

        ObjectGuid const leaderGuid = GetLeaderFor(bot->GetGUID());
        if (!leaderGuid) { tlog("skip: no leader directive"); return; }
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader || !leader->IsInWorld()) { tlog("skip: leader not in world"); return; }
        if (leader->GetMapId() != bot->GetMapId()) { tlog("skip: leader other map"); return; }
        if (!leader->GetMap() || !leader->GetMap()->IsDungeon()) { tlog("skip: leader not in dungeon"); return; }
        if (leader->IsInCombat()) { tlog("skip: leader in combat"); return; }

        // Don't lead if the path doesn't exist for this dungeon.
        auto const& path = EnsurePath(bot->GetMapId());
        if (path.size() < 2) { tlog("skip: no path for this map (<2 wp)"); return; }

        // Leash check.
        if (bot->GetDistance(leader) > TANK_LEASH)
        {
            tlog("skip: beyond tank leash (>30y from leader)");
            return;
        }

        // Past the skip gates, we're committed to leading this tick. Tell the
        // follow ticker to keep its hands off (no MoveFollow re-assert, no
        // stuck catch-up teleport) — otherwise, when the leader stops and the
        // tank idles at its lookahead, the ticker reads the constant distance
        // as "stuck" and teleports the tank onto the leader. Refreshed every
        // tick; expires shortly after we stop leading (combat / beyond leash).
        WowPsParty::MarkTankLeading(bot->GetGUID(), 2500);

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

        // Target = walk FORWARD along the path from the leader's cursor until
        // we're ~LEAD_DISTANCE ahead (by path length, not waypoint count — the
        // recorded waypoints are only ~2y apart, so a fixed "+3" lookahead made
        // the tank hug the leader). Clamped to the path end.
        uint32 targetIdx = nearestIdx;
        float acc = 0.0f;
        while (targetIdx + 1 < path.size())
        {
            float const seg = Dist3D(path[targetIdx].x, path[targetIdx].y, path[targetIdx].z,
                                     path[targetIdx + 1].x, path[targetIdx + 1].y, path[targetIdx + 1].z);
            if (acc + seg > LEAD_DISTANCE) break;
            acc += seg;
            ++targetIdx;
        }
        Vec3 const& wp = path[targetIdx];

        // Already at the target waypoint? nothing to do.
        float const distToWp = Dist3D(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                       wp.x, wp.y, wp.z);
        if (distToWp <= WAYPOINT_REACHED) { tlog("idle: already at lookahead waypoint"); return; }

        uint32 const botGuidLow = bot->GetGUID().GetCounter();

        // Tank's own nearest waypoint — used by both the stall-blink and the
        // cliff-blink below.
        uint32 tankNearest = 0;
        float  tankNearD   = std::numeric_limits<float>::max();
        for (uint32 i = 0; i < path.size(); ++i)
        {
            float const d = Dist3D(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                   path[i].x, path[i].y, path[i].z);
            if (d < tankNearD) { tankNearD = d; tankNearest = i; }
        }

        // Stall -> blink. We have a lookahead target beyond reach but the tank
        // can be wedged on a DOOR / baked-closed geometry / a recording gap that
        // the navmesh can't path. Sample progress; if it hasn't moved for ~1.2s,
        // NearTeleport it forward along the RECORDED path (the route the player
        // actually walked, so always valid) far enough to clear the obstacle —
        // the same blink Kevin OK'd for cliffs, now for doors too. A boss-gated
        // door is open by the time the tank arrives (it pauses in combat, so it
        // only resumes once the boss is dead). Runs BEFORE the re-issue dedup so
        // a tank wedged mid-MovePoint is still rescued (the dedup would otherwise
        // sit on the same blocked target until the player nudged forward — the
        // "tank just stops, we have to walk forward manually" report).
        {
            bool wedged = false;
            {
                std::lock_guard<std::mutex> lock(g_tankStallMutex);
                TankStall& s = g_tankStall[botGuidLow];
                uint32 const now = getMSTime();
                if (now - s.lastMs >= STALL_SAMPLE_MS)
                {
                    float const moved = Dist3D(bot->GetPositionX(), bot->GetPositionY(),
                                               bot->GetPositionZ(), s.x, s.y, s.z);
                    if (s.lastMs != 0 && moved < STALL_MIN_MOVE) ++s.ticks;
                    else                                          s.ticks = 0;
                    s.x = bot->GetPositionX(); s.y = bot->GetPositionY(); s.z = bot->GetPositionZ();
                    s.lastMs = now;
                    if (s.ticks >= STALL_LIMIT) { wedged = true; s.ticks = 0; }
                }
            }
            if (wedged)
            {
                // Advance from the tank's cursor accumulating ~BLINK_CLEAR_DIST of
                // path length (capped at the lookahead so we never blink past
                // where we're leading to), then teleport there.
                uint32 blinkIdx = tankNearest;
                float acc = 0.0f;
                while (blinkIdx + 1 < path.size() && blinkIdx < targetIdx && acc < BLINK_CLEAR_DIST)
                {
                    acc += Dist3D(path[blinkIdx].x, path[blinkIdx].y, path[blinkIdx].z,
                                  path[blinkIdx + 1].x, path[blinkIdx + 1].y, path[blinkIdx + 1].z);
                    ++blinkIdx;
                }
                if (blinkIdx > tankNearest)
                {
                    // Only blink if the destination is in LINE OF SIGHT. An OPEN
                    // door (or a navmesh-baked-closed-but-now-open passage) has
                    // clear LoS, so we blink through; a CLOSED door or a real wall
                    // blocks LoS, so we wait. This is what makes a boss-gated door
                    // work: while it's shut the tank waits, and the instant it
                    // opens (boss dead) the next stall cycle sees LoS and blinks
                    // through — without ever teleporting past content still walled
                    // off.
                    if (!bot->IsWithinLOS(path[blinkIdx].x, path[blinkIdx].y, path[blinkIdx].z))
                    {
                        tlog("wedged but no LoS to blink target (closed door / wall) — waiting");
                        return;
                    }
                    tlog("blink: wedged (open door / geometry / gap), NearTeleport forward along path");
                    bot->GetMotionMaster()->Clear();
                    bot->NearTeleportTo(path[blinkIdx].x, path[blinkIdx].y, path[blinkIdx].z, path[blinkIdx].o);
                    std::lock_guard<std::mutex> lock(g_tankProgressMutex);
                    g_tankCursor[botGuidLow] = blinkIdx;
                    return;
                }
            }
        }

        // Vertical step too large for the navmesh? (jumps in BFD, dropdowns in
        // Stockades, the giant fall in LBRS …) Teleport across — same blink.
        // Checked over the tank's IMMEDIATE next stride so only a true cliff
        // trips it; descending ramps (Deadmines) walk normally.
        uint32 const stepIdx = std::min(tankNearest + 1, uint32(path.size()) - 1);
        float const dzStep = std::fabs(path[stepIdx].z - bot->GetPositionZ());
        if (dzStep > VERTICAL_STEP_TP)
        {
            tlog("blink: cliff on next stride, NearTeleport across");
            bot->NearTeleportTo(path[stepIdx].x, path[stepIdx].y, path[stepIdx].z, path[stepIdx].o);
            return;
        }

        // Avoid re-issuing MovePoint to the same waypoint every tick.
        {
            std::lock_guard<std::mutex> lock(g_tankProgressMutex);
            auto& cur = g_tankCursor[botGuidLow];
            if (cur == targetIdx + 1) return;  // already moving toward this one
            cur = targetIdx + 1;
        }

        tlog("LEAD: MovePoint to lookahead waypoint");
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(0, wp.x, wp.y, wp.z);
    }
}
