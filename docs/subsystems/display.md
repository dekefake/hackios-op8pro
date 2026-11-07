# Display, presentation, touch timing and brightness

How a frame gets from CoreAnimation to the panel, how the panel is powered and dimmed, and how touch reaches
iOS. Written from the source and from measurements on the phones; dated measurements say when they were taken.

Status key used across the documentation: **Verified on phone** means a run on the hardware showed it, **Built**
means it compiles and passes host or QEMU checks but was not seen on the phone, **Unverified** means no
evidence either way. See [../KNOWN_ISSUES.md](../KNOWN_ISSUES.md) for what is broken and
[../TODO.md](../TODO.md) for what is unfinished.

## Components

| Piece | File | Role |
|---|---|---|
| Framebuffer stand-in | `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` | The kernel side of `IOMobileFramebuffer.framework` that backboardd and CoreAnimation talk to: swaps, VSync, power, backlight, refresh-rate properties. The protocol (selectors, swap request layout) was worked out from iOS 16.5's user side. Anything not implemented is logged once. |
| Surfaces | `xnu/files/iokit/Kernel/MiniIOSurface.cpp` | Stand-in for `IOSurface.kext`: wired kernel memory shared between processes as Mach send rights or by number. Not implemented: child surfaces, wrapping client memory, purgeability, compression, notifications. |
| Panel scan (redwood) | `xnu/files/bsd/dev/hk_display.c` | Reads the DSI video panel's frame and line counters so frames land whole in one refresh. |
| Backlight | `xnu/files/iokit/Kernel/MiniAppleARMBacklight.cpp` | The `AppleARMBacklight` service that iOS's brightness code looks for. |
| Power glue | `xnu/files/bsd/dev/hk_power.c` | Screen-off and system-sleep coordination, see [power.md](power.md). |
| User-space hooks | `userland/src/inject.c`, `inject_sb.c`, `eagl.c` | Frame-clock and frame-rate behaviour of the window server, OpenGL ES bridge. Userland repo. |

The display is the framebuffer the firmware left (`FB_BASE` in the device profile). iOS thinks it is an iPhone X
(the identity comes from an iPhone's device tree, `xnuloader/identity.h`); the screen is reported as sRGB, not
P3, because WebKit otherwise draws into ten-bit surfaces that neither this kernel's surfaces nor the window
server's renderer handle (`xnuloader/loader.c`, `gamut_p3`).

## Redwood (POCO X5 Pro): DSI video panel, 120 Hz

The panel is sent the framebuffer over and over by the display controller's timing engine at its own rate
(2432 lines a frame of which 2400 are shown, 119.93 Hz). Without help the copy of a frame into the framebuffer
tears and frames come at a pace of their own. `hk_display.c` reads INTF 1's frame and line counters (two bus
clocks are switched on for that; the timing itself is only read). With `fb_scan=1` the framebuffer driver copies a
frame only where the scan lets it land whole in one refresh, runs at the panel's rate and tells CoreAnimation
that rate, and VSync notifications come when the panel's frames begin. Status: verified on the phone (README
claim "no tearing"); this path is not used on the OnePlus 8.

## OnePlus 8 (instantnoodle): DSI command-mode panel, 90 Hz

All of this is the default on `board=instantnoodle`. Measurements are from 2026-10-23 (RAM-only runs).

### What the firmware leaves

`fb_probe=1` dumps it. A DSI command-mode panel driven by CTL0: VIG0 and VIG1 (the two 540-pixel halves) both
fetch the boot framebuffer at `0x9c000000` (stride 4320). They feed LM0/LM1, then dual DSC, then INTF1. INTF1's
tear check is on and **autorefresh is on** (`0x80000001`): the MDP re-fetches that memory on every TE, which is
why copying into it from the CPU tears. The MDP fetches through apps-SMMU context bank 4. **Reading that
context bank or its page tables sends the phone to Qualcomm CrashDump mode**, and the hypervisor's SMMU has no
ATOS, so a mapping can only be found by trying. Only the global SMMU registers (SCR0, IDR, SMR, S2CR) are safe.
A second framebuffer at `0x9ca00000`, inside the firmware's `cont_splash_region@9c000000` (0x2300000 bytes),
works; map it with `IOMemoryDescriptor::withPhysicalAddress`, not `ml_io_map` (10 MB panics: the IO map range is
too small). See [../research/hardware-qualcomm.md](../research/hardware-qualcomm.md).

