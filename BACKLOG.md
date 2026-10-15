# Backlog — the shared work board

## How to use it

- Read this board and CLAUDE.md before work. Kevin's directives rank first; otherwise claim the highest unclaimed Ready item.
- Move a claim to In progress with agent/date, commit it immediately and coordinate in the office.
- Use one bold title and at most one evidence line per item; keep details in documentation.
- Preserve peer items. Record new work in Ready or Icebox immediately and move only your claims.
- Done requires committed, pushed work and named live evidence; builds alone belong in Shipped, awaiting live proof.
- Commit and push board changes with the work; retain the newest 20 Done entries.

## In progress

- **Repair the complete Linux build without precompiled headers** (Implementor c76e32b6, 2026-10-16).
  - Full linking reached three undefined Cell::VisitObjects instantiations in Moonglade, ZulDrak and Nagrand PvP scripts; each lacks CellImpl.h. Preserve all encounter code and verify the final binary link.

- **Block private aura and pet-spell acquisition through GM commands** (Implementor c76e32b6, 2026-10-15).
  - Aura commands call Aura::TryRefreshStackOrCreate directly, and Pet::addSpell has no private policy check; cover direct grants and persistence without changing ordinary spells.

- **Integrate the existing Necromancer and Sorceress with private access and stock-client fallbacks** (Implementor c76e32b6, 2026-10-15).
  - Guarded SRP/source-IP/spoof/boundary, strict startup and shared-count fixtures pass, including auth restart. Raw integer/float/addition/bit field edits now preserve private class identity; native policy checks pass, actual command negatives await the full world. See ../docs/PRIVATE_MODS.md; full deployment/gameplay proof remain.
