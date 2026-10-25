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

## Guest swap rate and boot movies (release/v0.10.0)

SDK branch for this port is `release/v0.10.0` (`ce7e73e` plus local commits).
An experimental branch had been checked out; the title is rebuilt against
the release runtime.

`patches/sdk/rexglue-sdk-v0.10.0.patch` did not apply: HEAD already had the
forever-starve from `60cfb2c`. The drained-EOS behavior (produce samples,
wait until the guest drains the output ring, then decode the truncated tail
and take the normal end-of-buffer swap) is merged onto that tree. The patch
file is regenerated so it applies on this HEAD.

Guest present is `PM4 XE_SWAP` in `command_processor.cpp`, counted by
`rex::perf::NotePresentedFrame`. The title samples it every 5 seconds
(`src/host/guest_present.cpp`). Boot movie names are refused only when
`skip_boot_logos` is set (`src/host/boot_flags.cpp`, `SetFileOpenHook`).

Evidence, windowed, `resolution_scale=1`, vsync left on, ROV:

- `logs/guest-present-002.log`: `Ubisoft.bik` opened (skip off). After the
  first second, swap is 30 Hz: 150 presents / 5 s, last dt ~33 ms, 24 Hz
  bucket empty. Not a 24 fps Bink clock and not 60.
- `logs/guest-present-003-skip.log`: flag on, `Ubisoft.bik` refused twice,
  no fatal, swap stays ~30 Hz.
- `logs/guest-present-004-after-boot.log`: ~60 s later `AttractMode_US.bik`
  is refused twice. Swap stays 30 Hz through that wait and for ~25 s after.
  That tail was not confirmed as the title menu, world, or a fight.

World and fight intervals are not measured yet.
`logs/guest-present-011-nudge.log` stays at 30 Hz through the Orochimaru
line. Two bursts hit 60 Hz (about 19:29:58 and 19:31:33, a few seconds
each) while XMA was still stitching. The log does not name the screen.
The four `VdSwap` sites return to `821612D4` (`sub_821610F0`),
`821B26A8` (`sub_821B2578`), `821A1750` (`sub_821A15B0`), and
`821B8848` (`sub_821B8740`). `guest swap callers 5s` counts those.
`logs/guest-present-012-callers.log`: the 30 Hz boot and the first 60 Hz
window are both `sub_821610F0` (one call per present). The other three
callers stay at 0 through that window. Inside `821610F0`, offset +500
selects `VdSwap` directly versus `sub_821B2578`; the boot took the direct
side. The player was in a fight by 19:46. From 19:46:29 through 19:46:49
the buckets are ~290–295 frames at 60 Hz and ~300 calls of `821610F0`.
`821B2578`, `821A15B0`, and `821B8740` stay 0 for the whole run. Fight
present is 60 Hz on the same function as the post-boot 60 Hz stretch.

## Orochimaru line frozen, music still playing

Evidence (`logs/guest-present-005-play.log`, English subtitle on screen):
- Guest swap stays 30 Hz. BGM continues. Space does not advance the line.
- `drained EOS` fires at `kicks=1..3` on every spanning packet
  (`valid0` / `valid1` still a ping-pong pair), then XMA logs stop while
  present continues. Same hang as run-mnk-010: the worker declares the
  voice finished before the game submits the next 2KB buffer.

`logs/guest-present-006-voice.log` after a 300 ms wait still shows
`drained EOS` on every packet (`waited_ms=300`, `skip -9 bits`). The other
buffer becomes valid only after `SwapInputBuffer`. Zero-padding that gap
desyncs ffmpeg, so the line never reaches voice-done. Language toggle
resets the context and hides it.

`logs/guest-present-009-no-zeropad.log`: splits stitch, then context 2
hits `split hold released` and XMA goes silent while present stays 30 Hz.
The voice object counts consumed PCM (`+436`, countdown `+276`) and sets
done (`+416`) only after the last buffer is freed. Dropping that last
frame leaves the counter short, so the pose stays up.

`logs/guest-present-010-tail.log`: 943 real stitches, then 71
`split tail emitted` at 800 ms. After each tail ffmpeg logs `skip -9 bits`
or a spectral RLE overflow, and context 0 (the line) stops receiving a
second buffer inside that window. The tail is a false end: stitches resume
about 60 ms later from a dirty codec, and the subtitle stays up.