### The presentation pipeline

- **Scanout (`fb_push`).** At the first frame autorefresh is turned off, as Linux's
  `dpu_hw_intf_disable_autorefresh` does. Each frame goes into the framebuffer the panel is not being sent from,
  VIG0/VIG1 are pointed at it, and CTL0 is flushed and started. The tear check holds the transfer to the panel's
  TE; PINGPONG_0's done interrupt (MDP INTR bit 8, MDSS SPI 83, GIC 115) frees the other buffer. `fb_push=1`
  keeps one buffer and waits for each transfer; `fb_push=0` gives back the firmware's autorefresh. On return to
  the loader and on a panic the boot framebuffer and autorefresh come back (`hk_display_quiesce`).
- **90 Hz.** The panel's own switch, as Android's device tree has it for `timing@1` of the ANA6705: nine DCS
  long writes through DSI0's command DMA at the same DSI clock. The packets sit past the last pixel of the boot
  framebuffer, in memory the display SMMU already maps. `fb_hz=60` switches back (the panel keeps its rate across
  runs). The measured TE rate is logged.
- **120 Hz on the OnePlus 8 Pro** (profile default since 2026-10-28). `panelRate(120)` sends this panel's own `timing@1` switch from the live device tree (11 DSI commands, same
  DSI clock as 60: `60 22`, `ba 02`, `f7 0f`, CASET/PASET, `b9 01`, `b0 01`, `b9 0c 54 00 0b`, 1080 x 2376), then INTF1's tear-check count is scaled to 120. Checked on the phone
  (RAM run of `out/instantnoodlep-standalone23.img`, `fb_hz=120`): "TE 60 Hz before, 110 Hz after" by the panel's own TE counter, `UIScreen maximumFramesPerSecond` 120, and the
  owner saw a clean picture that was clearly smoother. Dropped frames remain here and there (TODO: measure). Boot-arg `fb_hz=` must be given once.
- **Dropped frames at 120 Hz (2026-10-28): measured; the first explanation was wrong.** Workload: `userland/root-perf2.apfs` (rc.jobs: `rwtouch` taps Settings, then 40 rounds of flicks and
  drags, one `rwtouch` per line: a line takes few arguments), boot-arg `fb_kicklog=1` (only the `tl: k` line of each push; `touch_lat=1` writes six lines a frame, more than the loader's
  log window holds at 120 a second, and lines are lost), the intervals between consecutive pushes while the screen draws at 90+ a second (`perfrun.sh`/`fs2.py` of the session, kept in the scratchpad: take the `tl: k`
  lines, ignore intervals across a lost id). Result: **runs fall into two groups whatever is changed**: about 87.5% of the intervals within one refresh (<10 ms), 7.2% longer than 12.5 ms, push-to-done p95 12.6 ms
  (seven runs), or about 93% / 6.5% / p95 8.27 ms (five runs). Per-boot, not per-setting: the same kernel with the frame boost off gave 87.4, 88.2 and 93.3, with it on 87.0, 87.7 and 93.1; a governor held at its top row
  (`cpufreq_gov=0`) gave 93.5 once; `gpu_dcvs=0` 87.8 and 88.2. So the CPU governor's tick, the GPU's clock and a boost from the display driver (`fb_boost=1`, `frameBoost()` / `hk_cpufreq_frames()`, left in and OFF) were not the cause.
  The two groups differ in where the late intervals fall (10-14 ms in the first, 14-18 ms in the second): the phase of the pushes against the panel's TE, a property of the boot. The GPU stage of a frame
  (`tl: g`) is 5.75 ms median, 8.0 p95 at 120 Hz: close to the 8.33 ms budget, the real limit. Not tried: why a boot lands in one group (the phase), and a cheaper frame (scaling by the MDP's scaler
  instead of a GPU blit; Mesa tile mode).
