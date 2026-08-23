# MirageTreesExt

A standalone [Syringe](https://github.com/Ares-Developers/Syringe) DLL for
Yuri's Revenge that upgrades the Mirage disguise into a configurable **decoy
forest** of real, individually-damageable trees — for **any** TechnoType
(units, infantry, aircraft, buildings), not just units.

Coexists with Phobos and Antares (no Ares). Built with the same YRpp + Phobos
toolchain as the sibling `*Ext` DLLs.

## What it does

When a configured techno becomes still, it lays down a random cluster of real
`TerrainClass` trees around itself — random count, random spread, each with its
own health. Move it (or destroy it) and the forest is torn down.

## INI

```ini
[MGTK]
CanDisguise=yes
DisguiseWhenStill=yes
Mirage.DefaultDisguises=TREE01,TREE02,TREE03,TREE04  ; TerrainTypes; falls back to [General]DefaultMirageDisguises
Mirage.AttackCursorOnDisguise=yes
Mirage.Distance=0,0    ; min,max spread radius (cells)
Mirage.Count=1,1       ; min,max decoy tree count
Mirage.Health=0        ; per-tree HP (0 = the TerrainType's own Strength)
```

## Build

CI (GitHub Actions, `windows-latest`, MSBuild) builds `MirageTreesExt.dll`
(DevBuild|x86). Drop the DLL into the RA2/YR game folder alongside your other
Syringe DLLs.

See [DESIGN.md](DESIGN.md) for architecture, hook map, and open RE items.
