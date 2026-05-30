# MythicPlus integration (ALE/Eluna + AIO)

Mythic+ dungeon system (huptiq/MythicPlus) integrated into this AzerothCore +
playerbots fork. It is an **Eluna Lua** module driven by Rochet2's **AIO**
client/server GUI framework — no C++ scripts.

## What was added

| Piece | Where |
|-------|-------|
| ALE (Eluna) Lua engine | `modules/mod-ale/` (azerothcore/mod-eluna; the fork's CMake already special-cases `mod-ale`) |
| AIO framework (server) | `MythicPlus/AIO_Server/` → deployed to `lua_scripts/AIO_Server/` |
| AIO addon (client) | `MythicPlus/AIO_Client/` → deployed to the client `Interface/AddOns/AIO_Client/` |
| MythicPlus Lua | `MythicPlus/lua/` (`Mythic_Server/Client/Locale.lua`) → `lua_scripts/MythicPlus/` |
| SQL | `MythicPlus/Data/SQL/` (3 world + 5 characters tables) |
| Client texture/DBC assets | `MythicPlus/Data/Client/` (raw files for the MPQ patch) |

## Deploy (server)

1. Build the server (the `mod-ale` engine compiles into worldserver).
2. `.\deploy.ps1` — copies AIO + MythicPlus Lua into `bin/.../lua_scripts/`,
   installs the AIO client addon, creates `mod_ale.conf`, adds `Logger.ALE=4`.
3. Apply the SQL **once** to the live DBs:
   - `Data/SQL/world/*.sql` → world DB
   - `Data/SQL/characters/*.sql` → characters DB
4. Restart worldserver. Confirm in `Server.log`: `Executed N Lua scripts` with no
   `Error loading` lines.

## Schema note (already fixed)

`creature_and_keystones.sql` was written for an older creature/item schema. The
keystone-NPC and keystone-item inserts referenced columns this fork dropped
(`scale`, `trainer_type/spell/class/race`, `mechanic_immune_mask`,
`spell_school_immune_mask`, item `StatsCount`). `fix_sql.py` strips unknown
columns (+ their aligned values) against the live schema; the committed SQL is
already filtered. The pedestal NPC (900001 "Font of Power") has `ScriptName=''`
— it's hooked via Eluna `RegisterCreatureGossipEvent`, not a C++ script.

## Client (your step — needs your WoW client + assets)

1. **AIO addon** — `deploy.ps1` already copies `AIO_Client` into your client
   `Interface/AddOns/`. Required for the GUI to receive server-pushed code.
2. **MPQ texture/sound/DBC patch** — build per `Data/Client/README.md`
   (Ladik's MPQ Editor; WDBXEditor for the optional keystone icon DBCs).
   Without it the system still works but custom textures are missing and the
   keystone shows a `?` icon.

## How it works in-game

Custom IDs: pedestal NPC **900001**, keystone item **900100**, vault GO
**900000**. Players insert a keystone at the "Font of Power" inside a Heroic
dungeon to start a timed, affixed, scaled run; rating + weekly vault are tracked
in the `character_mythic_*` tables; loot from `world_mythic_loot` / `world_vault_loot`.

## Bots / henchmen in M+ (your goal — NOT yet validated)

The framework is in and loads cleanly, but running M+ **with party-of-5
heroes/henchmen has not been tested** and likely needs work:
- **Enemy-forces credit**: `MythicEnemyKillCheck` keys off player kill events —
  verify bot kills count toward the required forces (they should, bots are
  Players, but confirm in a live run).
- **Keystone insertion / start**: only the keystone holder interacts the
  pedestal; bots won't. That's fine (you start it), but confirm the run applies
  to the whole party group (bots included) for scaling + completion + loot.
- **Scaling/affixes**: affixes apply to creatures, scaling to the instance —
  bots should inherit it, but Sanguine/Bursting/etc. interacting with our combat
  AI needs a live check.
- **Vault/rating**: per-character; bots won't accrue rating (fine), but make sure
  the leader's run completes/credits with a bot party.

This is the next phase — needs a live client + a real run to validate and tune.