- **Scaling by the display engine instead of the GPU blit: measured first, not built (2026-10-28).** `fb_probe=3` (read only) dumps the VIG pipes' scalers: VIG0 (SSPP 0x5000) and VIG1 (0x7000) have one at
  SSPP + 0xa00, `HW_VERSION 0x3000` (DT: "qseedv3lite"; Linux programs a version >= 0x2004 with the Lite tables), idle, and the two separable coefficient tables (+0x100, +0x300, 33 words) read all zero; mainline
  Linux sets `lut_flag = 0`, Android's tables come from its user space (not here). Then the GPU was measured (`gpu_stats=1`: "tl: u busy N permille of 1 s in M submissions" from the core's own timestamps): during the
  scripted Settings scroll the **3D core is 48.5% busy, 0.67 ms a submission, 6.6 submissions per frame at 111 frames a second**, i.e. about 4 ms of every 8.3: not the bottleneck. The scaling blit is a part of that
  4 ms, and the MDP would need the unscaled frame in the display's memory (the display surfaces here are shared images in ordinary memory, the MDP's SMMU maps the splash region only) and a coefficient table whose format
  is unknown, with the picture visible only to the owner's eyes: a gain of well under a millisecond a frame for all that. Not built. `libhackios-gl` can be rebuilt on this Mac now (`tools/macdev/glbuild`, gl.xml from
  Mesa 25.0.7's tarball and the exports of the shipped libOSMesa; a rebuild without changes is byte-identical to the shipped library), which was the other obstacle.
  What limits a frame is the window server's CPU time (3 to 9.5 ms a frame in `hk-gl: frame` lines; the frame's GPU work already overlaps the next frame's CPU work: the display's driver waits for it, `glwait`); `MESA_GLTHREAD`
  through `glenv=` changed nothing. A second GPU copy per frame (the mirror into the display surface) does not exist here (`glmirror=0` test: no change).
- **CoreAnimation at 90.** iOS takes the panel's frame period from the framebuffer property `IOMFBDisplayRefresh`
  (`displayMinRefreshInterval` in 2^-32 s, `displayRefreshStepMachTime` in mach time), not from `TimingElements`.
  Without it an iPhone X is 60 Hz everywhere. The window server's frame clock is our VSync notification
  (`ca_vbl`, automatic when the panel is faster than 60), fired at the panel's TE as the tear check's read-pointer
  interrupt reports it. `ca_fps` (automatic; `ca_fps=0` off) gives SpringBoard's animations and display links a
  30..90 range with a high-frame-rate reason, as a ProMotion iPhone's get; without it the busy-second median was
  74 fps instead of 87.
- **The tear.** The window server alternates two surfaces (3 and 4) and draws the next frame as soon as
  `IOMobileFramebufferSwapWait` returns, which used to be immediately, so the GPU wrote pixels being copied.
  `SwapWait` now waits until the surface it will draw into next has been copied. A detector that logs GPU work
  naming a surface during its copy found 0.
- **Scaling by the GPU (`fb_gpu=1`, default).** The two framebuffers are scanout `MiniIOSurface`s
  (`MiniIOSurfaceCreateScanout`), write-combined, inside the firmware's display region. After CoreAnimation draws a
  frame `userland/src/eagl.c` asks for a framebuffer that is neither waiting nor being pushed (framebuffer
  selector 200), scales the frame into it with `glBlitFramebuffer(..., GL_LINEAR)` and says so (201). The swap
  that follows pushes that framebuffer after its GPU fence if the frame's surface has had no GPU work since; the
  CPU copies nothing. Render targets need rows in multiples of 64 bytes, so the pipes read rows of 4352 bytes
  (`SSPP_SRC_YSTRIDE0` 0x5024/0x7024) instead of the firmware's 4320. The firmware's pitch and the last frame
  repacked to it go back with the boot framebuffer for the loader. A swap with no such frame is copied by the
  CPU as before, in 48-row pieces taken in turn. `fb_gpu=0` keeps everything as before.
- **GPU fences raise an interrupt** (`gpu_irq=1`, GIC 332, `RBBM_INT_0` bit 20 from `CACHE_FLUSH_TS`), so a wait
  sleeps instead of polling every 200 us.
- **Touch.** S6SY761 reports arrive by the TLMM summary interrupt (`touch_irq=0`: 10 ms polling). The worker runs
  at priority 92: at the default 81 the copying threads delayed it by up to 7 ms at 90 Hz.

`xl screenshot` takes the framebuffer the kernel pushed last, at the pitch it was written at (`loader_screen` in
`osfmk/arm64/loader_return.c`); a loader built before this reads the boot framebuffer at 4320, skewed.

### Numbers (ms, median / p95; 2026-10-23)