Do not zero-pad a spanning frame. Keep the saved half until the real second
buffer arrives. If that wait passes 1 s, write silence into the output ring
so the title's 2400-byte refill lead can move, and still do not feed those
zeros to ffmpeg.

`logs/guest-present-011-nudge.log`: the user played the same line on English,
returned to the title menu, started a new game, and played it again on
Japanese. Both advanced with no language toggle. The log has real stitches
and silence nudges, and no `skip -9` / `split tail emitted`.

Graphics imports in `generated/default/broken_bond_init.cpp` are `Vd*`, not
`IDirect3D` / `D3DDevice`. Present is `__imp__VdSwap` at `0x82C0A9BC`.
Also imported: ring-buffer init, command buffer, display mode/gamma, scaler,
`VdRetrainEDRAM` / `VdRetrainEDRAMWorker`. Draw and shader setup are inside
the XEX. No Bink import; movies go through the file open.

The only `VdSwap` call is `sub_821B1DD0`. It takes the guest device in `r3`,
reads `VdGetSystemCommandBuffer`, then swaps. Callers: `sub_821B8740`,
`sub_821A15B0`, `sub_821B2578`, `sub_821610F0`. The title counts which of
those return addresses ran; it does not change the swap. Xenos still draws.

Draws are PM4 `DRAW_INDX_2` (opcode `0x36`), stored with `stwu` into a
walking command pointer. `sub_821A1348` picks `sub_821A0E70` or
`sub_821A0D18`. `sub_821BEB30` calls `sub_821BDFD8`, which emits one of
these packets and then calls `sub_821A1348`. The same opcode is also written
by `sub_821A5498`, `sub_821BD728`, and by `sub_821AD328`. That last one is
the flush: `sub_821610F0` calls it immediately before `VdSwap`.

`sub_821BD3D0` writes `SET_CONSTANT` (`0x2D`). `sub_8257A2A0` and
`sub_8257A480` write `SET_STATE` (`0x25`) and `IM_LOAD` (`0x27`);
`sub_8257A0C8` writes `SET_STATE`. None of those three has a direct call.
`sub_821B6CE0` writes `IM_LOAD` and is called from three sites. No draw,
state, or shader hook is installed.

## Shader database (this dump)

`game/xeshader.sdb` is a big-endian `3BDS` container, version 4.1.
`shader_db` (`tools/shader_db.cpp`, CMake target `shader_db`, not linked into
the game) reads it and writes nothing.

The file header is six big-endian words: magic, version `0x00040001`, source
count, shader count, source offset `0x18`, shader-table offset. Each source is
a 64-byte name, checksum, compressed size, raw size, index, then a zlib blob.
On this dump that section is 85 files (`dx9featuredefinitions.h` and the
`vs*` / `ps*` HLSL). It ends exactly where the shader table starts.

The table is a 32-byte preamble plus 8156 records of 11 big-endian words:
payload offset, compressed size, raw size, a tag, then seven more words. Each
payload is zlib. The dword at offset 48 of the inflated object is the D3D9
version token. This dump inflates to 9788660 bytes: 4648 `vs_3_0`
(`0xFFFE0300`) and 3508 `ps_3_0` (`0xFFFF0300`). The tag word is 1 for 4647
records, 2 for 3508, and 0 for one; both 1 and 2 contain vertex and pixel
shaders, so the stage is the version token.

Each inflated object is big-endian. The first dword is `0x102A1101` for every
`vs_3_0` and `0x102A1100` for every `ps_3_0`. The next two dwords are the byte
offset and size of the second section, and those two sections add up to the
object. Dword 6 is the offset of a descriptor `{u32 offset, u32 bytes}` whose
offset is relative to that second section. On this dump that microcode is
4339980 bytes, every size a multiple of 12. Two control-flow instructions
are packed in each three dwords. The first exec address is the control-flow
length, and `shader_db` checks every slot up to that length: all 8156 shaders
pass.

