# GPU: Adreno, OpenGL ES and the Metal driver

The phone's Adreno draws CoreAnimation's frames through OpenGL ES (Mesa/freedreno) and, as research in progress,
Metal. This page separates what is **finished and verified** (OpenGL ES for the window server) from what is
**work in progress** (Metal). The Metal-to-NIR shader compiler research lives in the userland repository's
`docs/air.md` and `docs/air-report.md`; this page only summarizes what it means for the user.

| Device | GPU | Kernel file | State |
|---|---|---|---|
| redwood | Adreno 642L (A660 family), firmware `a660_*` | `xnu/files/bsd/dev/hk_adreno.c` | OpenGL ES works with `gpu=1`; no per-program page tables, no fence interrupt, no DCVS |
| instantnoodle | Adreno 650 (`kona` variant, chip id `0x06050002`), firmware `a650_*` | same file, `kona` branches | OpenGL ES works; DCVS, hang recovery, per-program page tables, fence IRQ, clock gating, sleep collapse; Metal in progress |

Vendor firmware (`a6xx_gmu`, `sqe`, zap shader) is never in the repository; it is taken from the phone's own
vendor partition (`fw-gpu/`, see [../BUILD.md](../BUILD.md)).

## hakios-metal compared with this driver (2026-10-28)

`../hakios-metal` (C++, 1.1 kLOC) is a Metal command model (queue, command buffers, blits by CP_MEMCPY/CP_MEM_WRITE, .metallib parser) over the same msm-DRM interface of `HKGPU` (its transport
only needs the service named `HKGPU`; upstream calls it `HakiosGPU`). It has no shaders: nothing is drawn through it, and everything on screen is OpenGL ES (Mesa) here. It is not a
replacement for HKGPU (kernel) or for `hkmetal.m` (7.5 kLOC, AIR to ir3 shaders, 263 of 271 system fragment shaders draw); worth taking from it: its fake-GPU test method (a strict packet interpreter run
under the sanitizers), and the measured fact that one CP_MEMCPY of 4 Mi words hung the GPU (we use none). Nothing in it would speed up the 120 Hz UI: the GPU stage of a frame is 5.75 ms of the 8.33 ms budget and it is drawn by Mesa (display.md, "Dropped frames at 120 Hz").

## Kernel side

### Bring-up (`hk_adreno.c`)

`gpu=1` starts the GPU while the system starts and keeps it for the window server (the `HKGPU` service). The
file's steps are how the hardware was found out and they stay for diagnosis; `adreno=N` runs steps 1 to N and
stops. Reads come before writes, and every step says what it is about to touch.

| Step | What |
|---|---|
| 1-3 | reads: GPU clocks in GCC, RPMh supply names, fuses (which GPU/speed bin), GPU clock controller |
| 4-5 | writes: the CX power switch (the part that is not the 3D core: GMU, bus interface, GPU SMMU) and its clocks, checked with the SoC frequency counter |
| 6-9 | reads: GMU registers, GMU's RPMh channel, the GPU's SMMU and its first context |
| 10 | writes two words into GMU memory and reads them back |
| 11-12 | asks TrustZone whether it takes calls from this kernel; has it check the signature of the **zap shader** (header goes first, see below) |
| 13-16 | page tables and an SMMU context for the GMU; GMU firmware loaded and started; connection to RPMh; HFI queues |
| 17 | talks to the GMU: asks for the 3D core's power |
| 18-21 | the 3D core's translation context with ring and microcode (`a6xx_sqe`), zap shader loaded, command processor started, core commanded out of secure mode (skipping the A660-only `CP_CHICKEN_DBG`/`QOS` writes on `kona`) |
| 22 | a picture filled with three colours, shown on the panel |

Rules learned the hard way (full account in [../research/hardware-qualcomm.md](../research/hardware-qualcomm.md)):

- An access the hypervisor does not like ends the run, at once or a quarter of a minute later, often silently.
- **Cold `gpu=1` used to send the phone to CrashDump mode.** TrustZone crashes when asked to give up the zap
  shader (image 13, `PAS_SHUTDOWN`) before it has been handed that image's header in the same boot, and step 19
  asked exactly that first. The header now goes first (`step_zap_load`; commit `3e84e9c`). A cold cycle is cheap:
  `xl reboot`, stock fastboot, `xl enter`.
- A failed GPU run can leave CX stuck on: recover with `fastboot reboot bootloader` (no flashing), then re-enter.
- Steps are re-entrant now only because `block_new()` frees old memory, `gmu_block_count` is reset and the 27 ms clock
  measurements are skipped on resume (found while building sleep collapse).