| stage | firmware autorefresh, 60 Hz (`022537`) | 90 Hz, CPU scaling (`164134`) | GPU scaling (`184104`) |
|---|---|---|---|
| SwapEnd to presenter (queue) | 0.04 / 30.1 | 0.03 / 5.7 | 0.03 / 0.08 |
| SwapEnd to frame ready to push | 12.1 / 46.8 | 5.9 / 17.1 | 5.8 / 10.8 |
| kick to panel RAM written (TE + 8.7 ms transfer) | none (tore) | 11.36 / 11.36 | 11.36 / 11.37 |
| SwapEnd to panel RAM written | not measurable | 17.2 / 28.5 (max 229) | 17.2 / 22.2 (max 118) |
| frames in a busy second (p50 / p90) | at most 60 | 87 / 88 | 87 / 88 |
| swaps a refresh late (busy windows) | | 5.6% | 2.2% |
| touch report line to worker | 0.016 / 0.032 (IRQ) | 0.017 / 0.031 | 0.015 / 0.028 |
| touch I2C read (se13) | 2.2-2.8 | 3.1 / 3.6 | 3.0 / 3.8 |

The numbers in brackets are log time stamps in `logs/`. With GPU scaling 98-100% of the frames of a busy five
seconds are pushed as the GPU drew them. The median from SwapEnd to the panel did not move: it is one push in
flight at a time, TE plus an 8.7 ms transfer. About 2% of swaps still come a refresh late (TODO).

GPU DCVS (`gpu_dcvs=1`, see [gpu.md](gpu.md)) follows load: above 50% busy straight to 525 MHz, one level down
(400, 305) only after four periods under 25%. At the same place in a session the gestures' frame rates were the
same with it on or off.

Measurement procedure and fallbacks:

```sh
. common/lib.sh; load_device instantnoodle          # never a bare ./xl enter (redwood's UEFI)
nix-shell --run './xl enter && ./xl ramdisk userland/flashboot.apfs'
XL_SECS=180 XL_FOLLOW=4 XL_FOLLOW_KEEP=1 XL_CMDLINE="cpus=8 cpufreq=1 bootcpu=7 ufs=2 root_apfs=ufs_darwin gpu=1 metal=2 metal_profile=1 touch_lat=1 log_launchd=0" ./xl run
# fallbacks: fb_hz=60, fb_push=1, fb_push=0, ca_vbl=0 / ca_fps=0, touch_irq=0
python3 tools/pipeline-latency.py logs/HHMMSS-run.log logs/HHMMSS-run.log.live.*
```

Host checks: `CC=clang python3 tools/instantnoodle-input-test.py`, `python3 tools/pipeline-latency.py --self-test`.

### Idle cost

A home screen left alone drew 86 frames a second because the Clock icon's two display links
(`SBUIPreciseClockTimer`, `SBHClockApplicationIconImageView`), which `ca_fps` made ask for the panel's full 90 Hz,
kept animating: backboardd used 7.3 s of CPU time in 15 s. They are held to `ca_ambient=30` fps (a third of the
panel's rate, so frames fall on refreshes evenly); backboardd now uses 2.5 s in 15 s. Every other animation and
link still asks for 90. Details: [../research/ios-userland.md](../research/ios-userland.md).

## Smoothness: the clock was the cause (2026-10-27, perf session)

**Measured (OnePlus 8 Pro, RAM run, boot-args of the known-good config + `touch_lat=1 cpuload=100`, kernel `perf0`, `logs/perf/base1.live`):** with no `cpufreq=1` the gold and prime clusters sat at EPSS row 0 (710 / 844 MHz; `perfctl:` said "0 MHz"), the memory vote was never raised and no governor ran. All seven cores were 99-100% busy in the first gesture, `rwtouch idle 60 30` did not settle, and the first swipe ran at 55.8 frames a second with 32% of frame steps over 16.7 ms, the window server's CPU draw (Mesa) 15.4 ms of a 16.7 ms frame. **Fix (built, `perf1`; verified on the phone by log and by the owner's eye, "much snappier"):** `hk_cpufreq_enabled()` in `hk_cpufreq.c`: `cpufreq=N` if given, else on for `board=instantnoodle*` (the 8 had it in its profile, the 8 Pro did not); rows 0 -> 17/19 (2419 / 2841 MHz), governor "takes over", memory vote 16383 busy. `cpufreq=0` restores the old behaviour for A/B. **Not measured:** frame-time distribution after the fix (the run was stopped before the gestures), 120 Hz. At `fb_hz=60` CoreAnimation uses its own timer, not the panel's TE (`ca_vbl` is automatic only above 60 Hz): `ca_vbl=1` is an untried boot-arg experiment. Test image with idle and gesture probes: `userland/root-perf2.apfs` (small image + `/hackios/etc/rc.jobs`, made with `7zz x` / `tools/apfs/mkapfs.py`).

