# Naruto: The Broken Bond — Porting Log

Evidence-driven notes for the ReXGlue native recompilation. One section per
meaningful iteration. Do not paste raw log dumps here.

## Link milestone (M3)

Problem:
XUsbcam imports were unresolved at final link.

Evidence:
Generated code calls `__imp__XUsbcamCreate` and six related Vision Camera
imports (`Destroy`, `GetState`, `SetConfig`, `SetView`, `SetCaptureMode`,
`ReadFrame`). The installed ReXGlue 0.10 runtime does not provide them:
`xboxkrnl_usbcam.cpp` exists in SDK source but is commented out of the kernel
CMake list, and this project links the installed package rather than an
in-tree SDK.

Root cause:
Broken Bond imports Xbox Vision Camera APIs that are not currently compiled
into the ReXGlue runtime. Gameplay does not require a camera.

Files changed:
- `src/compat/xusbcam.cpp`
- `CMakeLists.txt` (added the compat TU; stage `xenos` GPU plugin)
- `docs/PORTING_LOG.md`

Fix:
Project-level `REX_EXPORT(__imp__XUsbcam*, …)` compatibility layer representing
an unavailable camera:

- `XUsbcamCreate` returns success and writes a null handle (subsystem init,
  not a real device). Matches Xenia's Create-success requirement.
- `XUsbcamGetState` returns 0 (disconnected). Caller only captures if state==2.
- `XUsbcamDestroy` returns success (nothing to tear down).
- Config / view / capture / read return `X_ERROR_DEVICE_NOT_CONNECTED` so the
  game does not arm capture or consume fabricated frames. Return codes are
  Win32 DWORD (callers compare against 997 = `ERROR_IO_PENDING`).

Did not use `REX_EXPORT_STUB` because it leaves `r3` undefined and those
callers inspect the result.

Result:
Executable linked successfully (`broken_bond.exe`). Incremental RelWithDebInfo
build compiled `src/compat/xusbcam.cpp` and linked without unresolved XUsbcam
symbols.

Next:
First runtime launch with `--game_data_root` and `--gpu_plugin=xenos`.

## Runtime start (M4-M6)

Problem:
Process started, ReXGlue initialized, XEX loaded, then guest execution hit
an unregistered indirect call.

Evidence (`logs/run-001.log`):
- GPU plugin `xenos` loaded (`rexgpu-xenosrd.dll`)
- D3D12 adapter NVIDIA GeForce RTX 4070
- VFS mounted game data as `game:` / `d:`
- 32694 recompiled functions registered
- `game:\default.xex` loaded (title 55530825)
- `[FATAL] Call to invalid or unregistered function at guest address 0x821FE040`

Root cause:
`0x821FE040` is not a generated function. Neighbors are vtable-dispatch
thunks that end/start around this address:

- `sub_821FE028` ends with `bctr` at `0x821FE040`
- `sub_821FE060` starts at `0x821FE060`

The 32-byte hole was never registered. Indirect dispatch indexes the
function table by exact address, so a vtable slot pointing at
`0x821FE040` traps.

Files changed:
- `broken_bond_manifest.toml` (`[entrypoint.functions] 0x821FE040 = {}`)

Fix:
Declare the missing entry so codegen discovery can emit it. Size/end are
not specified; analysis should stop at the thunk terminator.

Result:
Codegen emitted `sub_821FE040` as an 8-instruction vtable thunk
(`lwz` this from r7, shuffle args, `lwz` vtable[2], `bctr`). Rebuild linked.

Relaunch (`logs/run-002.log`) then failed at the next missing entry.

## Missing function entries (M7)

Problem:
Guest indirect calls keep trapping on exact addresses that were never
registered as function starts. GapFill does not split `bctr` thunks, so
holes between discovered functions stay unmapped.

Evidence:
- run-001: `0x821FE040` (hole between `sub_821FE028` and `sub_821FE060`)
- run-002: `0x822086E0` (24-byte hole after `sub_822085F0` ends at `0x822086E0`)
- run-003: `0x821C9DE8` (inside 32-byte hole `0x821C9DE0-0x821C9E00` after
  this-adjustor `sub_821C9DD8`)

Each CONFIG entry codegen'd into a real thunk/helper, not garbage.

Fix:
Keep adding `[entrypoint.functions]` entries for the exact trapped address.
Also register the containing hole start when it differs.

A bulk attempt to register ~360 estimated holes produced empty stubs
("not in any code region") and new unresolved branches. Reverted to
runtime-hit addresses only.

Evidence (continued):
- run-004: `0x821CD298`
- run-005: `0x821D0D18`
- run-007: `0x823104A8`
- run-008: `0x822E4648`, then `0x82310580`

Files changed:
- `broken_bond_manifest.toml` (`[entrypoint.functions]`)

## Guest + GPU (M7-M11)

