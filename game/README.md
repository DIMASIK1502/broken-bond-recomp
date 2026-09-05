# Place extracted Xbox 360 game files here.

This directory is the `game_root` referenced by `broken_bond_manifest.toml`.
ReXGlue maps it to the guest path `game:\` at runtime.

After ripping your disc to an ISO:

1. Extract the ISO contents into this directory (for example with extract-xiso,
   Xbox Image Browser, or 7-Zip for XISO-format images).
2. Ensure the entrypoint executable is at `game/default.xex`
   (the path set in `broken_bond_manifest.toml` → `[entrypoint].file_path`).
3. Keep all other game assets in their original relative layout under this folder.

If your dump already lives elsewhere, you can junction it here on Windows:

```bat
mklink /J game "D:\Xbox360\Naruto Broken Bond"
```

**Do not commit anything else under `game/`.** These are copyrighted assets used
locally for recompilation only.