## Screen off and on

backboardd asks the framebuffer for the display's power (`IOMobileFramebufferRequestPowerChange`, selector 12):
`0` when the screen locks or dims out, `1` when it wakes. Verified on the phone (2026-10-25): the panel goes dark
and lit in about 30 ms and the picture is right after the wake.

| Screen off | Screen on |
|---|---|
| panel: DCS display off (0x28) | display on (0x29) |
| pushes to the panel only remembered; the latest is sent at wake | the latest frame pushed |
| no VSync notifications | VSync resumes for every client |
| touch chip (S6SY761) held in reset, no 100 ms watchdog | chip started as at boot (about 145 ms) |
| memory vote down to `ddr_dark=` (1024) | every core group at its top row for 150 ms, memory vote at its top, GPU DCVS looked at at once |

**Never send DCS sleep-in (0x10) to the ANA6705.** After it the panel loses its DSC setup and shows coloured
static. Neither sleep-out plus display-on nor Android's whole on-command brought the picture back: Android's
picture parameters are not what the firmware set the display engine up with. Android resets and repowers the
panel to recover; here the firmware's setup has to stay, so only 0x28/0x29 are used. Only a phone reset
recovers it.

**Frame pushes.** One push that does not finish in 100 ms no longer drops the display into the firmware's
autorefresh for the rest of the run (CPU-copied frames, tearing, no GPU scaling). That happens only after 3 failures
in a row (`PUSH_FAILURES`). A push whose done bit is set but whose interrupt never came is counted as done and
logged ("a push was done but its interrupt (115) did not come"); it should not happen. DCS packets go through
DSI0's command DMA from a page between the two framebuffers (`DSI_DMA_PHYS`). Status of the 3-strikes rule and
of the interrupt-quiesce fix that made it necessary: see [power.md](power.md).

## Brightness