### The service `HKGPU` (`xnu/files/iokit/Kernel/HKGPU.cpp`)

A program that opens `HKGPU` speaks the interface of Linux's **msm DRM driver**, the one Mesa's freedreno knows:
every request is a user-client call with the request number as selector (user side: `tools/mesa/hackios-drm`,
standing in for libdrm inside Mesa). Implemented: parameters, buffers (create, get iova, map, free), command
submission, wait for fence, plus one request that is not Linux's: a buffer that is an IOSurface's memory
(`surfaceBuffer()`, `kHKSurface` 0x7e). Buffers are wired, zero-filled, write-combined kernel memory entered in the
3D core's SMMU context (`gpu_cached=1` makes them cached). Not implemented: fences and buffers as file
descriptors; interrupts on redwood (waiting there is asking again five times a millisecond).

### OnePlus 8 features

- **Per-program page tables (`gpu_pt`, default on `kona`).** `CP_SMMU_TABLE_UPDATE` switches context bank 0's TTBR0
  per HKGPU client. The first 1 GB is shared (kernel ring/fences, privileged); program virtual addresses start at
  `0x40000000` out of one global list. The a650 `CP_PROTECT` list keeps a program's commands away from registers
  the kernel owns; the SCM aperture (MP 0x1b) is accepted by this TrustZone. Verified on the phone
  (`userland/src/gpu_iso_test.c`, `RC_JOBS=rc.jobs.gpuiso`, passed three times including a protect violation
  followed by recovery; the system shows 0 SMMU faults; 90 Hz cost unchanged at 490 us per submission). A request
  the SMMU refuses is counted against the program (`MSM_PARAM_FAULTS`). Not done: `CPU_PREP` waits for all GPU work
  rather than per buffer; no per-client memory cap; redwood has `gpu_pt` off (no a660 protect list).
- **Hang recovery (`gpu_recover=1`, default on `kona`).** A GPU that stops answering is reset (3D core half) and
  restarted so only the submission that hung is lost. Verified: 3 hangs of 3 recovered (`rc.jobs.metalrecover`).
  A CrashDump is not recoverable.
- **DCVS (`gpu_dcvs=1`).** Every 50 ms the core's busy time decides: above 50% straight to 525 MHz; one level down
  (400, 305) after four periods under 25%; only the three levels the GMU was given at start, at the supply levels
  the phone's device tree names. A change is one HFI message (120-143 us). From `gpu_hot` (85.0 C default; the
  phone's own trip is 95 C) it is held at 305 MHz, by the GPU's own sensors (`board_thermal_gpuss_0/1`; SM8250:
  tsens0 15 and tsens1 8). The GPU spent 75-83% of the time at 305 MHz in the gesture scripts, sensors read 4-5 C
  less, frame rates and late frames unchanged. `gpu_dcvs=0` holds 525 MHz.
- **Touch boost (`gpu_boost=MS`, default 300, 0 off).** A touch puts the GPU at its top level for 300 ms. One run each:
  late frames 5.8% -> 5.0%, p95 17.9 -> 16.7 ms (the same build twice differs by about 0.5 points). Late frames
  are mostly waiting for SpringBoard's commit, not the GPU.
- **Hardware clock gating (`gpu_hwcg=0` turns it off).** Linux's `a650_hwcg[]` (49 registers; `tools/mk-hwcg.py`
  makes `bsd/dev/hk_a650_hwcg.h` from it and Mesa's register names), applied in hw_init as `a6xx_set_hwcg`. Verified
  the GPU draws and frames are the same. **The power saved has not been measured** (no way to read the drain with
  the cable in).