Evidence (`logs/run-006.log` onward):
- Guest reached title init: `XamNotifyCreateListener`, `SetInterruptCallback`
- XAudio client registered and `SubmitFrame` running
- Xenos created render targets (1280x512 4xMSAA, pipelines compiling)
- `NtCreateFile('\\Device\\Image')` fails with `STATUS_NO_SUCH_FILE` (expected
  without a DVD image device; not the crash)
- `BaseHeap::Release failed because address is not a region start` (logged,
  not the fatal)

Result:
Guest entry, audio, and first GPU submissions are happening. Remaining
fatals are still missing function-table entries, not GPU/kernel API gaps.

## Keyboard past the menu

Evidence (`logs/run-mnk-001.log`):
- `--mnk_mode` launched; guest reached the title menu with GPU pipelines compiling
- First action past idle menu trapped at `0x82216E40` (hole between `sub_82216DE0`
  and `sub_82216E68`)

New Game (`logs/run-mnk-002.log`):
- `XamContentCreateEnumerator` (save slots, 0 items) then fatal at `0x822DC6A8`
- Hole between vtable thunk `sub_822DC698` (`bctr`) and `sub_822DC6C0`

Fix:
`0x82216E40` and `0x822DC6A8` in `[entrypoint.functions]`. Persist
`mnk_mode = true` in `out/build/win-amd64-relwithdebinfo/broken_bond.toml`.

Result:
Pending relaunch to continue New Game.

## Engine intro black frames + save location

Evidence (`logs/run-mnk-004.log`):
- Story and fighting both run. Audio keeps going during 2-3s black flashes.
- `async_shader_compilation` (ReXGlue default true) skips draws while a D3D12
  PSO is still compiling. Pipeline-create bursts match the hitch:
  127 PSOs at 21:06:53-54 (engine cutscene after New Game), 53 at 21:08:36
  (later scene). GPU thread presents a cleared frame; XAudio is independent.
- HDD is not missing. Dummy device id 1 is connected. `XamShowDeviceSelectorUI`
  auto-picks it (no Xbox storage blade UI). Saves already exist:
  `C:\Users\Admin\Documents\broken_bond\B13EBABEBABEBABE\55530825\00000001\`
  (`Autosave.ninjasave`, `PlayerSave1.ninjasave`). Enumerator goes from 0 items
  (New Game) to 2 items after create. Later DeviceSelector spam is the title
  retrying the blade, not a disconnected disk.

Fix:
- Force `async_shader_compilation = false` in `OnPostSetup` (after the GPU
  plugin registers the cvar) and in `broken_bond.toml` so cutscenes wait for
  pipelines instead of skipping to black. First visit to a scene can still
  hitch ~2s on last frame; the shader cache under the user-data folder
  shrinks that on replay.
- Log the HDD/content root and existing `.ninjasave` packages at `OnPostSetup`.
  Saves live in ReXGlue user data, not next to the game dump.

Did not change ReXGlue: dummy HDD name, headless device selector, and
`XamContentGetDefaultDevice` stub are generic runtime behavior. This title
does not import GetDefaultDevice; GetDeviceData(type==1) already accepts HDD.

## Gameplay black except pause / jutsu-charge UI

Evidence (user + `logs/run-mnk-005.log` session from 21:42):
- 3D world goes black while audio and simulation continue.
- Pause (Start) and holding the jutsu-seal charge button restore the image;
  releasing / closing UI returns to black. Camera angle also matters.
- That is not PSO compile: it toggles instantly with UI, and UI/3D share the
  same present path. Matches host occlusion/vis-query culling world draws
  (UI passes are not conditional) and/or invalid vertex fetch dropping meshes.

Tried (`logs/run-mnk-007.log`):
- Flags actually applied (`Disabled occlusion_query_enable`,
  `Enabled gpu_allow_invalid_fetch_constants`).
- User: still black after combo-tutorial stages; pause / jutsu UI still restore
  the world. Occlusion / invalid fetch ruled out.

Next:
- NVIDIA was on host RTV (`render_target_path_d3d12` empty → RTV/DSV). Combo
  stages allocate extra EDRAM surfaces (`280x2344`, `160x4096`, `640x4096`)
  that likely alias the 1280x512 4xMSAA scene. RTV drops overlapping tiles;
  UI/pause may sample a copy or skip that pass.
- `render_target_path_d3d12=rov` via `launch.bat` / toml (kInitOnly, cannot
  set in OnPostSetup). First ROV run recompiles PSOs; window title should
  show `Direct3D 12 - ROV`.

Result (`logs/run-mnk-008.log` + user):
- Black screens gone. Startup hitch is ROV PSO compile; cache fills after.
- Idle-black / combat-visible was the same EDRAM aliasing (idle post vs VFX).
- Orochimaru Hokage-summon hang: pause still works, Space/A does not skip.
  Switching pause-menu voice language JP↔EN unstuck the scene. Confirmed
  XMA EOS wait (`game-compatibility#416`), not occlusion and not skip input.

