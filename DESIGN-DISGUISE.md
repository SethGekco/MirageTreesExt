# MirageTreesExt — Disguise System Design

The goal (Rex): a techno **looks like a tree(s) to enemies** until exposed, while
the owner (and configured allies) see it their own way. Fully customizable:
who sees what, what exposes it, and what gates it.

## Two things `Mirage.Disguise` must do

1. **Hide the techno from enemies** and show a tree in its place.
2. **Expose** on defined conditions (sensor, combat, proximity, …), and **gate**
   on defined conditions (moving, powered, health, designators, …).

## Core mechanism: cloak + disguise-tree (reuse the engine)

Rather than a fragile custom draw-swap, lean on the engine's **cloak** system,
which already provides per-viewer invisibility, sensor/detector reveal, and
reveal-on-fire. `CloakState` (Uncloaked/Cloaking/Cloaked/Uncloaking),
`TechnoClass::Cloak(bool)` / `Uncloak(bool)` (virtuals @ vtable), `Cloakable`.

- **Disguise active** → `Cloak()` the techno + place a **disguise-tree** on its
  cell / footprint.
- **Enemy** sees: techno cloaked (invisible) + the tree → a tree.
- **Owner/allies** see: the techno (they see through cloak) + the tree rendered
  per the chosen **owner-view mode**.
- **Exposed** (sensor/detector/combat) → cloak reveals → enemy sees the real
  techno; hide the disguise-tree from that viewer.

Validation risk (Phase 1): confirm infantry AND buildings cloak cleanly on
command (cloak generators already cloak buildings, so expected OK).

## Config schema (all per-TechnoType, all optional)

```ini
; --- effects (independent, either/both) ---
Mirage.Decoys=no            ; scatter separate decoy trees around it (existing)
Mirage.Cover=no             ; plant trees ON the techno, it still shows (the current
                            ;   "tree on top" option — kept, renamed from Disguise)
Mirage.Disguise=no          ; TRUE disguise: cloak + tree, enemies see a tree

; --- what the owner / allowed houses see while disguised ---
Mirage.Disguise.ViewMode=unit      ; unit | tree | pulse | shimmer   (customizable)
Mirage.Disguise.ViewAudience=owner ; owner | allies | all  (who gets ViewMode vs enemy view)
Mirage.FadeStyle / FadeOpacity / FadePulseRate  ; reused for pulse/tree fade

; --- gating: disguise only ACTIVE while ALL hold ---
Mirage.Disguise.WhileStill=yes           ; drop while moving
Mirage.Disguise.DropOnFire=yes           ; drop when it attacks
Mirage.Disguise.DropOnDamage=yes         ; drop when hit
Mirage.Disguise.RequiredHealth=0         ; only >= this % (or a min/max range)
Mirage.Disguise.PoweredBy=<BuildingType list>   ; only while such a structure exists/powered
Mirage.Disguise.EnemyProximity=0         ; drop if a hostile is within N cells (0=off)
Mirage.Disguise.Designators=<Type list>  ; owner units/buildings that ENABLE it (SW-style)
Mirage.Disguise.Inhibitors=<Type list>   ; enemy units/buildings that BLOCK it (SW-style)

; --- exposure: enemy reveals it via ---
Mirage.Disguise.Detectors=<Type list>    ; sensor/psychic/etc types that expose it
Mirage.Disguise.DetectorProximity=0      ; detector reveal radius
```

(Exact key names to be finalized as phases land.)

## Architecture

- **Condition framework** — a list of predicates evaluated each tick to decide
  `DisguiseActive(techno)`: still, health, powered, proximity, designators,
  inhibitors. Mirrors the SW designator/inhibitor pattern and this project's
  existing gating. One `EvaluateDisguise()` returns active/inactive.
- **Cloak driver** — toggles `Cloak()`/`Uncloak()` on the active/inactive edge.
- **Disguise-tree** — reuse the decoy tree spawn/placement + fade registry, with
  per-observer rendering (enemy: solid; owner: ViewMode) via the existing terrain
  draw hooks (0x71C2BC/0x71C2DC/0x71C309), extended with a full-skip (invisible)
  path for the "unit only" owner view.
- **Detector/exposure** — largely native cloak (sensors). Custom detector types +
  proximity layer on top per-house.

## Phase plan

- **Phase 1 (validate):** `Mirage.Disguise` = cloak the techno + place a tree on
  its cell/footprint while still. Confirm infantry + building cloak works and the
  enemy stops seeing the techno. Owner view = whatever cloak gives (shimmer) + tree
  for now.
- **Phase 2:** per-observer disguise-tree render (enemy solid / owner ViewMode:
  unit-only via draw-skip, or pulse/fade). Hide tree from exposed enemies.
- **Phase 3:** gating conditions — WhileStill, DropOnFire/Damage, RequiredHealth,
  PoweredBy.
- **Phase 4:** SW-style Designators/Inhibitors + EnemyProximity + custom
  Detectors/DetectorProximity.
- **Phase 5:** polish, ViewAudience, docs, encyclopedia write-back.

`Mirage.Cover` (current "tree on top") stays as a separate lightweight option;
its building-art-erasure bug (tree redraw clipping building art) is fixed by also
repainting the building under the covering trees.