Offset `0x28` of every object is a big-endian `D3DXSHADER_CONSTANTTABLE`
(header size `0x1C`, version equal to the token at offset 48, target string
`vs_3_0` or `ps_3_0`). Each constant is 20 bytes: a name offset from the
table, a register set, and a register count. This dump has 70214 constants,
61033 float4 (set 2) and 9181 samplers (set 3), and 127 distinct names. No
bool or int constants. `shader_db` checks the table and writes nothing.

The installed SDK build has `REXGLUE_USE_VULKAN=OFF` and `REXGLUE_USE_D3D12=ON`.
`rexgpu-xenosrd.dll` contains the DXBC translator and does not contain the
SPIR-V translator. `shader_db` therefore compiles that translator from the
sibling `rexglue-sdk` checkout (glslang plus the SPIR-V translator sources)
and does not rebuild the game plugin. It translates every shader in memory
with all translator feature bits enabled, 2x MSAA off, and EDRAM interlock
off. The default modification asks for no dynamically addressable registers;
this dump has none. All 8156 modules are SPIR-V 1.5, each instruction stays
inside the module, and each has one entry point of its own stage. None
reported a fatal error. The device-matched in-memory total is 319527540 bytes.

Descriptor sets on that translation, counted from `OpDecorate`: set 0
binding 0 is shared memory on every shader, and the EDRAM buffer is absent.
Set 1 carries system constants, bool/loop constants, and fetch constants on
every shader. Vertex float constants are on 4647 shaders and pixel float
constants on 3491, 8138 in total, with at most 256 registers. Set 2 is used
by 271 vertex shaders, each with exactly two images and one sampler. Set 3
is used by 3444 pixel shaders, up to 16 images and 8 samplers. Three shaders
write depth. No shader writes a memexport element. `DerivativeControl` is on
the 3444 pixel shaders that sample, and `DemoteToHelperInvocationEXT` plus
`SPV_EXT_demote_to_helper_invocation` is on the 36 shaders that kill pixels.
The translation follows the probed device. `SignedZeroInfNanPreserve` and
`RoundingModeRTE` are on every module, and `SampleRateShading` is on every
pixel shader. `DenormFlushToZero` is absent: this GPU does not support it.
The in-memory total for that feature set is 319527540 bytes. The tool
writes no cache.

On this machine `shader_db` also loads `SPIRV-Tools-shared.dll` from
`H:\Tools\VulkanSDK` and `vulkan-1.dll`. The device is an NVIDIA GeForce
RTX 4070, API 1.4.341, driver name NVIDIA. `maxStorageBufferRange` is
4294967295, so one shared-memory binding covers the guest address space.
Float32 signed-zero/inf/nan preserve and round-to-nearest-even are
supported. Float32 denorm flush-to-zero is not, and the translation leaves
that execution mode off. Sample-rate shading, demote-to-helper,
`scalarBlockLayout`, and `uniformBufferStandardLayout` are supported.
`vkCreateShaderModule` accepted all 8156 modules and each module was
destroyed immediately. With dynamic rendering, scalar block layout, and one
pipeline layout (set 0 binding 0 storage buffer, set 1 uniform bindings
0/1/3/4, set 2 two sampled images and one sampler), `vkCreateGraphicsPipelines`
accepted all 4648 vertex shaders paired with an empty fragment stage.
Pixel shaders use a layout of their own: set 0 binding 0 is the shared-memory
storage buffer, set 1 is uniform bindings 0/2/3/4, set 2 is empty, and set 3
is that shader's sampled images followed by its samplers. Nine such layouts
cover the dump. The vertex stage only writes `gl_Position`, because the
default pixel modification declares no interpolators. Color attachments
follow the written render-target mask (`R8G8B8A8_UNORM`), and a depth
attachment is present only when the shader writes depth. With that,
`vkCreateGraphicsPipelines` accepted all 3508 pixel shaders. A draw still
has to pair a vertex shader's interpolator outputs with the pixel shader's
inputs; this probe does not. `spirv-val` for
Vulkan 1.2 with scalar block layout, which the SDK Vulkan device enables
for GPU emulation, passed all 8156.
Without that option the same modules fail because `XeSystemConstants`
member 7, `user_clip_planes`, is at offset 28. The running renderer is
still Xenos.