Next:
- Keep ROV. Stop forcing `occlusion_query_enable=false` on next relink.
- Expect more JP↔EN toggles on later lines until XMA EOS is fixed in ReXGlue
  (Canary `use_new_decoder` is not in this SDK).

## Cutscene hang on voice EOS

Evidence: Orochimaru Hokage-summon waits forever; BGM and portal VFX continue;
pause works; Space/A does not skip; JP↔EN voice toggle unsticks. Same as
Xenia `game-compatibility#416`.

Root cause (ReXGlue XMA, not title script): last frame of a packet with no
next packet set `error_status=4` and returned without `SwapInputBuffer`, so
`input_buffer_*_valid` stayed set. The title waits for voice-done
(`XMAIsInputBufferValid` / EOS), not a decoder error.

Fix (SDK `xma_context.cpp`, Canary new-decoder EOS):
- Missing next packet on last-frame split → `SwapInputBuffer`, log a warn.
- Split-header missing next packet already swapped; now also logs.
- `kMaxFrameLength` (0x7FFF) still `error_status=4`.

Rebuild `rexruntimerd.dll` RelWithDebInfo and copy into the title build dir.
Replay the same cutscene without changing language; look for
`treating as EOS` in the log. JP↔EN remains the fallback if a line still
sticks (`kMaxFrameLength` path).

Follow-up (`logs/run-mnk-010.log`): the EOS swap was wrong. It fired on every
2KB ping-pong gap (`packet 0` needs `packet 1`, one buffer still valid).
Voices disappeared; BGM (XMP) kept playing; every dialogue hung. Canary new
decoder still uses `error_status=4` on that site.

Next SDK attempt: starve the kick (no swap, no error 4) so the next buffer
can arrive; `Work()` breaks on `input_starved_`.

User (`run-mnk-011`): **English voices never hang.** Japanese hangs at the
Hokage-coffin line. Same as Xenia #416 (JP XMA / dialogue wait). Starve-wait
spins on the same `read=` with the other buffer never submitted — JP last
frames span a packet that is not coming. EN last frames fit, so the existing
end-of-buffer swap runs.

## Delayed EOS (starve N then swap)

SDK `xma_context.cpp` / `context.h`: keep starve-wait for ping-pong, then
after `kStarvedEosKicks` (16) consecutive `Work()` hits at the same
`read_offset` with no next packet, `SwapInputBuffer` **without**
`error_status=4`. Wait log is debug; one warn `starved EOS` on the swap.
Canary still sets error 4 here — do not copy that. Immediate swap
(`1b16613`) and forever-starve (`60cfb2c`) both failed this title.

Rebuild RelWithDebInfo `rexruntimerd.dll` and copy into the title build dir
(if the exe is running, `launch.bat` promotes `rexruntimerd.dll.new`).
Replay with `logs/run-mnk-012.log`.

User (`run-mnk-012`): delayed EOS at 16 kicks **broke all voices**, not just
JP. Clips play a fragment then stop. Log shows `starved EOS after 16 kicks`
on eight contexts at once (`packet 0` needs `packet 1`, one buffer valid);
~180ms later the other buffer is valid and we EOS again. Decoder is faster
than the title submit; the swap drops the spanning last frame and chops
every 2KB packet. Raising N would only delay the same chop.

Reverted the timeout swap. Starve-wait only (011 behavior): EN voices work,
JP coffin hang remains. No `error_status=4`. Kick-count EOS is wrong: the
XMA worker / title re-Enable is much faster than audio time, so 16 kicks
is not "wait for ping-pong".

## Drained EOS (root)

Decoder is faster than the title. A spanning last frame with no next packet
is a ping-pong gap until the guest **plays** what we already wrote. Then if
the other buffer is still missing, the stream is done.

SDK: starve while `produced_output_this_input_` is false or the output ring
still has free-space < capacity. When this input has produced samples and
`remaining_subframe_blocks >= output_buffer_block_count` (guest drained),
decode the truncated tail (zero-padded) and use the normal end-of-buffer
`SwapInputBuffer`. Still no `error_status=4`. Wait logs are rate-limited.

Replay `logs/run-mnk-014.log`: JP coffins should advance after the line
finishes (look for `drained EOS`); EN voices must not chop like 012.

User: JP coffins and EN voices both play through. Log has `drained EOS,
truncated tail` (often `kicks=1`) then ping-pong valid bits flip. 012
chopped because it swapped and **returned without decoding**. This path
decodes the remaining bits first, then the normal end-of-buffer swap.
Keep ROV. Refresh `G:\BrokenBond-Test` when sending another build.

