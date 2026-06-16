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
            uint32           areaId     = 0;   // recorder's area at start — the wing key
            uint32           accumMs    = 0;
            std::vector<Vec3> buffer;
            float            lastHeading = 0.0f;   // heading into the last recorded waypoint
            bool             hasHeading  = false;  // false until the 2nd point gives us a segment
        };

        // playerGuidLow -> recording state
        static std::mutex                                           g_recMutex;
        static std::unordered_map<uint32, RecordingState>           g_recording;

        // Path cache: mapId -> the recorded paths on that map. A multi-wing dungeon
        // (Scarlet Monastery x4, Dire Maul x3) puts several wings on ONE map id, so
        // we store one path PER WING keyed by the recorder's area; playback picks
        // the wing whose waypoints are nearest the leader. Loaded lazily; invalidated
        // when a map's paths are updated or cleared.
        struct PathGroup { uint32 area = 0; std::vector<Vec3> pts; };
        static std::mutex                                           g_pathCacheMutex;
        static std::unordered_map<uint32, std::vector<PathGroup>>   g_pathCache;
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
        // density is independent of the ghost speed (time sampling left ~5y
        // gaps at speed that cut corners and stalled the tank). Drop a point at
        // least every REC_SEGMENT_MAX yards on a straight, and an EXTRA one
        // whenever the heading turns by REC_TURN_RAD so tight corridors stay
        // crisp. REC_MIN_STEP suppresses points while standing still / jittering.
        // Tuned dense (1.5y straights / ~10° corners) so the tank never has to
        // span a long gap to the next point — a sparse corner used to read as the
        // nearest waypoint being BEHIND it, making it briefly turn back.
        constexpr float  REC_MIN_STEP       = 0.5f;
        constexpr float  REC_SEGMENT_MAX    = 1.5f;
        constexpr float  REC_TURN_RAD       = 0.17f;   // ~10 degrees
        constexpr float  REC_PI             = 3.14159265f;
        constexpr float  TANK_LEASH         = 45.0f;   // stop & wait past this from leader
        constexpr float  LEAD_DISTANCE      = 30.0f;   // aim this far ahead ALONG the path
        constexpr float  WAYPOINT_REACHED   = 3.5f;
        // Vertical step size that pathfinding can't handle (jumps, drops,
        // dropdowns through holes). A genuine drop is a near-vertical PLUNGE: a
        // big Z change over almost no horizontal ground. Because recording is
        // 2D-distance based (a fall drops almost no points until the player lands
        // and walks on), a real fall leaves one stride that's tall but barely
        // horizontal, whereas a walked staircase or ramp ALWAYS covers real
        // horizontal ground per stride. So a stride only blinks when its Z change
        // exceeds VERTICAL_STEP_TP AND is steeper than DROP_SLOPE_RATIO (dz:horiz,
        // ~63deg — well above any walkable WoW slope). That keeps steep stairs and
        // ramps WALKED instead of teleported one waypoint at a time.
        constexpr float  VERTICAL_STEP_TP   = 6.0f;
        constexpr float  DROP_SLOPE_RATIO   = 2.0f;
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

        static std::vector<PathGroup> const& EnsureGroups(uint32 mapId)
        {
            std::lock_guard<std::mutex> lock(g_pathCacheMutex);
            auto& loaded = g_pathCacheLoaded[mapId];
            if (loaded) return g_pathCache[mapId];

            auto& groups = g_pathCache[mapId];
            groups.clear();
            // ORDER BY area_id, sequence so consecutive same-area rows form one wing.
            QueryResult q = CharacterDatabase.Query(
                "SELECT `area_id`,`x`,`y`,`z`,`orientation` FROM `dungeon_path` "
                "WHERE `map_id` = {} ORDER BY `area_id`, `sequence`", mapId);
            if (q)
            {
                do
                {
                    Field* f = q->Fetch();
                    uint32 const area = f[0].Get<uint32>();
                    if (groups.empty() || groups.back().area != area)
                        groups.push_back(PathGroup{ area, {} });
                    groups.back().pts.push_back({ f[1].Get<float>(), f[2].Get<float>(),
                                                  f[3].Get<float>(), f[4].Get<float>() });
                } while (q->NextRow());
            }
            loaded = true;
            return groups;
        }

        // Pick the recorded path whose NEAREST waypoint to the leader is closest —
        // the wing the party is actually in. Returns an empty path (so the tank
        // doesn't lead) if even the closest is far away — a different wing or no
        // recording here — so the tank never walks the wrong wing's route.
        // WING_SELECT_MAX_DIST sits far below the ~1000y gap between Scarlet
        // Monastery / Dire Maul wings, yet generous enough to cover a leader briefly
        // off the recorded line.
        static constexpr float WING_SELECT_MAX_DIST = 250.0f;
        static std::vector<Vec3> const& SelectPathForLeader(uint32 mapId, Player* leader)
        {
            static std::vector<Vec3> const empty;
            std::vector<PathGroup> const& groups = EnsureGroups(mapId);
            if (!leader || groups.empty()) return empty;
            float const lx = leader->GetPositionX(), ly = leader->GetPositionY(),
                        lz = leader->GetPositionZ();
            std::vector<Vec3> const* best = nullptr;
            float bestD = WING_SELECT_MAX_DIST;
            for (PathGroup const& g : groups)
            {
                float groupNear = std::numeric_limits<float>::max();
                for (Vec3 const& w : g.pts)
                {
                    float const d = Dist3D(lx, ly, lz, w.x, w.y, w.z);
                    if (d < groupNear) groupNear = d;
                }
                if (groupNear < bestD) { bestD = groupNear; best = &g.pts; }
            }
            return best ? *best : empty;
        }

        static void InvalidatePathCache(uint32 mapId)
        {
            std::lock_guard<std::mutex> lock(g_pathCacheMutex);
            g_pathCache.erase(mapId);
            g_pathCacheLoaded[mapId] = false;
        }

        static void PersistPath(uint32 mapId, uint32 areaId,
                                std::vector<Vec3> const& points, uint32 recordedBy)
        {
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            // Replace only THIS wing's path (same map + area), leaving the other
            // wings of a shared-map dungeon intact.
            tx->Append("DELETE FROM `dungeon_path` WHERE `map_id` = {} AND `area_id` = {}",
                       mapId, areaId);
            for (uint32 i = 0; i < points.size(); ++i)
            {
                Vec3 const& p = points[i];
                tx->Append(
                    "INSERT INTO `dungeon_path` "
                    "(`map_id`,`area_id`,`sequence`,`x`,`y`,`z`,`orientation`,`recorded_by`) "
                    "VALUES ({}, {}, {}, {}, {}, {}, {}, {})",
                    mapId, areaId, i, p.x, p.y, p.z, p.o, recordedBy);
            }
            CharacterDatabase.CommitTransaction(tx);
            InvalidatePathCache(mapId);
        }

        static void ApplyGhostMode(Player* p, bool on)
        {
            if (!p) return;
            // SetGameMaster makes the player invisible/untargetable to hostiles
            // and ignores aggro. SetSpeed takes a multiplier (m_speed_rate),
            // so passing 3.0 = 3× normal, 1.0 = baseline. 3x (down from 5x) is
            // still brisk but slow enough to trace corners precisely on foot.
            p->SetGameMaster(on);
            p->SetGMVisible(!on);
            float const rate = on ? 3.0f : 1.0f;
            // forced=true so the speed-change packet is actually sent to the
            // controlling client (without it the player's own speed often
            // doesn't update).
            p->SetSpeed(MOVE_RUN,        rate, true);
            p->SetSpeed(MOVE_RUN_BACK,   rate, true);
            p->SetSpeed(MOVE_WALK,       rate, true);
            p->SetSpeed(MOVE_SWIM,       rate, true);
            p->SetSpeed(MOVE_SWIM_BACK,  rate, true);
            // Record on FOOT, with gravity. The route must follow walkable ground
            // so the tank-playback can path it — and (the bug this fixes) a
            // DOWNWARD ramp has to actually LOWER your Z. The old fly +
            // disable-gravity left the recorder floating at a fixed height, unable
            // to descend ramps (Deadmines and most dungeons), so a route could
            // never be finished. GM mode still ignores aggro and
            // OpenNearbyDoorsForRecorder swings shut doors open, so nothing walls
            // off the route on the ground. Both flags are forced OFF on entry too,
            // so a player who toggled recording while already flying is grounded.
            p->SetCanFly(false);
            p->SetDisableGravity(false);
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
            constexpr float DOOR_OPEN_RANGE = 12.0f;   // open with lead at ghost speed
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
            st.areaId = player->GetAreaId();   // distinguishes wings of a shared-map dungeon
            st.buffer.push_back({ player->GetPositionX(), player->GetPositionY(),
                                   player->GetPositionZ(), player->GetOrientation() });
            g_recording[guidLow] = std::move(st);
            lock.unlock();
            ApplyGhostMode(player, true);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Path recording |cff66ff66ON|r — ghost mode "
                "+ 3x speed. Run the route through the dungeon (down ramps and all), "
                "then press the bound key again to save.");
            return true;
        }

        // Stop + persist
        RecordingState st = std::move(it->second);
        g_recording.erase(it);
        lock.unlock();

        if (!st.buffer.empty())
        {
            PersistPath(st.mapId, st.areaId, st.buffer, guidLow);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Saved {} waypoint(s) for map {} (wing area {}).",
                uint32(st.buffer.size()), st.mapId, st.areaId);
        }
        ApplyGhostMode(player, false);
        return false;
    }

    bool IsRecordingPath(ObjectGuid playerGuid)
    {
        std::lock_guard<std::mutex> lock(g_recMutex);
        return g_recording.find(playerGuid.GetCounter()) != g_recording.end();
    }

    // Logging out mid-recording would otherwise persist ghost mode (GM + 3x
    // speed) onto the character — they'd relog with broken movement. Drop the
    // recording (without saving the partial buffer) and reset movement state on
    // the way out.
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
        // so the route stays crisp at 3x ghost speed (per-tick work is a few
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

    // Clear only the WING the clearer is standing in (the recorded path nearest
    // them), so clearing SM Library doesn't wipe the other three wings. Returns the
    // number of waypoints removed (0 = nothing recorded near them). The nearest-wing
    // pick mirrors SelectPathForLeader, so it also catches legacy area_id=0 rows.
    uint32 ClearPath(uint32 mapId, Player* clearer)
    {
        uint32 area = 0, removed = 0;
        bool haveArea = false;
        std::vector<PathGroup> const& groups = EnsureGroups(mapId);
        if (clearer)
        {
            float const lx = clearer->GetPositionX(), ly = clearer->GetPositionY(),
                        lz = clearer->GetPositionZ();
            float bestD = WING_SELECT_MAX_DIST;
            for (PathGroup const& g : groups)
            {
                float groupNear = std::numeric_limits<float>::max();
                for (Vec3 const& w : g.pts)
                {
                    float const d = Dist3D(lx, ly, lz, w.x, w.y, w.z);
                    if (d < groupNear) groupNear = d;
                }
                if (groupNear < bestD)
                { bestD = groupNear; area = g.area; removed = uint32(g.pts.size()); haveArea = true; }
            }
        }
        if (!haveArea) return 0;   // no recorded path near the clearer -> nothing to clear
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append("DELETE FROM `dungeon_path` WHERE `map_id` = {} AND `area_id` = {}", mapId, area);
        CharacterDatabase.CommitTransaction(tx);
        InvalidatePathCache(mapId);
        return removed;
    }

    uint32 GetPathWaypointCount(uint32 mapId)
    {
        uint32 n = 0;
        for (auto const& g : EnsureGroups(mapId)) n += uint32(g.pts.size());
        return n;   // total across all wings — a coarse "is anything recorded here" gate
    }

    // True if a recorded path exists for the WING the leader is in (the same
    // proximity test playback uses). Lets the lead-tank gate match TankFollowPath:
    // suppress MoveFollow only when this tank will actually path-follow, else a
    // partially-recorded multi-wing dungeon would strand it (no path AND no follow).
    bool HasPathForLeader(uint32 mapId, Player* leader)
    {
        return SelectPathForLeader(mapId, leader).size() >= 2;
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

        // Pick the recorded path for the WING the leader is actually in (multi-wing
        // dungeons share a map id). Empty -> no recording near the leader -> don't
        // lead (and never walk another wing's route).
        auto const& path = SelectPathForLeader(bot->GetMapId(), leader);
        if (path.size() < 2) { tlog("skip: no recorded path for this wing (<2 wp)"); return; }

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
                    // A tall vertical DROP to the blink target is a fall the navmesh
                    // can't path (the tank wedged at the lip): blink unconditionally,
                    // because LoS down to a landing below a ledge / through a hole is
                    // often occluded by the lip itself — gating on LoS there would
                    // strand the tank. This is the safety net for an angled run-off
                    // drop that the eager cliff-blink below (slope-gated) doesn't
                    // catch.
                    bool const tallDrop =
                        std::fabs(bot->GetPositionZ() - path[blinkIdx].z) > VERTICAL_STEP_TP;
                    // Otherwise (a near-level wedge) only blink if the destination is
                    // in LINE OF SIGHT. An OPEN door (or a navmesh-baked-closed-but-
                    // now-open passage) has clear LoS, so we blink through; a CLOSED
                    // door or a real wall blocks LoS, so we wait. This is what makes a
                    // boss-gated door work: while it's shut the tank waits, and the
                    // instant it opens (boss dead) the next stall cycle sees LoS and
                    // blinks through — without ever teleporting past content still
                    // walled off.
                    if (!tallDrop &&
                        !bot->IsWithinLOS(path[blinkIdx].x, path[blinkIdx].y, path[blinkIdx].z))
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

        // Vertical step the navmesh can't path? (jumps in BFD, dropdowns in
        // Stockades, the giant fall in LBRS …) Teleport across — but ONLY a
        // genuine near-vertical plunge, judged by the recorded next stride's own
        // geometry (dz vs horizontal run), so steep STAIRS and descending RAMPS
        // are walked normally instead of hopped one waypoint per tick. See
        // DROP_SLOPE_RATIO. Checked over the tank's immediate next recorded stride.
        uint32 const stepIdx = std::min(tankNearest + 1, uint32(path.size()) - 1);
        if (stepIdx > tankNearest)
        {
            Vec3 const& from = path[tankNearest];
            Vec3 const& to   = path[stepIdx];
            float const dz    = to.z - from.z;
            float const horiz = std::sqrt((to.x - from.x) * (to.x - from.x) +
                                          (to.y - from.y) * (to.y - from.y));
            if (std::fabs(dz) > VERTICAL_STEP_TP && std::fabs(dz) > horiz * DROP_SLOPE_RATIO)
            {
                tlog("blink: near-vertical drop on next stride, NearTeleport across");
                bot->NearTeleportTo(to.x, to.y, to.z, to.o);
                return;
            }
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