- **Fence interrupt (`gpu_irq=1`, GIC 332).** `RBBM_INT_0` bit 20 from `CACHE_FLUSH_TS`; waits sleep instead of poll.
- **Block collapse in system sleep (`gpu_sleep=1`, default).** `hk_gpu_suspend()`/`hk_gpu_resume()` in
  `hk_adreno.c`, `hk_gpu_sleep_begin()/end()` in `HKGPU.cpp`, called from `hk_power_will_sleep()/did_wake()`.
  Before: CX and GX stayed on in every sleep. Now `tools/t power` shows CX **off** and GX **off** in every sleep
  (10 sleeps in one run) and the wake takes **133 ms** (it was 615). The block is given back as at the end of a
  run (`gpu_block_stop()`: GMU slumber, zap shader and CX half given up; the vendor's `a6xx_gmu_suspend`) and
  steps 1..21 run again on wake (`a6xx_gmu_resume`). The programs' buffers, tables and fences are memory of ours and
  stay; the fence word is put back to the last submission's number. `HKGPU.cpp` holds `gGPULock` from the one
  call to the other so nothing submits and no register of a collapsed block is touched (that would hang the phone).
  A submission not done at the sleep leaves the GPU on for that sleep. `gpu_cycle=N` tries N stop/start cycles at boot.
  **Not done: IFPC** (the GMU collapsing GX by itself between frames while awake), which needs the GMU's power config
  and an OOB handshake around every host register access; the mainline driver does not do it for this GPU either.
- Facts: `glFlush` is 4.6 ms median (submission plus waiting); GMEM versus sysmem rendering changes nothing
  (`FD_MESA_DEBUG=sysmem`: 5.6% against 6.0% late); shader compile and texture upload are in boot frame 1 only.
  The Mesa shader cache is disabled at build (`-Dshader-cache=disabled`).

## User side: OpenGL ES for CoreAnimation (`userland/src/eagl.c`, userland repo)

`libhackios-gl.dylib` puts an OpenGL ES implementation (Mesa 25.0.7 with freedreno on Darwin, `tools/mk-mesa.sh`,
patches in `tools/mesa/`) behind iOS's `EAGLContext` inside backboardd, so the window server's renderer is the
OpenGL ES one. It is loaded only when the kernel started the GPU (`gpu=1`, service `HKGPU` present); otherwise
CoreAnimation uses its own software renderer. Build with `GL=1`. Boot-args: `gl=0` never, `gl=1` also without a GPU
(Mesa software, for working on `eagl.c` in QEMU).

CoreAnimation asks six things of an `EAGLContext` (`-initWithAPI:properties:`, `-API`, `-setParameter:to:`,
`-getMacroContextPrivate` which is a table of functions called at offsets of iOS's own OpenGLES library listed in
`src/eagl_slots.txt` and generated by `mkeagl.py`, `-texImageIOSurface:...`, `-attachImage:toCoreSurface:...`). IOSurfaces
become GPU textures without copying where the driver can share the memory (`hk_gl_surface_texture()`).

Shaders: CoreAnimation draws a layer with one of ninety "functions" (a fragment shader each). Its OpenGL ES renderer
lacks some; `eagl.c` supplies stand-ins (e.g. vibrancy, group-over compositing, a four-sample blur `sb3`) and some
draws are made with no program at all. Findings and the pitfalls are in
[../research/ios-userland.md](../research/ios-userland.md). Diagnostic boot-args (`gltrace*`, `glcensus`, `glshaders`,
`glshow*`, `glskip`, `glplain`, `glwait`, `glcopy`, `glfetch`, `glyes`) are listed in [../BOOT_ARGS.md](../BOOT_ARGS.md).

Verified on the phone: lock screen, home screen, Control Center, Settings, App Library with translucent pods
(grey-pod bug fixed 2026-10-25: the vibrant shader used `mix(behind, s, a)` instead of source-over), notification
banners, scrolling. See [../KNOWN_ISSUES.md](../KNOWN_ISSUES.md) for visual defects still open (the poster
switcher, `sb3` blur refusal, the Control Center glyph opacity workaround).

## Metal (work in progress, instantnoodle only)

**2026-11-07: on by default, whole system** (loader `default_args`; `metal=0` opts out). What it took, each verified on the phone (RAM images 134-158):
- **Maps black but for strips:** not Metal. Maps' drawables were complete (`metal_probe=1` reads them after the fence: 0% black, alpha set); CoreAnimation
  *detached* Maps' full-screen CAMetalLayer to a hardware display layer and composited the rest with a hole (colour 0, blending off: `gltrace_place`). The
  swaps had two layers (`kern.hackios.fb_layers` 0x3) and MiniIOMobileFramebuffer shows one. `CA_DISABLE_DETACHED=1` in backboardd (inject.c; `ca_detach=1`).
- **Maps' colours** (buildings red/green/yellow, labels green): the DaVinci ground shader takes its lighting through `half3` varyings; 16-bit varyings now
  cross the stage boundary as 32-bit (`is_wide_varying`, air_to_nir.c). Varyings are also linked by name now (`link_varyings_by_name`, air_metal.c).
- **Maps' first launch crashed** in fd6_draw_vbos (NULL rasterizer): the driver called the library from several threads with no lock; every call takes
  `rw_pipe_lock` now (recursive). Found with the image list the driver prints at start (`image 0x... /hackios/lib/...`) and `llvm-symbolizer` on the
  unstripped library in the build volume.
- **Photos crashed on launch:** `insertelement` at a run-time index refused (now `nir_vector_insert`); then its grid drew nothing: its uber-shader declares
  textures it binds as nil (legal in Metal) -- zeroed stand-ins now (`stand_in_texture`/`stand_in_buffer`); thumbnails are '420f' surfaces read as R8 + RG8
  planes (kHKSurface answers per plane: `planes` = N+1 in), as format 500 (an RGBA copy of Y, Cb, Cr), or with rows not a multiple of 64 (a copy); ASTC and
  ETC2 textures are served (block-aware strides).
- Also added: blend colour, scissor and viewport rectangles, vertex textures and samplers, `air.pack/unpack.{u,s}norm`.
- Tools: `metal_dump=NAME` + `build/mesa-host/build/src/compiler/air/air2nir` (in the `hk-ws` volume) read any app's shader as NIR; `metal_skip=NAME` finds
  which pipeline paints what; `metal_texlog=1`. The cross build of the library needs `CPATH` to a one-file `endian.h` (the SDK has only machine/endian.h).

**Do not read "Metal runs" as "Metal apps work".** What exists is an experimental driver, off by default.

- `metal=1`: Metal serves test programs. `metal=2 metal_profile=1`: Metal is handed to the whole system (backboardd's
  compositor too); this reaches the home screen. Metal for the whole system is **not** the default; chronod's widgets
  still have two pipelines refused.
- The driver is `userland/src/hkmetal.m` (about 7,500 lines): a `_MTLDevice` subclass declared in source, registered
  via Metal's own `MTLAddDevice` with `MTLDeviceSPI` declared at run time (`class_addProtocol`); it opens `HKGPU`
  the way Mesa's shim does. The IOGPU road was measured shut (`IOServiceOpen` with Apple's type 1 returns
  `kIOReturnUnsupported`). Shader compilation is `libhackiosair.dylib` (AIR bitcode to NIR to ir3), loaded with
  `dlopen` so a device exists even without a GPU.
