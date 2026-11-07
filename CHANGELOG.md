# Changelog

Newest first. Older history is in `git log`.

## Unreleased — tooltip hook no longer crashes when hovering your own units or trees

**Why:** a 30-player 1000x1000 game crashed with `C0000005 at 00000000`, `ESI =
0000002B` while the mouse rested on the map (2026-11-07). The delayed-tooltip hook
`DisplayClass_GetToolTip_MirageName` sat at 0x4AE668, stealing 8 bytes. Those bytes
cover 0x4AE66C, which `DisplayClass::GetToolTip` jumps to for your own techno and
for any non-techno such as a tree. The jump ran the last byte of the hook's `E9
rel32` as an instruction. That byte depends on where Syringe put the stub, so it
changed per launch: `04` on a normal map (harmless), `07` (`pop es`) and `06`
(`push es`, this crash) on big maps.

**What:** the hook moved to 0x4AE600, stealing 6 (`mov ecx,[esp+0xC]; test
ecx,ecx`), where the hovered object is first loaded. Nothing branches inside it,
the stolen bytes hold no relative operand, and every path to the name passes
through it. It now checks `AbstractFlags::Techno` before treating the object as a
techno, because trees reach it too. Behaviour is unchanged: an enemy's disguised
techno still shows no name.

**Consumer action:** none.