**Saved brightness at boot (fixed 2026-10-28, kernel `ufs40`, image 29).** backboardd restores the saved level within the first seconds of a boot; `hk_display_set_backlight()` could only keep it (log: "not sent: no DSI command path") because the push path, and with it the DSI
command path, starts with the first frame, and nothing sent the kept value afterwards: the panel stayed at the firmware's level (0x168) until iOS asked again. `backlightFirstSend()` (end of `pushStart()`) sends it; log line "the brightness iOS set earlier (panel value ...) sent now that the push has started".
Auto-brightness: see TODO T04 (no light sensor reaches the application processor: it is behind the DSP's sensor framework).


Before 2026-10-26 there was none: backboardd sets brightness through a service that was not there (UserEventAgent
logged "Error calling IOServiceAddInterestNotification for backlight service").
`MiniAppleARMBacklight.cpp` is that service (class `AppleARMBacklight`, registry name `backlight`,
`backlight-control` true, `IODisplayParameters`). What iOS sets on it was found by logging every property
(`MiniAppleARMBacklight: backboardd sets ...`): `backlight-level` and `brightness` as 16.16 fractions (0x8000 =
50%, 0x10000 = 100%), `FadePeriod`, `DisplayBrightness`, `DisplayBrightnessSliderPosition` (the nits fields are
0). `backlight-level` becomes the panel's DCS `write_display_brightness`: `51 hi lo`, 11 bits, 1..0x7ff, high byte
first, in high-speed mode (the ANA6705's device tree: `qcom,mdss-dsi-bl-max-level 0x7ff`, `bl_ctrl_dcs`, HBM
commands `51 0c 7c`), with the level squared (a slider is perceptual, the panel's value linear in light). It is sent
again when the panel is lit after a dark (the panel loses it), and the firmware's default 0x168 is put back on the
way to the loader (backboardd sets level 0 at shutdown; the loader's menu stays readable).

- **Verified:** the path from `BKSDisplayBrightnessSet` in SpringBoard through backboardd to the panel's DSI
  command. 20/90/5/100% reach the panel as 0x52/0x67a/0x6/0x7ff; the link takes the writes
  (`kern.hackios.backlight`, `backlight_sent`, `tools/t brightness`), one more after a wake. Test hook:
  SpringBoard Darwin notifications `com.hackios.brightness.{5,20,50,90,100}` (`inject_sb.c`).
- **Not verified:** that the light changes (nothing here measures light) and the byte order on the panel (no DSI
  read is implemented; the device tree's own `51 0c 7c` is the evidence).
- There is no ambient light sensor in the port, so auto-brightness has nothing to read, and CoreBrightness's wait
  for one (`-[BLControl waitForALSArrival]`, five seconds) still happens at window-server start: `inject.c` has a
  `brightness_fixes()` that skips it, but it is switched off because with it backboardd stopped adopting our touch
  service (see [input-peripherals.md](input-peripherals.md)).

## Night Shift, True Tone, Auto-Brightness (OnePlus 8 Pro, 2026-11-07, verified on the phone, RAM images 191-206)

- **Night Shift:** CoreBrightness hands CoreAnimation a colour matrix (`-[CAWindowServerDisplay setColorMatrix:scale:rampDuration:]`, and the
  white point with `setWhitePoint:rampDuration:error:`); an iPhone's display pipe applies them, CoreAnimation finds none here and drops them.
  `inject.c` `display_colour_start()` passes them to the framebuffer driver as `IOMobileFramebufferSetMatrix` (selector 55; location 1 the
  white point, 2 the colour matrix, 3 True Tone); `MiniIOMobileFramebuffer.cpp` keeps them (also `GetMatrix`, 54) and gives them to the
  window server (selector 202), whose last GPU pass (`eagl.c` `panel_tint()`, instead of the blit into the panel's framebuffer) multiplies
  every pixel by their product in linear light. Seen: Night Shift's matrix R 0.80 G 0.67 B 0.40, the whole screen warm; off: the blit again.
- **True Tone:** the switch (Display & Brightness) shows when the server's `SupportedColorFX`/`SupportsAmbientColorAdaptation` is set
  (hooked: CoreBrightness's own colour module never takes this ALS for its adaptation). The switch is kept by `inject.c` (preference
  `TrueTone` of `com.hackios.display`); while on, the colour sensor's temperature (SEE `rgb`, 3900-4300 K under the room's lamps) sets a white
  point halfway from D65 towards it (4500-7500 K, Bradford adaptation) as matrix location 3: seen 5226 K for 3948 K light.
- **True Tone flicker in Maps (fixed 2026-11-07, RAM images 208-210; the owner's look still due):** two causes. (1) Frames the GPU did not
  scale into a panel framebuffer (a swap of more than one layer, a surface drawn into again after `panelReady`, none free in time) were
  copied by the kernel without the matrix: `MiniIOMobileFramebuffer.cpp` `tintRefresh()`/`tintPixel()` now put those copies through the
  same product, in linear light, with integer tables (at most 2 of 255 from the shader's result, in dark tones). (2) The sensor's
  temperature wobbles 100 K and more a second, and each step was a jump of the white point: `true_tone_apply()` filters it (~5 s), ignores
  changes under 40 K and fades in steps of 15 % (at least 12 K). Seen: the matrix's blue gain 0.715 -> 0.692 in small steps.
- **Auto-Brightness:** Settings shows it where MobileGestalt's `ambient-light-sensor` is true, which is an IOService of PrimaryUsagePage
  0xff00 / PrimaryUsage 4 (`MiniALSServiceStart()` publishes one with slpi=1; MobileGestalt's cache had to be removed once to see it);
  CoreBrightness's auto-brightness runs on the ALS HID service (`inject.c` `als_start()`, made at once so that CoreBrightness finds it when
  it starts, fed with SEE's lux and the light's XYZ, `copyEvent` answered): the backlight follows the room (levels 32333..33104 seen ramping).
  The loader also adds `arm-io/aop/iop-aop-nub/als` to the device tree (another MobileGestalt path). `als=0` turns the ALS service off.

## What iOS asks of the framebuffer (research notes)

- Frame period comes from `IOMFBDisplayRefresh`, not `TimingElements` (above).
- IOSurface and framebuffer selectors, and the swap request layout, are in `MiniIOMobileFramebuffer.cpp`.
- CoreAnimation draws a layer's pixels with one of ninety "functions" (a fragment shader each); its OpenGL ES
  renderer lacks some; see [../research/ios-userland.md](../research/ios-userland.md) for which, and for the
  vibrancy and backdrop findings.

## Related

- [gpu.md](gpu.md) for scaling and fences; [power.md](power.md) for sleep and the screen; [input-peripherals.md](input-peripherals.md) for touch drivers.
- [../research/hardware-qualcomm.md](../research/hardware-qualcomm.md) for SMMU/CrashDump rules.