- Measured working on the phone: blits, clears, 4x MSAA resolve, command-buffer completion, fences, events and
  shared events (within one process), heaps (`metal_sync_test` 10/10); **263 of Apple's 271 system fragment shaders
  draw and the 258 compared agree with softpipe**; 47 compute kernels; command buffers cost one GPU submission and
  one fence (`metal_sync=1` restores a fence per draw: 107 us against 7,086 us for eight full-screen draws);
  Apple Maps runs: its pipelines build and its command buffers complete without error (`RC_JOBS=rc.jobs.maps`,
  offline, **no picture checked**).
- Not implemented (the driver says so once per feature): mesh/object shaders, tessellation, indirect command buffers,
  tile shading and imageblocks, visible/intersection function tables, indirect draws and dispatches, more than one
  viewport, depth clip clamp, vertex amplification, GPU counter sampling, texture access counters, texture views,
  wide lines, per-binding LOD clamps, wrapping caller memory, heaps do not suballocate, shared events across
  processes.
- Third-party Metal apps: none tried. Weather, Photos and Measure never opened a Metal device in the tests.
- Corpus numbers (456 modules in the system `.metallib` files, 2026-10-23): all read by our bitcode reader; 321
  translate and compile through ir3 (every entry point: 271 fragment, 47 compute, 3 vertex); the other 135 are
  `air.visible` library functions (all translate when wrapped); 255 identical to LLVM's execution and drawn; 30
  run on the phone with a finished result. Source of truth and caveats: `docs/air.md`, `docs/air-report.md`
  (generated by `tools/air/corpus.py`). Reaching ir3 means it compiled, not that it is correct.
- A known Metal-related abort: SpringBoard's Siri orb (`SUICOrbView -_updateMetalTextureRasterSize:`) asks Metal for
  something it will not give; `inject_sb.c` prints what Metal answered (`siri_orb=0` skips the prewarm, a
  diagnostic only). Whether it still aborts under `metal=2` is not recorded.
- Why IOSurface drawables matter: `-[CAMetalLayer nextDrawable]` sends a device one message,
  `-newTextureWithDescriptor:iosurface:plane:` (`hkdrawable.m`; research in air.md).

### Performance: Maps (2026-11-07, OnePlus 8 Pro, RAM images 211-228; measured on the phone)

