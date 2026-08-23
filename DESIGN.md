# MirageTreesExt — Design

A standalone Syringe DLL that turns the vanilla single-tree Mirage disguise into
a configurable **decoy forest**: any TechnoType, when still, lays down a random
cluster of **real, individually-damageable tree objects** around itself.

Coexists with Phobos / Antares (never Ares — Antares is the toolchain of record).
No Ares dependency anywhere.

## Goals (from the feature spec)

- **More quantity** — spawn N decoy trees, not one (`Mirage.Count=min,max`).
- **Random spread** — scatter them over a radius (`Mirage.Distance=min,max`, cells).
- **Random center / random selection** — each tree is a random pick from the
  disguise pool, placed at a random offset.
- **Per-tree health** — each decoy is a real object with its own HP
  (`Mirage.Health`, "damage before they vanish").
- **All TechnoTypes** — units, infantry, aircraft AND buildings. The vanilla
  system is `UnitClass`-only.

## Why this is bigger than vanilla mirage

Vanilla + Phobos mirage is a **pure render swap** in `UnitClass::UpdateDisguise`
(`0x746A30`): one `UnitClass` draws itself as one tree; "killing the tree" just
damages the unit. There are no separate objects and no per-tree health, and it is
units-only.

This feature instead spawns actual `TerrainClass` instances (the same class the
map editor places for trees) around the techno. Each is a genuine world object
with its own `Health`, occupying its own cell, killable independently.

## INI schema (per TechnoType section)

```ini
[MGTK]
CanDisguise=yes            ; vanilla — required
DisguiseWhenStill=yes      ; vanilla — required (the "lay forest when still" trigger)
Mirage.DefaultDisguises=TREE01,TREE02,TREE03,TREE04  ; TerrainTypes only. Falls back to [General]DefaultMirageDisguises.
Mirage.AttackCursorOnDisguise=yes   ; (Phase 2) keep an attack cursor while disguised
Mirage.Distance=0,0        ; min,max spread radius in cells
Mirage.Count=1,1           ; min,max number of decoy trees
Mirage.Health=0            ; per-tree HP; 0 => use the TerrainType's own Strength
```

`CanDisguise` + `DisguiseWhenStill` gate the whole system (`HasMirageTrees()`),
matching vanilla mirage semantics so existing mirage units light up for free.

## Architecture

Mirrors the sibling standalone DLLs (TechnoAttachmentExt, AITriggerTypeExt).

- **Ext storage**: Phobos `Container<T>` in **unordered_map mode** — `Canary`
  defined, `ExtPointerOffset` OMITTED — so we claim NO fixed offset inside the
  game objects and never collide with Phobos/Antares ExtData. Canaries:
  `TechnoTypeExt = 0x14173E37`, `TechnoExt = 0x14173EC7`.
- **`TechnoTypeExt`** (`src/Ext/TechnoType/Body.*`): parses the `Mirage.*` keys;
  `HasMirageTrees()`.
- **`TechnoExt`** (`src/Ext/Techno/Body.*`): per-instance live decoy list
  (`std::vector<TerrainClass*> MirageTrees`), `MirageActive`, `MirageAnchor`.
  `~ExtData` and `InvalidatePointer` keep the list clean.
- **Runtime** (`src/Hooks.MirageTrees.cpp`): `Spawn/Clear/Update` + the driver
  hooks.

No RulesExt needed — `DefaultMirageDisguises` is a vanilla `RulesClass` field.

## Hooks

| Address | What | Register(s) | Notes |
|---|---|---|---|
| `0x711835` / `0x711AE0` | TechnoTypeClass ctor/dtor | ESI / ECX | container alloc/remove |
| `0x7162F0`+`0x716DC0` / `0x716DAC` / `0x717094` | TechnoTypeClass save/load pre + load-suf + save-suf | stack | container stream |
| `0x716123` | TechnoTypeClass LoadFromINI | EBP, [ESP+0x380] | parse `Mirage.*` |
| `0x6F3260` / `0x6F4500` | TechnoClass ctor/dtor | ESI / ECX | container alloc/remove |
| `0x70BF50`+`0x70C250` / `0x70C249` / `0x70C264` | TechnoClass save/load | stack | container stream |
| `0x4DA8A0` | FootClass::Update | ESI | per-frame driver (foot) |
| `0x43FE69` | BuildingClass_AI | ESI | per-frame driver (buildings) |
| `0x71BB90` | TerrainClass ctor (via `GameCreate`) | — | spawn a decoy |

**Hook-encyclopedia notes** (per standing rule — consult before, contribute after):
- `0x43FE69` is a documented **shared chain point**: Phobos `BuildingClass_AI`,
  Antares/Ares `BuildingClass_Update_SensorArray`, all 0xA. Syringe chains
  same-address hooks so co-hooking is safe; our size matches (0xA).
- `0x4DA8A0`, `0x746A30` (mirage), `0x71BB90` (TerrainClass ctor) are **not yet
  in the registry** — add entries once verified in-game.

## Open RE questions (verify in the build + in-game loop)

1. **RE-VERIFY #1 — spawn placement.** Does `GameCreate<TerrainClass>(type, cell)`
   (ctor `0x71BB90`) both construct AND place the tree on the cell + global
   `TerrainClass::Array`, or is a follow-up `Unlimbo(coord, dir)` required? The
   map loader uses `new TerrainClass(type, cell)`, suggesting the ctor places it.
2. **RE-VERIFY #2 — removal.** Is `pTree->Limbo()` + `GameDelete(pTree)` the
   correct full teardown (cell detach + Array removal + free), or is there a
   dedicated `TerrainClass` uninit path to prefer?
3. **"Still" detection.** v1 uses `FootClass::GetCurrentSpeed()==0 &&
   Destination==nullptr`. Confirm this doesn't flicker (spawn/clear thrash) when
   a unit briefly pauses mid-path; may need a settle timer like vanilla.
4. **Occupation / crush.** Real trees occupy cells — confirm they don't trap the
   owning techno or block its own movement when it tries to leave (we clear on
   move, but the anchor cell itself is skipped for placement).
5. **Health / vanish semantics.** Confirm `Mirage.Health` as raw HP matches
   Rex's intent for "damage before they vanish" (vs. a damage counter).

## Phasing

- **Phase 0 (this session)**: scaffold + toolchain + Ext parsing + first-cut
  spawn/despawn runtime. Target: green CI DLL.
- **Phase 1**: in-game verify RE #1/#2, get one vehicle laying/tearing a forest
  correctly. Fix placement/removal.
- **Phase 2**: settle timer, `Mirage.AttackCursorOnDisguise`, building/infantry
  coverage polish, occupation behaviour.
- **Phase 3**: save/load serialization of the live forest (v1 is transient — the
  forest re-lays after load), and encyclopedia write-back for the new hooks.

## Non-goals (for now)

- Serializing live decoy trees across save/load (Phase 3).
- Networked visual disguise of the techno itself (this is a decoy system, not a
  render swap; the techno can additionally use the vanilla/Phobos render mirage).
