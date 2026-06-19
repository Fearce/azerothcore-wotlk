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

## Client (WowPs HD client)

On WowPs both client pieces are automated by
`D:\WowPs\tools\mpqbuild\make-patch.ps1` — it packs the `Interface` assets into
`Patch-Y.MPQ`, installs that + the `AIO_Client` addon into the HD client, and
emits `D:\WowPs\MythicPlus-ClientPatch.zip` for other players. Re-run it whenever
the textures/sounds or the addon change. See `tools/mpqbuild/README.md` for the
`Patch-Y.MPQ` naming rationale and why the optional keystone-icon DBC step
(`Data/Client/README.md`) is deliberately skipped (it would clobber the HD
client's custom item DBCs; cost is a cosmetic `?` icon on the keystone).

Generic (non-WowPs) setup: `deploy.ps1` copies `AIO_Client` to a client
`Interface/AddOns/`, and you build the MPQ by hand per `Data/Client/README.md`.

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