How it was measured (boot-args in [../BOOT_ARGS.md](../BOOT_ARGS.md)): `metal_stats=1` prints per process, every 5 s, the
command buffers, draws and blits, the time `-commit` replays them into Gallium, the time from submission to fence (split into
the completion queue and the fence wait) and the time waited in `-[CAMetalLayer nextDrawable]`; the library adds where a draw's
time goes; backboardd's `eagl.c` adds a "pace" line (frames composited, frames on the panel, swaps not pushed). `gpu_log=1`
gives the kernel's line per submission (`tl: Q` with words and process, `tl: q` with the GPU's own time, `tl: w` with how long a
wait took to see its fence) in the loader's log (`tools/macdev/logpoll.py`); `gpu_stats=1` the GPU's busy share and clock a
second. Kernel lines printed after boot reach the loader's log, not `iphont log --live`.

What limited Maps while panning, and what was done (before -> after):
- **The GPU, not the processor.** Maps' map pass was 29.7-33 ms of GPU time (median) at 525 MHz, the GPU 65-85 % busy, and
  command buffers waited ~23 ms for their fence (frames are three command buffers, the completion queue is serial): ~11-18
  Maps frames a second. The processor's replay was 6-13 % of a core.
- **Tile memory** (`metal_tiler`, default on): DontCare loads and stores discard the attachment (`rwa_texture_discard`,
  freedreno's glInvalidateFramebuffer path), and a pass that resolves without storing renders its samples in tile memory and
  writes each tile out resolved (`rwa_draw.resolve`, EXT_multisampled_render_to_texture): the 4-sample texture is never
  written or read and the resolve blit (1.8 ms of the processor's a frame) is gone. 33 -> 29.7 ms.
- **Samples** (`metal_msaa`, default 2): Maps asks for 4 samples without asking whether they are supported; at 4 the
  Adreno's tile memory holds half the pixels, so the pass is played for twice the tiles (50 000 command words, against 17 000).
  29.7 -> 13.8 ms (median), fence 20 -> 9 ms. `metal_msaa=1` draws Maps black (not investigated).
- **The processor's replay** (`air_metal.c` `rwa_render_draw`): surfaces kept per texture and the framebuffer left bound
  (`metal_fbkeep`; freedreno compared framebuffers by surface pointer, so every draw dirtied all state); state bound only where
  it differs (`device->bound`); the index buffer no longer read to bound the vertices (`metal_strict=1` brings it back; the
  Adreno bounds its fetches itself). Per draw 32 -> 15 us in the driver, 19.5 -> 10 us in the library.
- **Idle power:** with nothing new to show the window server still swapped up to 58 times a second, each a full scaled copy by
  the processors and a push to the panel; the kernel now leaves such swaps unpushed (`fb_same=1`: as before), and GPU
  submissions at rest went from ~72 to 0-40 a second.
- **Still open:** in the busiest second Maps' map passes were 415 ms of GPU time and backboardd's compositing 387 ms (434
  submissions: about twenty per composited frame, ~1.3 ms each, probably one per offscreen pass, each reloading its target into
  tile memory); and `-commit` spends ~0.8 ms submitting since the tile-memory changes. Maps panned at about 21 frames a second
  at best in these runs (it was 11), not 120.
- **Panel-density Metal layers** (`metal_native`, default on since 2026-11-07, the owner's choice): `hkmetal.m` lowers a
  CAMetalLayer's contentsScale by panel / display (3 -> 2.512 here, from `kern.hackios.fb_panel`), so Maps draws 1080 x 2340
  instead of 1290 x 2796 and the window server stretches it as before. Scaling only the drawable was tried first and drew
  Maps black: Maps makes its own multisample and depth targets from its view, and its passes had attachments of two sizes.
  The saving is not measurable across boots: Maps reopens where it was left, and what is on the map changes the GPU's time
  more than the 30 % fewer pixels (11.2 and 11.0 ms against 13.8 and 7.7 ms in four runs).
- **The window server's frame** (trace `gltrace_at=100 gltrace_frames=12`, RAM image 233): a frame with Maps changing is
  ~20 GPU submissions -- CoreAnimation draws the screen behind the translucent bars twice into 1100 x 2730 targets, a
  1100 x 1342 and a 782 x 1342 one for the blur, strips of 1290 x 177 and 1312 x 192, then the 1290 x 2796 frame (30
  draws), and `eagl.c` scales it to the panel (0.6 ms). About 10 million pixels of passes, each a tile pass of its own:
  8-12 ms of GPU a frame, at 30-85 frames a second (a 30 a second rhythm is always there while Maps is open). With
  Maps panning the GPU was 92-98 % busy, about 60 % of it the window server's. It is the backdrop blur of iOS's
  materials, not a fault of the GL path; Reduce Transparency would remove those passes (not tried).
- **What an iPhone does and this port did not (2026-11-07, RAM images 235-251, the owner's push for it):**
  - *Backdrops at a fraction of the screen's size.* Maps' backdrop group (`MapsTheme`) asks for scale 0.25 (seen with
    `hkmetal.m`'s `-[CABackdropLayer setScale:]` probe), but the window server captured the screen behind the bars at almost
    full size. QuartzCore's own `CA_BACKDROP_SCALE` (a debug option of its render server) set to 0.125 in backboardd
    (`inject.c`, `ca_backdrop=0` to leave it out): the window server's frame went from ~10 to ~5 ms of GPU, Maps' pans from
    5-15 to 40-55 frames a second, the bars' blur the same in screenshots.
  - *No cache maintenance for pixels no processor wrote.* `HKGPU.cpp` pushed every IOSurface a submission named out of the
    processor's cache when its seed moved and every 4 ms: 2.3 GB a second with Maps panning. Now only a surface locked for
    writing since (`MiniIOSurface.h` `cpuDirty`); `eagl.c`'s seed-only locks say so (`kHKLockSeedOnly`, 0x40000000) and
    are not made to wait for the GPU either; the display driver's own copies forget stale lines first
    (`MiniIOSurfaceFromGPU`). 0 KB a second since; `gpu_flush_all=1` as before.
  - *Pictures wrapped, not copied.* `surf_wrap` is on by default: refused, the window server made and filled surfaces of its
    own for Maps' pictures every frame (7.5 % of its time). Copies are added up (`surface_changed()`) only for wrapped memory
    (`glsum=all`: all), seldom once still (`glstill` on by default): that had become 55 % of the window server's time.
  - Measured with `hkprof_at=SECONDS hkprof_for=SECONDS` (`inject.c`, the in-process sampler `hkprof.h` on a timer; it now
    also puts samples in system code on their first caller outside it) and `iphont processes` (CPU time by process):
    backboardd was 83 % of a core while Maps panned. Still the largest: Mesa's bookkeeping per submission (buffer tables,
    ringbuffer objects) at ~35 % of backboardd's samples, QuartzCore ~17 %. Rendering the window server without tile memory
    (`glsysmem=1`) was slower.
- **Later the same evening (RAM images 252-257):** Maps' own CPU (`metal_prof_in=Maps metal_prof_at=S metal_prof_for=S`:
  `hkprof.h` in a Metal process) was VectorKit 28-31 %, malloc/objc/kernel calls ~40 %, our Metal stack ~11 %:
  `-[HKMetalWork dealloc]` released all 6 x 32 slots of every draw (now only the filled ones), `check_bindings`
  formatted a sentence for every buffer of every draw (now only for a refusal). Maps never went past ~60 frames a second
  because applications' display links are held to the iPhone X's 60: `hk_hifps.h` (inject_ui.c, hkmetal.m) lifts them as
  SpringBoard's are, but with every application at 120 the GPU was 95 % busy and Maps panned worse (13-36), so it is off
  unless `app_fps=1`. The owner found Maps smooth with the defaults (2026-11-07).
- **Do not boot `metal_sync=1` with Metal on everywhere:** image 222 (with it) never came up and the phone left USB until the
  owner reset it; there was no saved log. `metal_sync_in=NAME` waits per draw in that one process only.

## Verification record

Verified on the phone: cold `gpu=1` (logs `180011`, `180356`), GPU scaling into scanout, hang recovery 3/3, fence IRQ,
page-table isolation x3, DCVS, block collapse in sleep (8+ sleeps incl. 5 rapid and a 15 s powerd sleep), Metal
tests listed above. Not measured: power saved by hwcg or collapse; real-world Metal app rendering.

## Files

`xnu/files/bsd/dev/hk_adreno.c`, `hk_a650_hwcg.h`, `xnu/files/iokit/Kernel/HKGPU.cpp`, `MiniIOSurface.cpp`; userland
repo: `userland/src/eagl.c`, `hkmetal.m`, `hkdrawable.m`, `gpu_iso_test.c`, `metal_*test.m`, `tools/mesa/`, `tools/air/`,
`tools/mk-mesa.sh`, `tools/mk-hwcg.py` (kernel repo).
