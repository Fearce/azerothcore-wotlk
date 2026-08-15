#include "PartyPath.h"
#include "PartyFollow.h"
#include "PartyMgr.h"
#include "PartyRotation.h"   // BotLeadDistance

#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "MapCollisionData.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "StringFormat.h"
#include "Player.h"
#include "WorldSession.h"

#include "DetourCommon.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
        // Per-bot ACTIVE steer node: the recorded node the tank is currently walking toward.
        // We only re-issue MovePoint once she REACHES it (not every tick), so the spline runs a
        // whole lookahead between StopMoving's instead of restarting constantly. path.size() (an
        // invalid index) is the sentinel for "no active steer — (re)issue now" (set on halt).
        static std::unordered_map<uint32, uint32>                   g_tankCursor;
        // Per-bot HALT latch: true while the tank is parked because it's already >= lead distance
        // ahead of the leader. Hysteresis (clears at lead - LEAD_HYSTERESIS) stops walk/halt
        // flicker right at the boundary. Guarded by g_tankProgressMutex.
        static std::unordered_map<uint32, bool>                     g_tankHalted;
        // Per-bot LEADER cursor: the leader's nearest waypoint last tick. The
        // per-tick nearest scan starts here so it only ever moves FORWARD along the
        // route — a path that doubles back near itself otherwise let the raw argmin
        // snap to a waypoint BEHIND us and the tank oscillated between two of them.
        // Guarded by g_tankProgressMutex (same cadence, same bot key).
        static std::unordered_map<uint32, uint32>                   g_leaderCursor;

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
        // LEAD_DISTANCE (how far ahead the tank aims ALONG the path) is now per-tank:
        // a local in TankFollowPath sourced from WowPsParty::BotLeadDistance (set by
        // the rotation-editor slider, default 10). It used to be a fixed 30y constant.
        constexpr float  WAYPOINT_REACHED   = 3.5f;
        // Halt hysteresis: once the tank parks at the lead distance, it stays parked until the
        // leader has closed the gap by this much — so it doesn't flicker walk/halt at the edge.
        constexpr float  LEAD_HYSTERESIS    = 2.0f;
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
        // Max path-length a single leader-cursor scan may advance ahead of last
        // tick. Far larger than any one-tick leader move (~7y), but small enough
        // that a route doubling back on the same ground (up a ramp to a boss, back
        // down the same ramp) can't snap the cursor to the post-boss leg. A genuine
        // long jump (teleport / recorded drop) still resyncs via the unbounded scan.
        constexpr float  CURSOR_ADVANCE_WINDOW = 40.0f;
        // The tank's OWN on-route position (tankNearest) is resolved within a BAND
        // around the leader's cursor, never by a global argmin. A hub dungeon that
        // threads the same central chamber several times (Halls of Stone: every wing
        // returns through the centre) stacks a later pass's waypoints physically on top
        // of the current pass's, so a global nearest-scan on the tank's position snapped
        // tankNearest to whichever pass was momentarily closest — and when it caught a
        // LATER pass, the steer lookahead (tankNearest + lead) leapt the tank a whole
        // wing ahead, skipping the wing in between (Kevin: Halls of Stone wing-skip).
        // The leader cursor is already hub-safe (forward-only + windowed), so anchor the
        // tank's resolution to it: search only the stretch of route from TANK_NEAREST_BACK
        // behind the cursor to LEAD_DISTANCE + TANK_NEAREST_FWD_SLACK ahead — the band a
        // correctly-leading tank can occupy, far shorter (in path length) than a wing
        // loop, so a later hub pass can never win. The back margin preserves the halt's
        // `ahead` detection (the tank must still resolve BEHIND the cursor when the
        // leader has caught up to it).
        constexpr float  TANK_NEAREST_BACK      = 15.0f;
        constexpr float  TANK_NEAREST_FWD_SLACK = 20.0f;

        static float Dist3D(float ax, float ay, float az, float bx, float by, float bz)
        {
            float dx = ax - bx, dy = ay - by, dz = az - bz;
            return std::sqrt(dx*dx + dy*dy + dz*dz);
        }

        // True if the tank can WALK to (x,y,z) by a reasonably direct navmesh route
        // — a complete normal path no longer than ~2.5x (min +8y) the straight-line
        // distance. This is the "if it could easily just walk there, walk" gate:
        // before any blink/teleport we ask the navmesh, and only teleport when
        // there's NO usable path (a real hole / jump / sheer drop the mmap can't
        // connect) or the only route is a long detour around a recorded shortcut.
        // A genuinely walkable slope or a long-but-clear stride returns NORMAL and
        // gets walked instead of hopped. (No mmaps on the map -> SHORTCUT/NOT_USING
        // -> returns false -> teleport, same as before.)
        static bool NavWalkable(Player* bot, float x, float y, float z, float straight)
        {
            PathGenerator gen(bot);
            bool const ok = gen.CalculatePath(x, y, z, false);
            PathType const t = gen.GetPathType();
            if (!ok)
                return false;
            if (t & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT
                     | PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY))
                return false;
            if (!(t & PATHFIND_NORMAL))
                return false;
            return gen.getPathLength() <= std::max(straight * 2.5f, straight + 8.0f);
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

        // ================== Auto-routing: no recording needed ==================
        //
        // A recorded route is a human tracing the dungeon once. An AUTO route is the
        // server working it out live: read the map's boss list out of
        // DungeonEncounter.dbc, walk the tank to whichever boss is nearest and still
        // standing, re-pick when it dies, and pull the lever when the way is shut.
        //
        // The pathfinder is the one piece that had to be built rather than borrowed.
        // AzerothCore's PathGenerator caps every query at MAX_PATH_LENGTH = 74 polys
        // (~296 yards) and shares ONE dtNavMeshQuery per map with a 1024-node A* pool
        // — far too small to search from one end of a dungeon to the other, and not
        // safe to run concurrently with the movement code that owns it. So routing
        // runs its own thread_local query with a 65535-node pool (Detour's ceiling:
        // node indices are 16-bit) over a 1024-poly corridor.
        //
        // What comes out is a dense polyline, the same shape a recording produces, so
        // TankFollowPath's steer/lead-halt/blink machinery drives it unchanged — and
        // the tank still only advances as far as the LEADER lets it, exactly as on a
        // recorded route. Auto-routing decides WHERE, never how fast.

        constexpr uint32 LR_MAX_POLYS    = 1024;
        constexpr uint32 LR_MAX_STRAIGHT = 256;
        constexpr int    LR_NODE_POOL    = 65535;

        // Density of the generated line. The string-pulled corridor is corners only;
        // TankFollowPath's steer lookahead needs nodes closer together than
        // WAYPOINT_REACHED, or every candidate node reads as "already reached" and the
        // tank freezes. 1.8y matches what the recorder lays down on a straight.
        constexpr float  ROUTE_NODE_STEP  = 1.8f;
        // Only materialise this much of the corridor per build. The route is rebuilt
        // as the party advances, so densifying and Z-pinning the far half of a
        // 900-yard corridor is VMAP work we would throw away.
        constexpr float  ROUTE_PREFIX_MAX = 250.0f;

        // Rebuild cadence. The route is always built FROM THE LEADER, so it stays
        // short and the leader sits at its head — which is exactly what the playback's
        // leader-cursor expects.
        constexpr uint32 ROUTE_TTL_MS      = 12000;
        constexpr float  ROUTE_REBUILD_MOVE = 35.0f;   // leader this far from the build origin -> rebuild

        // Objective bookkeeping.
        constexpr float  OBJECTIVE_ARRIVE       = 15.0f;   // "we're at the boss"
        constexpr float  OBJECTIVE_ALIVE_RADIUS = 90.0f;   // ...and nothing of its entry lives within this
        constexpr uint32 OBJECTIVE_CHECK_MS     = 1000;    // that sweep is a grid visit — pace it
        constexpr uint32 AUTO_STATE_IDLE_MS     = 30 * 60 * 1000;   // drop an untouched instance's state

        // Gate pulling. GATE_ARRIVE is how close to the dead end of a blocked corridor
        // the tank must be before it starts looking — clicking things from halfway down
        // the hall would fire on scenery that has nothing to do with the obstruction.
        constexpr float  GATE_ARRIVE       = 15.0f;
        constexpr float  GATE_SEARCH_RANGE = 30.0f;
        constexpr uint32 GATE_RETRY_MS     = 6000;

        // ---- Long-range navmesh routing --------------------------------------

        static dtNavMeshQuery* AcquireRouteQuery(dtNavMesh const* mesh)
        {
            // One query per thread, re-init()'d per call. init() rebinds the mesh and
            // only reallocates when the existing pool is too small, so this reuses the
            // ~2 MB node pool across calls while staying correct when an instance is
            // unloaded and a different map's mesh turns up at the same address.
            thread_local std::unique_ptr<dtNavMeshQuery, decltype(&dtFreeNavMeshQuery)>
                query(nullptr, &dtFreeNavMeshQuery);
            if (!query)
                query.reset(dtAllocNavMeshQuery());
            if (!query || dtStatusFailed(query->init(mesh, LR_NODE_POOL)))
                return nullptr;
            return query.get();
        }

        // String-pulled corner points from the walker to (tx,ty,tz), in world space.
        // `reached` says whether the corridor actually arrived — false means the
        // destination is walled off from here, which is the signal a gate needs
        // opening. Returns false when there is no corridor at all (no mmaps, off-mesh
        // start, unreachable target).
        static bool SearchRouteCorners(Player* walker, float tx, float ty, float tz,
                                       std::vector<Vec3>& corners, bool& reached, bool& budgetCapped)
        {
            corners.clear();
            reached = false;
            budgetCapped = false;
            if (!walker || !walker->IsInWorld() || !walker->GetMap())
                return false;

            dtNavMesh const* mesh = walker->GetMap()->GetMapCollisionData().GetMMapData().GetNavMesh();
            if (!mesh)
                return false;   // map has no mmaps — auto-routing is simply off here
            dtNavMeshQuery* query = AcquireRouteQuery(mesh);
            if (!query)
                return false;

            dtQueryFilterExt filter;   // PathGenerator's "assume Player" flag set
            filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_MAGMA));
            filter.setExcludeFlags(0);

            // Detour's coordinate order is {y, z, x}.
            float const from[VERTEX_SIZE] = { walker->GetPositionY(), walker->GetPositionZ(),
                                              walker->GetPositionX() };
            float const to[VERTEX_SIZE]   = { ty, tz, tx };
            float extents[VERTEX_SIZE]     = { 3.0f,  5.0f, 3.0f };
            float tallExtents[VERTEX_SIZE] = { 3.0f, 50.0f, 3.0f };   // GetPolyByLocation's retry box

            auto snap = [&](float const* pt, dtPolyRef& ref, float* nearest)
            {
                if (dtStatusSucceed(query->findNearestPoly(pt, extents, &filter, &ref, nearest))
                    && ref != INVALID_POLYREF)
                    return true;
                return dtStatusSucceed(query->findNearestPoly(pt, tallExtents, &filter, &ref, nearest))
                    && ref != INVALID_POLYREF;
            };

            dtPolyRef fromRef = INVALID_POLYREF, toRef = INVALID_POLYREF;
            float fromPt[VERTEX_SIZE] = { 0.0f, 0.0f, 0.0f };
            float toPt[VERTEX_SIZE]   = { 0.0f, 0.0f, 0.0f };
            if (!snap(from, fromRef, fromPt) || !snap(to, toRef, toPt))
                return false;

            thread_local std::vector<dtPolyRef> corridor(LR_MAX_POLYS);
            int npolys = 0;
            if (dtStatusFailed(query->findPath(fromRef, toRef, fromPt, toPt, &filter,
                                               corridor.data(), &npolys, int(LR_MAX_POLYS)))
                || npolys <= 0)
                return false;
            reached = (corridor[npolys - 1] == toRef);
            // A corridor that filled the whole buffer was cut off by OUR budget, not by
            // the dungeon. That distinction matters: "stopped short" is the signal that
            // a gate is shut, and a budget cut must never be mistaken for one.
            budgetCapped = (uint32(npolys) >= LR_MAX_POLYS);

            // The funnel keeps every string-pulled segment inside the corridor, so the
            // line is walkable by construction — we need PathGenerator's density, not
            // its smooth-path iteration.
            thread_local std::vector<float>         pts(LR_MAX_STRAIGHT * VERTEX_SIZE);
            thread_local std::vector<unsigned char> flags(LR_MAX_STRAIGHT);
            thread_local std::vector<dtPolyRef>     refs(LR_MAX_STRAIGHT);
            int ncorners = 0;
            if (dtStatusFailed(query->findStraightPath(fromPt, toPt, corridor.data(), npolys,
                                                       pts.data(), flags.data(), refs.data(),
                                                       &ncorners, int(LR_MAX_STRAIGHT)))
                || ncorners < 2)
                return false;

            corners.reserve(size_t(ncorners));
            for (int i = 0; i < ncorners; ++i)
                corners.push_back({ pts[i * VERTEX_SIZE + 2], pts[i * VERTEX_SIZE + 0],
                                    pts[i * VERTEX_SIZE + 1], 0.0f });
            return true;
        }

        static float PolylineLength(std::vector<Vec3> const& pts)
        {
            float len = 0.0f;
            for (size_t i = 1; i < pts.size(); ++i)
                len += Dist3D(pts[i - 1].x, pts[i - 1].y, pts[i - 1].z, pts[i].x, pts[i].y, pts[i].z);
            return len;
        }

        // Walking distance to a point, or -1 when it can't be walked to. Used to order
        // objectives: a boss 40y away through a wall must lose to one 120y away down
        // the corridor.
        static float RouteDistance(Player* walker, float tx, float ty, float tz)
        {
            std::vector<Vec3> corners;
            bool reached = false, capped = false;
            if (!SearchRouteCorners(walker, tx, ty, tz, corners, reached, capped) || !reached)
                return -1.0f;
            return PolylineLength(corners);
        }

        // Full build: corridor -> dense, ground-pinned polyline the playback can drive.
        // `gated` is the narrow claim that something is physically shut between here and
        // the destination: the corridor stopped short of its own accord, AND the line we
        // handed back really does end there rather than at our prefix budget.
        static bool BuildRoute(Player* walker, float tx, float ty, float tz,
                               std::vector<Vec3>& out, bool& gated)
        {
            out.clear();
            gated = false;
            std::vector<Vec3> corners;
            bool reached = false, capped = false;
            if (!SearchRouteCorners(walker, tx, ty, tz, corners, reached, capped) || corners.size() < 2)
                return false;

            float laid = 0.0f;
            size_t laidThrough = 0;
            out.push_back(corners.front());
            for (size_t i = 1; i < corners.size() && laid < ROUTE_PREFIX_MAX; ++i)
            {
                laidThrough = i;
                Vec3 const& a = corners[i - 1];
                Vec3 const& b = corners[i];
                float const seg = Dist3D(a.x, a.y, a.z, b.x, b.y, b.z);
                if (seg < 0.01f)
                    continue;
                uint32 const steps = std::max(1u, uint32(std::ceil(seg / ROUTE_NODE_STEP)));
                float const heading = std::atan2(b.y - a.y, b.x - a.x);
                for (uint32 s = 1; s <= steps; ++s)
                {
                    float const t = float(s) / float(steps);
                    out.push_back({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                    a.z + (b.z - a.z) * t, heading });
                }
                laid += seg;
            }
            if (out.size() < 2)
                return false;
            out.front().o = out[1].o;

            // findStraightPath's Z is the poly plane, which floats over stairs and dips
            // into ramps. Pin every node to real walkable ground, as PathGenerator does.
            for (Vec3& p : out)
                walker->UpdateAllowedPositionZ(p.x, p.y, p.z);

            bool const prefixTruncated = laidThrough + 1 < corners.size();
            gated = !reached && !capped && !prefixTruncated;
            return true;
        }

        // ---- The dungeon's bosses, and where they stand ----------------------

        struct BossObjective
        {
            uint32      entry = 0;
            std::string name;
            float       x = 0.0f, y = 0.0f, z = 0.0f;
        };

        // (mapId, difficulty) -> bosses. Spawn positions are static world data, so one
        // build per map serves the whole process.
        static std::mutex                                            g_bossMutex;
        static std::unordered_map<uint32, std::vector<BossObjective>> g_bossCache;

        // A script-summoned boss has no spawn to walk to — but its fight still happens
        // somewhere, and Blizzard names the ENCOUNTER after the thing you walk up to
        // rather than the thing you finally kill. Gundrak's "Drakkari Colossus" credits
        // the summoned Drakkari Elemental; Utgarde Pinnacle's "Svala Sorrowgrave"
        // credits her post-ritual form. In both, the pre-fight NPC is spawned right
        // where the party needs to stand.
        //
        // So match the encounter name against creatures spawned on this map, exact or
        // whole-word prefix either way. UNIQUENESS is what makes this safe: a name that
        // matches two spawns is discarded rather than guessed at, because a wrong
        // stand-in would send the tank somewhere arbitrary. Elites are preferred but not
        // required — Utgarde Pinnacle's Svala is rank 0 while she runs the ritual, and
        // demanding elite silently dropped the exact case this exists for.
        //
        // Verify any change with `python tools\tank-route.py <map>`, which mirrors this
        // rule and prints every stand-in it resolves.
        static bool ResolveStandIn(uint32 mapId, std::string const& encounterName, BossObjective& out)
        {
            std::string escaped;
            escaped.reserve(encounterName.size() + 8);
            for (char c : encounterName)
            {
                if (c == '\'' || c == '\\')
                    escaped += '\\';
                escaped += c;
            }

            // Both prefix directions, resolved in SQL so only real candidates come back.
            QueryResult q = WorldDatabase.Query(
                "SELECT ct.`name`, ct.`rank`, c.`position_x`, c.`position_y`, c.`position_z` "
                "FROM `creature` c JOIN `creature_template` ct ON ct.`entry` = c.`id1` "
                "WHERE c.`map` = {} AND CHAR_LENGTH(ct.`name`) >= 4 AND ("
                "  ct.`name` = '{}' OR '{}' LIKE CONCAT(ct.`name`, ' %') "
                "  OR ct.`name` LIKE CONCAT('{}', ' %')) "
                "GROUP BY ct.`name`, ct.`rank`, c.`position_x`, c.`position_y`, c.`position_z`",
                mapId, escaped, escaped, escaped);
            if (!q)
                return false;

            std::vector<BossObjective> hits, elites;
            do
            {
                Field* f = q->Fetch();
                BossObjective candidate;
                candidate.name = f[0].Get<std::string>();
                candidate.x    = f[2].Get<float>();
                candidate.y    = f[3].Get<float>();
                candidate.z    = f[4].Get<float>();
                if (f[1].Get<uint8>() >= 1)
                    elites.push_back(candidate);
                hits.push_back(std::move(candidate));
            } while (q->NextRow());

            std::vector<BossObjective> const& chosen = (elites.size() == 1) ? elites : hits;
            if (chosen.size() != 1)
                return false;   // ambiguous (or nothing) — never guess where to send the tank

            out = chosen.front();
            LOG_INFO("module", "[WowPsParty AutoRoute] map {}: '{}' is summoned — routing to '{}', "
                               "who starts the fight there",
                     mapId, encounterName, out.name);
            return true;
        }

        static std::vector<BossObjective> const& EnsureBossObjectives(uint32 mapId, uint8 difficulty)
        {
            std::lock_guard<std::mutex> lock(g_bossMutex);
            uint32 const key = (mapId << 8) | difficulty;
            auto cached = g_bossCache.find(key);
            if (cached != g_bossCache.end())
                return cached->second;

            std::vector<BossObjective>& out = g_bossCache[key];

            // Heroic keys are often empty because the encounter is registered under
            // normal only — fall back rather than routing nowhere on heroic.
            DungeonEncounterList const* encounters =
                sObjectMgr->GetDungeonEncounterList(mapId, Difficulty(difficulty));
            if ((!encounters || encounters->empty()) && difficulty != DUNGEON_DIFFICULTY_NORMAL)
                encounters = sObjectMgr->GetDungeonEncounterList(mapId, DUNGEON_DIFFICULTY_NORMAL);
            if (!encounters || encounters->empty())
            {
                LOG_INFO("module", "[WowPsParty AutoRoute] map {} difficulty {}: no dungeon "
                                   "encounters registered — auto-routing unavailable here",
                         mapId, uint32(difficulty));
                return out;
            }

            std::string inList;
            std::vector<std::pair<uint32, std::string>> credits;   // creditEntry -> encounter name
            for (DungeonEncounter const* enc : *encounters)
            {
                if (!enc || enc->creditType != ENCOUNTER_CREDIT_KILL_CREATURE || !enc->creditEntry)
                    continue;   // spell-credit encounters have no creature to walk to
                if (!inList.empty())
                    inList += ',';
                inList += std::to_string(enc->creditEntry);
                char const* encounterName = enc->dbcEntry ? enc->dbcEntry->encounterName[0] : nullptr;
                credits.emplace_back(enc->creditEntry, encounterName ? encounterName : "");
            }
            if (credits.empty())
                return out;

            // There is no entry->spawn index in memory, so this is the router's one
            // SQL — a single statement, once per map, for the life of the process.
            QueryResult q = WorldDatabase.Query(
                "SELECT `id1`, `position_x`, `position_y`, `position_z` FROM `creature` "
                "WHERE `map` = {} AND `id1` IN ({}) ORDER BY `guid`", mapId, inList);
            std::unordered_set<uint32> spawned;
            if (q)
            {
                do
                {
                    Field* f = q->Fetch();
                    uint32 const entry = f[0].Get<uint32>();
                    if (!spawned.insert(entry).second)
                        continue;   // first spawn wins; a boss listed twice is one objective
                    BossObjective obj;
                    obj.entry = entry;
                    obj.x = f[1].Get<float>();
                    obj.y = f[2].Get<float>();
                    obj.z = f[3].Get<float>();
                    if (CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry))
                        obj.name = tmpl->Name;
                    if (obj.name.empty())
                        obj.name = Acore::StringFormat("creature {}", entry);
                    out.push_back(std::move(obj));
                } while (q->NextRow());
            }

            // Whatever is left is script-summoned and has nowhere to walk to — but the
            // fight still happens SOMEWHERE, and a stand-in usually marks the spot.
            uint32 viaStandIn = 0;
            for (auto const& credit : credits)
            {
                if (spawned.count(credit.first) || credit.second.empty())
                    continue;
                BossObjective standIn;
                if (!ResolveStandIn(mapId, credit.second, standIn))
                    continue;
                standIn.entry = credit.first;   // still killed/credited as the summoned boss
                out.push_back(std::move(standIn));
                ++viaStandIn;
            }

            LOG_INFO("module", "[WowPsParty AutoRoute] map {} difficulty {}: {} of {} encounter "
                               "boss(es) have somewhere to route to ({} via a stand-in)",
                     mapId, uint32(difficulty), uint32(out.size()), uint32(credits.size()),
                     viaStandIn);
            return out;
        }

        // Anything of this entry still breathing near the tank. Only meaningful once
        // the tank is AT the objective: further away the creature grid isn't loaded and
        // an empty result would read as "already dead".
        struct LiveCreatureOfEntry
        {
            LiveCreatureOfEntry(WorldObject const* src, uint32 entry, float range)
                : _src(src), _entry(entry), _range(range) {}
            bool operator()(Creature* c) const
            {
                return c && c->GetEntry() == _entry && c->IsAlive()
                    && _src->IsWithinDist(c, _range, false);
            }
            WorldObject const* _src;
            uint32 _entry;
            float  _range;
        };

        static bool BossStillStanding(Player* viewer, BossObjective const& obj)
        {
            std::list<Creature*> found;
            LiveCreatureOfEntry check(viewer, obj.entry, OBJECTIVE_ALIVE_RADIUS);
            Acore::CreatureListSearcher<LiveCreatureOfEntry> searcher(viewer, found, check);
            Cell::VisitObjects(viewer, searcher, OBJECTIVE_ALIVE_RADIUS);
            return !found.empty();
        }

        // ---- Per-instance run state ------------------------------------------

        using RouteRef = std::shared_ptr<std::vector<Vec3> const>;

        struct AutoRouteState
        {
            uint32   mapId       = 0;
            uint32   targetEntry = 0;          // the boss we're walking to (0 = none picked yet)
            RouteRef route;
            uint32   generation  = 0;          // bumped per rebuild; playback cursors reset with it
            uint32   builtMs     = 0;
            float    builtX = 0.0f, builtY = 0.0f, builtZ = 0.0f;
            bool     blocked     = false;      // corridor can't reach the objective — a gate is shut
            uint32   checkedMs   = 0;          // last "is the objective dead yet" sweep
            uint32   touchedMs   = 0;
            uint32   lastGateMs  = 0;
            std::unordered_set<uint32> cleared;     // boss entries confirmed down this run
            std::unordered_set<uint32> usedGates;   // gameobject guids we already pulled
        };

        // Keyed by instance id. AzerothCore hands a fresh instance a fresh id, so a
        // re-run of the same dungeon starts clean; the idle sweep reclaims the rest.
        static std::mutex                                 g_autoMutex;
        static std::unordered_map<uint32, AutoRouteState> g_auto;

        // Route generations are drawn from ONE process-wide counter rather than per
        // instance. A bot that walks out of one dungeon and into another must always see
        // the number change — two instances independently counting to 7 would leave it
        // driving the new polyline with the old polyline's cursors.
        static uint32 NextRouteGeneration()
        {
            static std::atomic<uint32> counter{ 0 };
            return counter.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        // Everything the router needs about an instance, copied out so the expensive
        // work (navmesh search, grid searches) happens with no lock held.
        struct AutoSnapshot
        {
            uint32   generation  = 0;
            uint32   targetEntry = 0;
            RouteRef route;
            uint32   builtMs     = 0;
            float    builtX = 0.0f, builtY = 0.0f, builtZ = 0.0f;
            bool     blocked     = false;
            uint32   checkedMs   = 0;
            std::unordered_set<uint32> cleared;
        };

        // Fetch (creating on first sight) the state for an instance, dropping any other
        // instance nobody has touched in a long while. Caller must hold g_autoMutex.
        static AutoRouteState& TouchAutoState(uint32 instanceId, uint32 mapId, uint32 now)
        {
            for (auto it = g_auto.begin(); it != g_auto.end(); )
            {
                if (it->first != instanceId && getMSTimeDiff(it->second.touchedMs, now) > AUTO_STATE_IDLE_MS)
                    it = g_auto.erase(it);
                else
                    ++it;
            }

            AutoRouteState& st = g_auto[instanceId];
            if (st.mapId != mapId)
                st = AutoRouteState{};        // recycled id, or first visit
            st.mapId     = mapId;
            st.touchedMs = now;
            return st;
        }

        static AutoSnapshot SnapshotAutoState(uint32 instanceId, uint32 mapId)
        {
            uint32 const now = getMSTime();
            std::lock_guard<std::mutex> lock(g_autoMutex);
            AutoRouteState const& st = TouchAutoState(instanceId, mapId, now);

            AutoSnapshot snap;
            snap.generation  = st.generation;
            snap.targetEntry = st.targetEntry;
            snap.route       = st.route;
            snap.builtMs     = st.builtMs;
            snap.builtX      = st.builtX;
            snap.builtY      = st.builtY;
            snap.builtZ      = st.builtZ;
            snap.blocked     = st.blocked;
            snap.checkedMs   = st.checkedMs;
            snap.cleared     = st.cleared;
            return snap;
        }

        // ---- Gates: the lever/gong/altar that a dungeon hides its next wing behind --
        //
        // When a corridor stops short of the boss, the way is physically shut — the
        // door is baked closed in the navmesh. That is the signal to look for the thing
        // a player would click.
        //
        // ONLY `GAMEOBJECT_TYPE_BUTTON`, and that restriction is load-bearing. In WoW's
        // data model a BUTTON *is* a switch that changes a door or a state, which is
        // exactly what we want; a GOOBER is the catch-all "player interacts, some script
        // runs" type, and it is full of things that would be a disaster to click blind —
        // Icecrown Citadel's seven Scourge Transporters (which would teleport the tank
        // away from its party), Blackrock Depths' Dark Iron Keg Shotgun (which starts
        // the Grim Guzzler bar brawl), boss-summoning spheres. Every gate verified to
        // actually open a way is a BUTTON: Gundrak's three altars, Halls of Stone's
        // Tribunal console, Utgarde Keep's forge bellows, Deadmines' and Shadowfang's
        // levers, Blackrock Depths' Lyceum runes.
        //
        // Under-including costs a visible stall that `tank-route.py --chain` explains.
        // Over-including scatters the party. So: BUTTON only.
        //
        // A LOCKED object is left alone regardless — opening one without its key would
        // be a cheat rather than a convenience — and a door that a boss kill opens is
        // already handled by the playback's wait-for-line-of-sight.

        struct UsableGateCheck
        {
            UsableGateCheck(WorldObject const* src, float range, std::unordered_set<uint32> const& used)
                : _src(src), _range(range), _used(used) {}
            bool operator()(GameObject* go) const
            {
                if (!go || !go->isSpawned() || go->GetGoState() != GO_STATE_READY)
                    return false;
                GameObjectTemplate const* tmpl = go->GetGOInfo();
                if (!tmpl)
                    return false;
                if (tmpl->type != GAMEOBJECT_TYPE_BUTTON)
                    return false;
                if (tmpl->GetLockId())
                    return false;   // needs a key — not ours to force
                if (_used.count(go->GetGUID().GetCounter()))
                    return false;
                return _src->IsWithinDist(go, _range, false);
            }
            WorldObject const* _src;
            float _range;
            std::unordered_set<uint32> const& _used;
        };

        // Click the nearest untried gate. Returns the name of what was pulled, empty if
        // there was nothing to pull. The already-pulled set lives on the instance state
        // whether or not this run is auto-routing — a recorded route walks into the same
        // shut doors, and must get the same lever.
        static std::string PullNearestGate(Player* bot, uint32 instanceId)
        {
            uint32 const now = getMSTime();
            std::unordered_set<uint32> used;
            {
                std::lock_guard<std::mutex> lock(g_autoMutex);
                AutoRouteState& st = TouchAutoState(instanceId, bot->GetMapId(), now);
                if (st.lastGateMs && getMSTimeDiff(st.lastGateMs, now) < GATE_RETRY_MS)
                    return {};
                used = st.usedGates;
            }

            std::list<GameObject*> gates;
            UsableGateCheck check(bot, GATE_SEARCH_RANGE, used);
            Acore::GameObjectListSearcher<UsableGateCheck> searcher(bot, gates, check);
            Cell::VisitObjects(bot, searcher, GATE_SEARCH_RANGE);
            if (gates.empty())
                return {};

            GameObject* best = nullptr;
            float bestD = std::numeric_limits<float>::max();
            for (GameObject* go : gates)
            {
                float const d = bot->GetDistance(go);
                if (d < bestD) { bestD = d; best = go; }
            }
            if (!best)
                return {};

            uint32 const guidLow = best->GetGUID().GetCounter();
            std::string const name = best->GetNameForLocaleIdx(LOCALE_enUS);
            {
                std::lock_guard<std::mutex> lock(g_autoMutex);
                AutoRouteState& st = TouchAutoState(instanceId, bot->GetMapId(), getMSTime());
                st.usedGates.insert(guidLow);
                st.lastGateMs = getMSTime();
            }
            best->Use(bot);
            LOG_INFO("module", "[WowPsParty AutoRoute] {} pulled gate '{}' (entry {}) at {:.1f}y — "
                               "route to the objective was blocked",
                     bot->GetName(), name, best->GetEntry(), bestD);
            return name;
        }

        // ---- The routing tick -------------------------------------------------

        // What the tank should drive this tick. An empty route means "auto-routing has
        // nothing for you here" — the caller then leaves the tank to plain following.
        struct AutoRoutePlan
        {
            RouteRef    route;
            uint32      generation = 0;
            bool        blocked    = false;   // the objective is walled off from here
            std::string objective;
        };

        // Nearest boss still standing, measured by REAL walking distance — a boss 40y
        // away through a wall must lose to one 120y down the corridor. When nothing is
        // walkable (a wing still sealed behind its gate) fall back to the nearest by
        // straight line, so the tank at least walks up to the obstruction where the
        // gate logic can see it.
        static BossObjective const* PickObjective(Player* bot, std::vector<BossObjective> const& all,
                                                  std::unordered_set<uint32> const& cleared)
        {
            // Each walk probe is a full A* over the dungeon, so only the closest few by
            // straight line are worth measuring properly — a boss on the far side of the
            // map is never the nearest one to walk to. Blackwing Lair has 18 encounters;
            // probing all of them on every kill would be a visible hitch.
            constexpr size_t WALK_PROBES = 5;

            std::vector<BossObjective const*> candidates;
            for (BossObjective const& obj : all)
                if (!cleared.count(obj.entry))
                    candidates.push_back(&obj);
            if (candidates.empty())
                return nullptr;

            float const bx = bot->GetPositionX(), by = bot->GetPositionY(), bz = bot->GetPositionZ();
            std::sort(candidates.begin(), candidates.end(),
                      [bx, by, bz](BossObjective const* a, BossObjective const* b)
                      {
                          return Dist3D(bx, by, bz, a->x, a->y, a->z)
                               < Dist3D(bx, by, bz, b->x, b->y, b->z);
                      });

            BossObjective const* byWalk = nullptr;
            float bestWalk = std::numeric_limits<float>::max();
            for (size_t i = 0; i < candidates.size() && i < WALK_PROBES; ++i)
            {
                float const walk = RouteDistance(bot, candidates[i]->x, candidates[i]->y, candidates[i]->z);
                if (walk >= 0.0f && walk < bestWalk) { bestWalk = walk; byWalk = candidates[i]; }
            }
            // Nothing walkable is normal early in a run — a wing stays sealed until its
            // gate opens. Head for the nearest one anyway, so the tank walks up to the
            // obstruction where the gate logic can actually see it.
            return byWalk ? byWalk : candidates.front();
        }

        static void CommitAutoState(uint32 instanceId, AutoSnapshot const& snap)
        {
            std::lock_guard<std::mutex> lock(g_autoMutex);
            auto it = g_auto.find(instanceId);
            if (it == g_auto.end())
                return;
            AutoRouteState& st = it->second;
            st.targetEntry = snap.targetEntry;
            st.route       = snap.route;
            st.generation  = snap.generation;
            st.builtMs     = snap.builtMs;
            st.builtX      = snap.builtX;
            st.builtY      = snap.builtY;
            st.builtZ      = snap.builtZ;
            st.blocked     = snap.blocked;
            st.checkedMs   = snap.checkedMs;
            st.cleared     = snap.cleared;
        }

        // Advance the objective if the last one died, then keep a fresh route to it.
        // The route is always built FROM THE LEADER, which is what puts the leader at
        // its head — exactly where the playback's leader-cursor expects to find them.
        // Everything expensive runs on a snapshot with no lock held; only one thread
        // ever drives a given instance, so the commit at the end can't race a peer.
        static AutoRoutePlan EnsureAutoRoute(Player* bot, Player* leader)
        {
            AutoRoutePlan plan;
            Map* map = bot->GetMap();
            if (!map)
                return plan;

            uint32 const mapId      = map->GetId();
            uint32 const instanceId = map->GetInstanceId();
            std::vector<BossObjective> const& bosses = EnsureBossObjectives(mapId, map->GetSpawnMode());
            if (bosses.empty())
                return plan;

            AutoSnapshot snap = SnapshotAutoState(instanceId, mapId);
            uint32 const now = getMSTime();

            BossObjective const* target = nullptr;
            for (BossObjective const& obj : bosses)
                if (obj.entry == snap.targetEntry) { target = &obj; break; }

            // Is the objective done? The check is a 90-yard grid sweep, so it runs at
            // most once a second, and only from close enough that the creature grid is
            // loaded — further out "nothing alive here" would simply be a lie.
            if (target
                && getMSTimeDiff(snap.checkedMs, now) >= OBJECTIVE_CHECK_MS
                && Dist3D(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                          target->x, target->y, target->z) <= OBJECTIVE_ARRIVE)
            {
                snap.checkedMs = now;
                if (!BossStillStanding(bot, *target))
                {
                    LOG_INFO("module", "[WowPsParty AutoRoute] {} is down — picking the next objective",
                             target->name);
                    snap.cleared.insert(target->entry);
                    snap.targetEntry = 0;
                    target = nullptr;
                }
            }

            bool objectiveChanged = false;
            if (!target)
            {
                target = PickObjective(bot, bosses, snap.cleared);
                if (!target)
                {
                    // Everything we know how to route to is down. Drop the route and let
                    // the tank fall back to following the leader out.
                    snap.targetEntry = 0;
                    snap.route.reset();
                    snap.blocked = false;
                    snap.generation = NextRouteGeneration();
                    CommitAutoState(instanceId, snap);
                    return plan;
                }
                snap.targetEntry = target->entry;
                objectiveChanged = true;
                LOG_INFO("module", "[WowPsParty AutoRoute] map {}: next objective is {} ({:.0f}y away)",
                         mapId, target->name,
                         Dist3D(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                target->x, target->y, target->z));
            }
            plan.objective = target->name;

            bool const stale = objectiveChanged
                || !snap.route || snap.route->size() < 2
                || getMSTimeDiff(snap.builtMs, now) > ROUTE_TTL_MS
                || Dist3D(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                          snap.builtX, snap.builtY, snap.builtZ) > ROUTE_REBUILD_MOVE;
            if (stale)
            {
                std::vector<Vec3> fresh;
                bool gated = false;
                bool const built = BuildRoute(leader, target->x, target->y, target->z, fresh, gated);
                // Record where we searched from either way, so a failed search doesn't
                // re-run every single tick.
                snap.builtMs = now;
                snap.builtX  = leader->GetPositionX();
                snap.builtY  = leader->GetPositionY();
                snap.builtZ  = leader->GetPositionZ();
                if (built)
                {
                    snap.route   = std::make_shared<std::vector<Vec3> const>(std::move(fresh));
                    snap.blocked = gated;
                    snap.generation = NextRouteGeneration();
                    LOG_INFO("module", "[WowPsParty AutoRoute] route to {}: {} node(s), {}",
                             target->name, uint32(snap.route->size()),
                             gated ? "corridor stops short — a gate is shut" : "clear");
                }
                else
                {
                    // No corridor AT ALL usually means the leader is momentarily off the
                    // mesh (mid-jump, mid-fall), not that the dungeon is gated — a genuinely
                    // shut door still returns a partial corridor up to it. So keep the last
                    // route and the last verdict rather than inventing a gate to hunt for.
                    LOG_INFO("module", "[WowPsParty AutoRoute] no navmesh corridor from {} to {} "
                                       "right now — keeping the previous route",
                             leader->GetName(), target->name);
                }
            }

            plan.route      = snap.route;
            plan.generation = snap.generation;
            plan.blocked    = snap.blocked;
            CommitAutoState(instanceId, snap);
            return plan;
        }

        // Which polyline the tank drives, and where it came from. A recorded route
        // always wins: it is a human's own line through the place, and Kevin's existing
        // recordings must keep behaving exactly as they did.
        struct TankPath
        {
            std::vector<Vec3> const* pts        = nullptr;   // borrowed — never owned
            RouteRef                 hold;                   // keeps an auto route alive while read
            bool                     isAuto     = false;
            uint32                   generation = 0;
            bool                     blocked    = false;
            std::string              objective;
        };

        static TankPath ResolveTankPath(Player* bot, Player* leader)
        {
            TankPath out;
            std::vector<Vec3> const& recorded = SelectPathForLeader(bot->GetMapId(), leader);
            if (recorded.size() >= 2)
            {
                out.pts = &recorded;
                return out;
            }
            AutoRoutePlan plan = EnsureAutoRoute(bot, leader);
            if (!plan.route || plan.route->size() < 2)
                return out;
            out.hold       = plan.route;
            out.pts        = plan.route.get();
            out.isAuto     = true;
            out.generation = plan.generation;
            out.blocked    = plan.blocked;
            out.objective  = std::move(plan.objective);
            return out;
        }

        // Playback cursors are indices into the polyline, so they are meaningless once
        // the polyline is replaced. Drop them whenever the route generation moves.
        static std::mutex                          g_routeGenMutex;
        static std::unordered_map<uint32, uint32>  g_botRouteGen;

        static bool RouteGenerationChanged(uint32 botGuidLow, uint32 generation)
        {
            std::lock_guard<std::mutex> lock(g_routeGenMutex);
            uint32& seen = g_botRouteGen[botGuidLow];
            if (seen == generation)
                return false;
            seen = generation;
            return true;
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

    // Belt-and-braces self-heal, called on LOGIN. If a player carries the ghost-mode
    // visual aura (37800, the GM "Transparency" Player::SetGMVisible applies) but is
    // NOT recording — a logout/crash/disconnect mid-record, or a save that persisted
    // the aura before the logout cleanup ran — they're stuck permanently transparent
    // (Kevin). Strip it and fully reset ghost state. Gated on the aura, so a normal
    // player is never touched. (Recording is in-memory and cleared on logout, so at
    // login no one is legitimately recording.)
    static constexpr uint32 GHOST_VISUAL_AURA = 37800;   // Player::SetGMVisible's aura
    void ClearStuckGhostMode(Player* player)
    {
        if (!player || !player->HasAura(GHOST_VISUAL_AURA)) return;
        ApplyGhostMode(player, false);                 // SetGMVisible(true) removes 37800 + resets speed/fly
        player->RemoveAurasDueToSpell(GHOST_VISUAL_AURA);   // explicit, in case GM state differed
        LOG_INFO("module",
            "[WowPsParty Path] cleared stuck ghost-mode aura (Transparency) on login for guid={}",
            player->GetGUID().GetCounter());
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

    // Cheap enough for the 1 Hz follow ticker: cached boss list + the instance's
    // kill flags. It must NEVER build a route — that belongs to the tank's own tick.
    static bool AutoRouteHasWork(uint32 mapId, Player* leader)
    {
        Map* map = leader ? leader->GetMap() : nullptr;
        if (!map || !map->IsDungeon())
            return false;
        std::vector<BossObjective> const& bosses = EnsureBossObjectives(mapId, map->GetSpawnMode());
        if (bosses.empty())
            return false;

        std::lock_guard<std::mutex> lock(g_autoMutex);
        auto it = g_auto.find(map->GetInstanceId());
        if (it == g_auto.end() || it->second.mapId != mapId)
            return true;   // nothing confirmed down yet — everything is still to do
        for (BossObjective const& obj : bosses)
            if (!it->second.cleared.count(obj.entry))
                return true;
        return false;
    }

    bool TankRouteAvailable(uint32 mapId, Player* leader)
    {
        return HasPathForLeader(mapId, leader) || AutoRouteHasWork(mapId, leader);
    }

    std::string DescribeTankRoute(Player* leader)
    {
        if (!leader || !leader->GetMap())
            return {};

        uint32 const mapId = leader->GetMapId();
        if (uint32 const wps = GetPathWaypointCount(mapId))
            return Acore::StringFormat(
                "|cff66ccff[WowPsParty]|r Recorded tank route found for this dungeon "
                "({} waypoints) — the tank will lead the pull path.", wps);

        std::vector<BossObjective> const& bosses =
            EnsureBossObjectives(mapId, leader->GetMap()->GetSpawnMode());
        if (bosses.empty())
            return Acore::StringFormat(
                "|cffffcc00[WowPsParty]|r No recorded route here, and this map has no boss "
                "encounters to auto-route to — the tank will follow you. Use Record Path "
                "to teach it the way.");

        uint32 pending = 0;
        {
            std::lock_guard<std::mutex> lock(g_autoMutex);
            auto it = g_auto.find(leader->GetMap()->GetInstanceId());
            bool const fresh = (it == g_auto.end() || it->second.mapId != mapId);
            for (BossObjective const& obj : bosses)
                if (fresh || !it->second.cleared.count(obj.entry))
                    ++pending;
        }
        if (!pending)
            return Acore::StringFormat(
                "|cff66ccff[WowPsParty]|r Every boss here is already down — the tank will "
                "just follow you out.");

        return Acore::StringFormat(
            "|cff66ccff[WowPsParty]|r No recorded route — the tank will find its own way to "
            "the {} boss(es) still standing, and open what it can along the way.", pending);
    }

    // Diagnostic: the TRUE nearest-waypoint distance to the leader across ALL
    // recorded groups on the map, IGNORING WING_SELECT_MAX_DIST. When the path
    // ticker reports "no recorded path", this tells us whether the leader really
    // is far from the route (a genuine gap) or the route is right there but the
    // 250y wing cap (or a Z/cache issue) wrongly rejected it (a selection bug).
    // Returns -1 if nothing is recorded on the map at all.
    static float NearestWaypointDistDebug(uint32 mapId, Player* leader)
    {
        std::vector<PathGroup> const& groups = EnsureGroups(mapId);
        if (!leader || groups.empty()) return -1.0f;
        float best = std::numeric_limits<float>::max();
        for (PathGroup const& g : groups)
            for (Vec3 const& w : g.pts)
            {
                float const d = Dist3D(leader->GetPositionX(), leader->GetPositionY(),
                                       leader->GetPositionZ(), w.x, w.y, w.z);
                if (d < best) best = d;
            }
        return best;
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
        // A seated bot has no feet of its own — TickBotVehicleMovement owns it, and a
        // MovePoint here would order a turret to drive off its own hull. This matters
        // more now that auto-routing reaches maps nobody recorded, Ulduar included.
        if (bot->GetVehicle()) { tlog("skip: bot is in a vehicle"); return; }

        ObjectGuid const leaderGuid = GetLeaderFor(bot->GetGUID());
        if (!leaderGuid) { tlog("skip: no leader directive"); return; }
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader || !leader->IsInWorld()) { tlog("skip: leader not in world"); return; }
        if (leader->GetMapId() != bot->GetMapId()) { tlog("skip: leader other map"); return; }
        if (!leader->GetMap() || !leader->GetMap()->IsDungeon()) { tlog("skip: leader not in dungeon"); return; }
        if (leader->IsInCombat()) { tlog("skip: leader in combat"); return; }
        if (leader->GetVehicle()) { tlog("skip: leader is in a vehicle"); return; }

        // Per-tank "lead the route" toggle (rotation editor, default ON). When OFF,
        // don't drive a route at all — bail so the lead-tank MoveFollow in PartyFollow
        // leads the party by mirroring the leader instead (the suppression gate there is
        // matched on the same toggle, so it won't yield the feet to this ticker).
        if (!WowPsParty::BotFollowRecordedPath(bot->GetGUID()))
        {
            tlog("skip: follow-path toggle off — leading via MoveFollow");
            return;
        }

        // The recorded route for the WING the leader is actually in (multi-wing dungeons
        // share a map id), else one generated live from the navmesh to the nearest boss
        // still standing. Neither -> don't lead (and never walk another wing's route).
        TankPath const tp = ResolveTankPath(bot, leader);
        if (!tp.pts || tp.pts->size() < 2)
        {
            // Include the TRUE nearest waypoint distance so we can tell a genuine
            // off-route gap from a wing-cap/Z/cache selection bug (Singing Grove).
            tlog(Acore::StringFormat(
                "skip: no route for this wing — nearest recorded waypoint {:.0f}y (cap {:.0f}), "
                "and nothing left to auto-route to",
                NearestWaypointDistDebug(bot->GetMapId(), leader), WING_SELECT_MAX_DIST).c_str());
            return;
        }
        std::vector<Vec3> const& path = *tp.pts;

        // Leash check — HORIZONTAL distance only. A big VERTICAL gap (the leader
        // just dropped down a hole / off a ledge on the recorded route — e.g. the
        // plunge before Anub'arak in Azjol'Nerub) blows past a 3D leash even though
        // the leader is right below us, which used to bail the whole path-follow
        // BEFORE the drop-blink could run, stranding the tank up top until it was
        // dragged with Come Hither. Gating on horizontal distance keeps us leading
        // straight down the recorded drop (the blink logic below carries us across),
        // while still stopping at range if the leader genuinely ran off sideways.
        {
            float const dxL = bot->GetPositionX() - leader->GetPositionX();
            float const dyL = bot->GetPositionY() - leader->GetPositionY();
            if (std::sqrt(dxL * dxL + dyL * dyL) > TANK_LEASH)
            {
                tlog("skip: beyond tank leash (>horiz from leader)");
                return;
            }
        }

        // Past the skip gates, we're committed to leading this tick. Tell the
        // follow ticker to keep its hands off (no MoveFollow re-assert, no
        // stuck catch-up teleport) — otherwise, when the leader stops and the
        // tank idles at its lookahead, the ticker reads the constant distance
        // as "stuck" and teleports the tank onto the leader. Refreshed every
        // tick; expires shortly after we stop leading (combat / beyond leash).
        WowPsParty::MarkTankLeading(bot->GetGUID(), 2500);

        uint32 const botGuidLow = bot->GetGUID().GetCounter();

        // A regenerated auto route is a different array, so every index we cached
        // against the old one is meaningless. Drop the cursors and force a fresh steer.
        if (RouteGenerationChanged(botGuidLow, tp.generation))
        {
            std::lock_guard<std::mutex> lock(g_tankProgressMutex);
            g_leaderCursor.erase(botGuidLow);
            g_tankCursor[botGuidLow] = uint32(path.size());   // sentinel: re-issue the steer
        }

        // The corridor to the objective stops short — something is physically shut
        // between here and the boss. Once the tank has walked as far as the route goes,
        // look for the lever/gong/altar that opens it.
        if (tp.isAuto && tp.blocked)
        {
            Vec3 const& routeEnd = path.back();
            if (Dist3D(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                       routeEnd.x, routeEnd.y, routeEnd.z) <= GATE_ARRIVE)
            {
                std::string const pulled = PullNearestGate(bot, bot->GetMap()->GetInstanceId());
                tlog(pulled.empty()
                        ? Acore::StringFormat("blocked on the way to {} — no gate to pull here",
                                              tp.objective).c_str()
                        : Acore::StringFormat("blocked on the way to {} — pulled '{}'",
                                              tp.objective, pulled).c_str());
            }
        }

        // Per-tank lead distance (yds) — how far ahead ALONG the path we aim.
        // Sourced from the rotation-editor slider (party_loadout.lead_distance),
        // default 10. Shadows the former file-scope 30y constant.
        float const LEAD_DISTANCE = float(WowPsParty::BotLeadDistance(bot->GetGUID()));

        // Find the leader's nearest waypoint (the "cursor") — FORWARD-ONLY and
        // WINDOWED. Scan from where the leader was last tick onward so the cursor
        // can only advance (no ping-ponging where the route doubles back). The
        // window caps how far AHEAD (by path length) a single scan may reach: a
        // route that doubles back on the SAME physical ground (walk UP a ramp to a
        // boss, then back DOWN the identical ramp) puts a far-ahead post-boss
        // waypoint at the leader's exact feet, and an unbounded nearest-scan would
        // jump the cursor to it on position jitter — the tank then skipped the
        // whole boss and walked off toward the next one (Kevin: Anomalus in the
        // Nexus). With the window the cursor advances contiguously; only a genuine
        // long jump (teleport / wing change / the recorded drop) falls through to
        // the unbounded resync below. The window (>> any single-tick leader move)
        // never blocks normal walking.
        auto nearestFrom = [&](uint32 from, uint32& outIdx, float window) -> float
        {
            uint32 const start = from < path.size() ? from : 0;
            outIdx = start;
            float best = std::numeric_limits<float>::max();
            float acc  = 0.0f;
            for (uint32 i = start; i < path.size(); ++i)
            {
                if (i > start)
                {
                    acc += Dist3D(path[i - 1].x, path[i - 1].y, path[i - 1].z,
                                  path[i].x, path[i].y, path[i].z);
                    if (window > 0.0f && acc > window) break;
                }
                float const d = Dist3D(leader->GetPositionX(), leader->GetPositionY(),
                                       leader->GetPositionZ(), path[i].x, path[i].y, path[i].z);
                if (d < best) { best = d; outIdx = i; }
            }
            return best;
        };
        uint32 scanStart = 0;
        {
            std::lock_guard<std::mutex> lock(g_tankProgressMutex);
            auto it = g_leaderCursor.find(bot->GetGUID().GetCounter());
            if (it != g_leaderCursor.end() && it->second < path.size())
                scanStart = it->second;
        }
        uint32 nearestIdx = scanStart;
        float  nearestD   = nearestFrom(scanStart, nearestIdx, CURSOR_ADVANCE_WINDOW);
        if (nearestD > LEAD_DISTANCE)            // lost the leader ahead of the cursor — resync
        {
            // FORWARD-FIRST resync. The leader is past the cursor's window — usually
            // because the cursor froze during a boss fight and the leader then walked
            // on. Re-acquire by the EARLIEST waypoint AHEAD of the cursor the leader
            // is essentially standing on (≤10y), which follows the recorded SEQUENCE
            // and therefore can't skip a leg. CRUCIAL in a HUB dungeon (Halls of
            // Stone: every wing returns through the centre, so the centre waypoints of
            // later passes sit on top of earlier ones — an unbounded global re-scan
            // leapt the cursor to a later pass and skipped Hall of Repose + Tribunal
            // of Ages). Only if no forward waypoint is near (genuine relocation —
            // teleport / wipe corpse-run / wing change) fall back to the unbounded
            // re-scan from 0.
            constexpr float REACQUIRE_RADIUS = 10.0f;
            uint32 fwdIdx = uint32(path.size());
            for (uint32 i = scanStart; i < path.size(); ++i)
            {
                float const d = Dist3D(leader->GetPositionX(), leader->GetPositionY(),
                                       leader->GetPositionZ(), path[i].x, path[i].y, path[i].z);
                if (d <= REACQUIRE_RADIUS) { fwdIdx = i; break; }   // earliest = no leg skip
            }
            if (fwdIdx < path.size())
            {
                nearestIdx = fwdIdx;
                nearestD   = Dist3D(leader->GetPositionX(), leader->GetPositionY(),
                                    leader->GetPositionZ(), path[fwdIdx].x, path[fwdIdx].y, path[fwdIdx].z);
            }
            else
            {
                uint32 gIdx = 0;
                float const gD = nearestFrom(0, gIdx, 0.0f);   // unbounded re-scan (relocation)
                if (gD + 1.0f < nearestD) { nearestIdx = gIdx; nearestD = gD; }
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_tankProgressMutex);
            g_leaderCursor[bot->GetGUID().GetCounter()] = nearestIdx;
        }

        // Discrete lookahead INDEX — still walked FORWARD by path-length from the cursor,
        // used by the end-of-route check and the geometry/blink logic below (those reason
        // about recorded nodes). Clamped to the path end.
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

        // Tank's own nearest recorded node — its position ON the route. Used by the steer,
        // halt, stall-blink and cliff-blink logic below. WINDOWED around the leader's
        // (hub-safe) cursor rather than a global argmin, so in a hub dungeon it can't snap
        // to a later pass stacked on the current one and skip a wing (see TANK_NEAREST_*).
        uint32 tankNearest = nearestIdx;
        {
            // Backward bound: step back from the leader cursor by TANK_NEAREST_BACK yards.
            uint32 lo = nearestIdx;
            float back = 0.0f;
            while (lo > 0 && back < TANK_NEAREST_BACK)
            {
                back += Dist3D(path[lo].x, path[lo].y, path[lo].z,
                               path[lo - 1].x, path[lo - 1].y, path[lo - 1].z);
                --lo;
            }
            // Forward bound: LEAD_DISTANCE (where a leading tank sits) plus slack, measured
            // as path length AHEAD of the cursor. Nodes between lo and the cursor accumulate
            // nothing, so the whole [lo, cursor] stretch is always searched.
            float const fwdBudget = LEAD_DISTANCE + TANK_NEAREST_FWD_SLACK;
            float  tankNearD = std::numeric_limits<float>::max();
            float  fwd = 0.0f;
            for (uint32 i = lo; i < path.size(); ++i)
            {
                if (i > nearestIdx)
                {
                    fwd += Dist3D(path[i - 1].x, path[i - 1].y, path[i - 1].z,
                                  path[i].x, path[i].y, path[i].z);
                    if (fwd > fwdBudget) break;
                }
                float const d = Dist3D(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                       path[i].x, path[i].y, path[i].z);
                if (d < tankNearD) { tankNearD = d; tankNearest = i; }
            }
        }

        // STEER NODE (Kevin's model): TRACE the recorded line — always head toward a node AHEAD
        // of the tank, even when it's beyond the lead distance. Walk forward from the tank's own
        // nearest node by ~LEAD_DISTANCE of path length to pick it: the recorded nodes are only
        // ~2y apart (closer than WAYPOINT_REACHED), so steering at the LITERAL next node would
        // read as "already reached" every tick and freeze her — a lookahead gives the spline real
        // runway and lets her flow node-to-node. The lead is NOT enforced by where we aim (that's
        // what caused the snappy chase of a moving lead point); it's enforced by the HALT below.
        uint32 navIdx = tankNearest;
        {
            float acc = 0.0f;
            while (navIdx + 1 < path.size() && acc < LEAD_DISTANCE)
            {
                acc += Dist3D(path[navIdx].x, path[navIdx].y, path[navIdx].z,
                              path[navIdx + 1].x, path[navIdx + 1].y, path[navIdx + 1].z);
                ++navIdx;
            }
        }
        Vec3 const& wp = path[navIdx];   // the node we steer toward (also the blink/stall target)

        // Leader walked PAST the recorded route end into an un-recorded side area — follow the
        // leader directly until the route resumes (else the tank idles at the end and is dragged).
        if (tankNearest + 1 >= path.size())
        {
            float const leaderToEnd = Dist3D(leader->GetPositionX(), leader->GetPositionY(),
                                             leader->GetPositionZ(), wp.x, wp.y, wp.z);
            if (leaderToEnd > LEAD_DISTANCE)
            {
                tlog("leader past route end (off-route area) — following leader directly");
                if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                    bot->GetMotionMaster()->MoveFollow(leader, 8.0f, 0.0f);
                return;
            }
        }

        // HALT gate. Only when the tank is genuinely AHEAD on the route (its cursor past the
        // leader's) AND its straight-line gap to the leader has reached the lead distance: STOP
        // and wait — never re-issue, never walk back. Resume toward the SAME steer node once the
        // leader closes the gap (hysteresis avoids walk/halt flicker at the edge). On halt we
        // also drop the active steer (sentinel) so the resume re-issues a fresh MovePoint.
        {
            bool const ahead = tankNearest > nearestIdx;
            float const gap  = Dist3D(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                      leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
            std::lock_guard<std::mutex> lock(g_tankProgressMutex);
            bool& halted = g_tankHalted[botGuidLow];
            if (ahead && gap >= float(LEAD_DISTANCE))
                halted = true;
            else if (!ahead || gap <= float(LEAD_DISTANCE) - LEAD_HYSTERESIS)
                halted = false;
            if (halted)
            {
                if (bot->isMoving()) bot->StopMoving();
                g_tankCursor[botGuidLow] = uint32(path.size());   // sentinel: re-issue on resume
                tlog("halt: leading by >= lead distance — waiting for the leader to close");
                return;
            }
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
                    // If the navmesh can still WALK to the blink target by a direct
                    // route, the tank isn't truly wedged — a transient block or a
                    // MovePoint that didn't take. Re-kick the walk instead of
                    // teleporting; only a genuine no-path wedge falls through to the
                    // blink below. This is the main "stop teleporting when it could
                    // just walk there" fix for the long-stride / sparse-waypoint case.
                    float const blinkStraight =
                        Dist3D(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                               path[blinkIdx].x, path[blinkIdx].y, path[blinkIdx].z);
                    if (NavWalkable(bot, path[blinkIdx].x, path[blinkIdx].y, path[blinkIdx].z,
                                    blinkStraight))
                    {
                        tlog("stall but path is walkable — re-issuing MovePoint, not blinking");
                        bot->GetMotionMaster()->Clear();
                        bot->GetMotionMaster()->MovePoint(0, wp.x, wp.y, wp.z);
                        return;
                    }
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
                        // A shut door the party is meant to OPEN looks identical to one a
                        // boss kill opens. Try the lever; if there isn't one, we wait, which
                        // is the old behaviour.
                        std::string const pulled = PullNearestGate(bot, bot->GetMap()->GetInstanceId());
                        tlog(pulled.empty()
                                ? "wedged but no LoS to blink target (closed door / wall) — waiting"
                                : Acore::StringFormat(
                                      "wedged with no LoS — pulled '{}' to try to open the way",
                                      pulled).c_str());
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
                // Even a steep recorded stride gets WALKED if the navmesh has a
                // direct path (a sparsely-recorded ramp can look near-vertical by
                // the dz:horiz ratio yet be perfectly walkable). Only teleport a
                // genuine plunge the mmap can't connect (a real hole/jump).
                // Measure the straight-line budget from the BOT's own position
                // (where NavWalkable starts its path), not the recorded stride, so
                // the length comparison uses a consistent baseline.
                float const strideDist = Dist3D(bot->GetPositionX(), bot->GetPositionY(),
                                                bot->GetPositionZ(), to.x, to.y, to.z);
                if (!NavWalkable(bot, to.x, to.y, to.z, strideDist))
                {
                    tlog("blink: near-vertical drop on next stride (no walk path), NearTeleport across");
                    bot->NearTeleportTo(to.x, to.y, to.z, to.o);
                    return;
                }
                tlog("steep recorded stride but navmesh can walk it — walking, not blinking");
            }
        }

        // Re-issue MovePoint ONLY when we've reached our current steer node (so the spline runs
        // a whole lookahead between StopMoving's, instead of restarting every tick), or the move
        // generator was lost (combat / blink / a halt that dropped the steer). g_tankCursor holds
        // the active steer node; the halt sets it to the sentinel so a resume re-issues here.
        {
            std::lock_guard<std::mutex> lock(g_tankProgressMutex);
            uint32& steer = g_tankCursor[botGuidLow];
            bool const genPoint =
                bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
            float const distToSteer = (steer < path.size())
                ? Dist3D(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                         path[steer].x, path[steer].y, path[steer].z)
                : std::numeric_limits<float>::max();
            // Still walking toward a valid node that's ahead of us and not yet reached? leave it.
            if (genPoint && steer < path.size() && steer >= tankNearest && distToSteer > WAYPOINT_REACHED)
                return;
            steer = navIdx;
        }

        tlog(Acore::StringFormat(
            "LEAD: MovePoint to steer node — tankNearest={} steer={}/{} leaderCursor={}",
            tankNearest, navIdx, uint32(path.size()) - 1, nearestIdx).c_str());
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(0, wp.x, wp.y, wp.z);
    }
}
