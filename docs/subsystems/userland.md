# iOS userland integration

The iOS 16.5 (20F66, iPhone10,3) user space runs **unmodified on disk**: not one byte of Apple's programs,
frameworks or the dyld shared cache is changed. What iOS misses is supplied by (a) kernel stand-ins for closed
drivers and (b) libraries of ours that the kernel inserts into chosen processes with `DYLD_INSERT_LIBRARIES`
(`kern.exec_inject`; dyld honours it because the AMFI stand-in says so). The userland sources belong to the
**userland repository** (`hackios-xnu-userland`, `common/ugit`); see [../architecture.md](../architecture.md) for the
two-repo layout. No Apple file is in either repository: `ios165/fetch.sh` downloads the IPSW from Apple's CDN.

iOS believes the phone is an **iPhone X** (`iPhone10,3`): the identity (`xnuloader/identity.h`) is made from an iPhone's
device tree by `xnuloader/mkidentity.py`. The screen is declared sRGB, not P3 (see [display.md](display.md)).

## Boot of the user space

1. The kernel runs `/sbin/launchd` of the big image = `stage0` (`userland/src/stage0.c`), which mounts the small image on
   `/hackios` and hands over. In a flash-root boot the iOS volume is the root and `init.c` (`flash_root`) bind-mounts the
   small image's pieces over it (e.g. `/progressui`).
2. `init` (process 1, `init.c`): mounts `/private/var` (ramfs, or the HFS+ data volume), runs `etc/rc` (normally `exec
   /sbin/launchd.ios`), checks what the kernel can do, one line per check; as a command runner it also runs a job list.
3. launchd.ios starts the stock jobs chosen by `userland/mkjobs.py` from iOS's own LaunchDaemons. `--rest` adds the
   rest of iOS (on demand: whatever starts by itself, `RunAtLoad`/`KeepAlive`, is removed so launchd runs a job only when
   a Mach service of it is asked for) and the "launch angels". The default job list is `IOS_JOBS` in `build.sh` (notifyd,
   cfprefsd, logd, containermanagerd, runningboardd, backboardd, lsd, powerd, ..., SpringBoard, wifid).
