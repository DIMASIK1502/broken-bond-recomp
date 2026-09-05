# Place extracted Xbox 360 game files here.

This directory is the `game_root` in `broken_bond_manifest.toml`. ReXGlue maps
it to the guest path `game:\` at runtime.

1. Extract your disc/ISO into this folder (extract-xiso, Xbox Image Browser, or
   7-Zip for XISO images).
2. The entrypoint must be `game/default.xex`.
3. Keep textures, audio, and other assets in their original relative layout.

Nothing else in this directory should be committed. These are copyrighted assets
used locally for recompilation only.