Xenos tiling for a later host path is `Untile` in
`rex/graphics/pipeline/texture/conversion.h`, with the tile sizes in
`rex/graphics/xenos.h`. `shader_db` compiles those sources and checks them
against `GetTiledOffset2D`. For every format whose block size is a power of
two (61 of 64), both a full rectangle and an unaligned window were untilled
at pitches 32, 64, 96 and 128. All 1366400 block addresses matched, and none
were repeated. The three skipped formats are the two 1-bit formats, whose
block size is zero bytes in the table, and `k_32_32_32_FLOAT`, which is 12
bytes per block and is not tiled. A 64×48 `k_8_8_8_8` image tiled with
`GetTiledOffset2D` and passed through `Untile` came back unchanged.
`CopySwapBlock` matched the four 32-bit endian modes. There is no 3D
`Untile`. `GetTiledOffset3D` was checked on 2457600 blocks (1, 2, 4, 8 and
16 bytes per block, pitches 32/64/96/128, heights 32/64, depth 16). Addresses
inside each volume are unique. The step for 8 slices along Z and for 32 rows
along Y is constant, and a tile corner's X offset matches the formula in the
SDK comment. `GetTiledAddressUpperBound3D` covers every block. 197664 blocks
are stored below the address of their own 32×32×4 corner, by up to 2176
bytes, so that corner is not the first byte of the tile. The span measured
from the corner does not contain 6 tiles; the first is 1 byte per block,
pitch 96, corner (32, 0, 0), highest address 11263 against a span of 8192.
The host renderer starts in `src/gfx/` as the static library `broken_bond_gfx`.
It is not linked into `broken_bond`, so the game still presents through Xenos.
`host_gfx` opens a Vulkan device with scalar block layout, standard uniform
layout, and dynamic rendering when the GPU has them. On this machine that
device is the NVIDIA GeForce RTX 4070, API 1.4.341. The driver accepted one
SPIR-V 1.5 vertex module. A 64×48 `k_8_8_8_8` image tiled with
`GetTiledOffset2D` and untilled by the SDK came back unchanged, and the same
linear bytes copied into a `R8G8B8A8_UNORM` image were read back unchanged.
The same image was then drawn to an offscreen target. The vertex stage writes
one interpolator at location 0, the fragment stage reads it and fetches the
texel, and the rendered bytes matched the source. `shader_db` then ran a batch
of guest pixel shaders from this dump on that device. The 8×8 target starts
magenta, alpha test is disabled, the color scale is 1, and pixel float
constants are (1, 0.5, 0.25, 1). Three of eight textureless shaders wrote
those constants into the color target: two as 255,127,64,255 and one as
255,255,127,255. Of 2565 pixel shaders whose textures are all 2D, the first
32 were drawn with their own image and sampler counts. Every image binding
received one 32×32 RGBA8 texture and every sampler binding was nearest.
Drawing each of those 32 a second time with a different texture changed all
64 pixels for 10 shaders, so those shaders sampled the bound image. Interpolator
0 was then enabled for textureless pixel shaders that read at least one
register. 62 such shaders exist. The first 16 were drawn twice, with the
vertex stage writing (1, 0.25, 0.5, 1) and then (0, 0.25, 0.5, 1) at location
0. Four of them changed all 64 pixels. One copied the interpolator unchanged:
255,64,127,255 became 0,64,127,255. Every one of the 16542 vertex fetches in
this dump has format 0 and stride 0, so the translator loads no words. A probe
patched the 31 textureless shaders that write interpolator 0 and have a single
vfetch indexed by r0.x: the empty fields became float4 (format 38) and a stride
of 4 dwords. System constants then had NDC scale (1, 1, 1) at byte 124,
vertex_index_max 0xFFFFFF at byte 500, and bit 10 set so W is kept. Shared
memory held a covering triangle and fetch constant vf0 pointed at it. The first
16 of those 31 produced fragments: 4 covered all 64 pixels and 12 covered 32.
The real format and stride are not in the shader database, and EDRAM is still
absent from this path.

`GetTiledAddressLowerBound3D` masks Z with `~kTextureTileDepth` (`~4`) rather
than `~(kTextureTileDepth - 1)`. On 300 of 600 tile corners, Z = 4 and
Z = 12, it returns the previous 4 slices. For every block it stayed at or
below that block's address, and for every tile it stayed at or below the
lowest address in the tile. This is still not `src/gfx/`.