4. Standard error of every job goes to the console (no crash reporter exists: the kernel says "Failed to send exception
   EXC_CORPSE_NOTIFY" and nothing else; `systrace`, `usertrap:`, `userabort:` lines and our own fault reporters are the only record).

**The DENY list** (`mkjobs.py`): jobs never loaded because they drive missing hardware or loop. It contains `watchdog`,
`CommCenter`, `wifi`/`WiFi` (the radio daemon family; `com.apple.wifid` is added by `WIFID`), `bluetooth`, `BTServer`,
`locationd`, `softwareupdate`, `thermalmonitord`, `keybagd`, `nfcd`, `biometrickitd`, `pearld`, `camera`, `audio`,
`ReportCrash`, `usb`, `accessoryd`, `mobileassetd`, `bird`, `homed`, `routined`, `photoanalysisd`, `avconferenced`,
`com.apple.voicemail.vmd` and others. Rules recorded in that file: a daemon can misbehave *because of another entry* (cloudd
spun retrying `nsurlsessiond` because that was denied); retest before trusting an old note; compare with a control from
the same afternoon. `ALLOW="word ..."` lets one through for a trial. Deliberately **not** denied: `mediaserverd` (it is where
VideoToolbox decodes HEIC, the iOS 16 wallpaper layers), `nsurlsessiond`, `apsd`, `mobileactivationd`, `aggregated`.

## Libraries of ours (userland repo, `userland/src/`)

| Library | Process | What |
|---|---|---|
| `libhackios.dylib` (`inject.c`) | backboardd, containermanagerd | virtual HID touch/buttons/slider service from `/dev/hk-touch`; renderer selection; OpenGL ES loader (`eagl.c`); ca_vbl/ca_fps frame clock; personas disabled; a fault reporter |
| `libhackios-sb.dylib` (`inject_sb.c`) | SpringBoard | activation/setup bypass, snapshot format, vibrancy and long-press fixes, alert slider to Do Not Disturb, status-bar clock offset, battery hooks, ca_ambient, layer dump, unlock |
| `libhackios-media.dylib` (`inject_media.c`) | mediaserverd | skips portrait prewarm (crash at address 0), flashlight device, camera device (`camera_device.c`, `camera_isp.c`: viewfinder at 30/60 fps pacing, flash for photographs, `camera: timing:` lines), software HEVC encoder (stills only) |
| `libhackios-app.dylib` (`inject_app.c`) | Sileo, apps | gives an app a stderr and a signal-time stack under the shared-cache slide |
| `libhackios-tweaks.dylib` (`inject_tweaks.c`) | Preferences, SpringBoard | tweak loader for `/var/jb/usr/lib/TweakInject` (ElleKit is the engine) |
| `libhackios-store.dylib`, `libhackios-fairplay.dylib` | appstored, fairplayd | experiments, built only with `STOREFRONT=1` |
| `HKAudio.driver` (`audio_plugin.c`) | mediaserverd HAL | the output/mic/actuator devices, see [audio.md](audio.md) |
| `libhackios-gl.dylib` (`eagl.c`) + Mesa | backboardd | OpenGL ES, see [gpu.md](gpu.md) |
| `hk_cell` (`hk_cell.c`) | tool | `status`, `watch N`, `sms NUMBER TEXT` over the kernel modem service's sysctls (`kern.hackios.cellular_*`, `modem_*`) |
| `hk_time` (`hk_time.c`, `com.hackios.time`) | launchd job | keeps the date across restarts: saves (PMIC RTC seconds, date) to `/private/var/hk_time` every 20 s and at start sets the clock to saved date + RTC delta when that is later |
| `hk_sysctl`, `hk_buddy`, `hk_logcfg` | tools | print sysctls; read/set the setup assistant's preferences (`set`/`undo`); write a unified-log debug profile |
| `libhackios-sb.dylib` cellular hook (`inject_sb.c`) | injected | the status bar's signal item from `kern.hackios.cellular_*` (bars, signal mode 1 = active); skips the second SIM's item and never writes the service name (SpringBoard aborted when it did) |
| `thermald` (`thermald.c`) | launchd job | posts `com.apple.system.thermalpressurelevel` from `kern.hackios.thermal_pressure` |
| `var_store` (`var_store.c`) | launchd job | persistence, see [storage.md](storage.md) |
| `rwtouch`, `power_test`, test programs | test jobs | scripted touches, power tests, hardware tests |

**Every address into the shared cache is for iOS 16.5 (20F66) iPhone10,3**, and every replacement is checked against what
is expected there before anything is written (`replace_method_of_kind()`: dyld interposing does not reach calls made
inside the shared cache, so methods are replaced by address). A different iOS build needs those tables re-derived.

### Tweaks and package manager

`tools/mk-jb.sh`/`jb-preinstall.py` build a Procursus bootstrap + Sileo for a `/var/jb` prefix (`JB=1`). Sileo, dpkg/apt,
tweaks and SSH are listed as working in the README. `nsurlsessiond` must run or Sileo's downloads sit on "Queued"
for ever. `/var/mobile/tmp` must exist (apps without a data container keep temporary files there).

### Job lists

`userland/rc.jobs*` (about 119 files) are scenarios run by `init`, not a shell: lines split on blanks, no quoting, no
variables, only `>> FILE` and a trailing `&`. iOS 16.5's root has **no shell** (`/bin` is `df` and `ps`); a line beginning
`/bin/sh` fails with status 127 on a flash-root boot. A program that needs a value needs an option for it, not an
environment variable. Notable: `rc.jobs.tour` (unlock and open every app of page one), `rc.jobs.installed` (the installed
boot, no diagnostics), `rc.jobs.power`, `rc.jobs.speakers/alerts/volume/mic/micquiet`, `rc.jobs.gpuiso`, `rc.jobs.metal*`,
`rc.jobs.look`, `rc.jobs.wakeperf`, `rc.jobs.fps`, `rc.jobs.maps`. `mkjobs.py` and `jobinfo.py` generate and inspect them.

## Findings about iOS behaviour here

Condensed from the source comments; the full account with offsets is in [../research/ios-userland.md](../research/ios-userland.md).

## Status bar and lock screen on the OnePlus 8 Pro (2026-10-28, owner: "it works" with `userland/root-sb2.apfs`, RAM boot)

iOS draws at 1290 x 2796 (430 x 932 pt, 3x; it was 1320 x 2868 until 2026-10-28, when the keyboard's digits plane turned out to need a size iOS 16.5 knows, K43; the numbers below were measured at 440 pt and carried over, see inject_ui.c); the camera hole is at x 20..56 pt, its centre 27.9 pt from the top (measured on the owner's photographs;
panel columns 61..137 of 1080). UIKit lays this screen's status bar out with `_UIStatusBarVisualProvider_DynamicSplit` (the iPhone 14 Pro's: an "ear" box
each side of a fake Dynamic Island taken from the screen's `_exclusionArea`; items centred in their box). `userland/src/inject_ui.c` (`libhackios-ui`, also in
SpringBoard) hooks DynamicSplit's `sensorAreaRect` (island moved right by `HK_SB_CUTOUT_RIGHT` 30 pt: clock box and icon box centres each move about 15 pt) and
`edgeInsets` (`HK_SB_CLOCK_UP` 5 pt off the top inset for the clock's centre line only: the first call per provider is the layout setup, the later ones the
clock's updates), only on a screen 1320 px wide. Its `FixedSplit` hooks (`HK_SB_CLOCK_RIGHT`, `HK_SB_BATTERY_RIGHT`, `HK_SB_BATTERY_UP`) are a fallback that did not run
here (the first image, root-sb1, used only those and changed nothing). The console says the values and, 8 s after the first answer, the status bar's view frames
(`libhackios-ui: status bar geometry ON ...`, `sensorAreaRect x .. moved right`, `layout of SpringBoard's status bar`). "A bit more right" is `HK_SB_CUTOUT_RIGHT`
(the icon box must stay about 90 pt wide or UIKit shrinks the icons). The old boot-arg `sb_clock=N` (inject_sb.c, 1080-wide iPhone X identity) is separate: do not
combine it with this. Applications get it only where the library is injected (INJECT in build.sh: Preferences, Safari, Notes, Maps, Messages); Camera and the rest
still draw the clock under the hole.
The thin cyan bar under the battery on the lock screen is (inferred) the Control Center coaching grabber of the cover sheet (`CSTeachableMomentsContainerView`
`_addControlCenterTutors`; shown while `userInvocationCount` < 2, kept in /var, empty at every boot): `cc_coaching()` in `inject_sb.c` hides it
(`HK_SB_HIDE_CC_COACHING`; Control Center itself is untouched). Build: `tools/macdev/ubuild OUT sb ui`; image `userland/root-sb2.apfs`.

## Files and boot-args

Boot-args read by userland (via `sysctl kern.bootargs`): see [../BOOT_ARGS.md](../BOOT_ARGS.md) ("Userland" table).
Build options (`RC`, `JOBS`, `REST`, `WIFID`, `OSLOG`, `INJECT`, `STOREFRONT`, `EXTRA`, `EXTRA_SHARE`, `RC_JOBS`, `GL`, `ADSP`,
`FLASHBOOT`, `CRYPTEXES`, `PREBUILT`, `CHECK`, `SYSTEM`) are documented at the top of `userland/build.sh`; the recipe is in
[../BUILD.md](../BUILD.md).
