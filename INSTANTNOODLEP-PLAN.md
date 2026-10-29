# OnePlus 8 Pro (instantnoodlep) port plan

Written 2026-10-26 from the docs only. No phone was attached, nothing was built, nothing was run.
Items marked *(from docs)* come from the repos. Items marked *(my belief)* are not verified and must
be checked against the 8 Pro's own live device tree.

## Base to use

| Repo here | What it is | Use it? |
|---|---|---|
| `iphont-xnu-kernel`, branch **`origin/instantnoodle`** (62 commits) | HackiOS with a finished OnePlus 8 board: UEFI package, loader platform, SM8250 drivers, GPU, audio, Wi-Fi, flash install | **Yes, this is the base.** `main` there is only a 2-commit redwood snapshot. |
| `iphont-xnu-userland` | iOS 16.5 userland build and hooks, with OnePlus 8 support | Yes |
| `hackios-xnu-kernel` / `-userland` | Upstream HackiOS, POCO X5 Pro (redwood) only, no OnePlus code | Reference only |
| `ios-instantnoodle` | Older "sweet"-style shim port (iOS 16.7.16). Docs and notes only | Reference only (SM8250 notes) |

A worktree on that branch is at `hackios-op8pro/` (local branch `instantnoodlep`). The original
clones were not modified

## Blocking decision: iOS version

HackiOS targets **iOS 16.5 (20F66)**. The hooks in `inject*.c` are patched by address and are valid
for that build only; the kernel is xnu-8796.121.2. The IPSW being downloaded is **16.7.16
(20H392)**, which belongs to the older `ios-instantnoodle` approach (xnu-8796.141.3). The two do not
mix. `ios165/fetch.sh` downloads the 16.5 IPSW itself (7.4 GB).

## What differs from the OnePlus 8 (same SM8250, same GIC/UFS/PMIC/Wi-Fi/GPU)

| Area | OnePlus 8 (in the branch) | 8 Pro, to find out |
|---|---|---|
| Panel / framebuffer | ANA6705, 1080x2400, DSC command mode, 90 Hz via 9 DCS writes, fb 0x9c000000, stride 4320, splash region 0x2300000 | *(my belief)* QHD+ ~1440x3168, 120 Hz, a different Samsung panel, so other stride, DSC setup, refresh-switch packets and brightness commands. Two framebuffers must fit in the splash region. `MiniIOMobileFramebuffer.cpp` and the `fb_gpu` scaling path hardcode the 1080x2400 geometry. **Biggest risk.** |
| Touch | Samsung S6SY761, bit-banged I2C, SE13 gpio36/37, reset 38, irq 39 | *(my belief)* a different IC (Synaptics?) on other pins. Needs a new driver or a pin/address change in `instantnoodle_touch.c`. |
| UEFI | `instantnoodlePkg`, DXE drivers mostly from `kebab`, USB DXE from the phone's own XBL, FD at 0x9FC00000 | New `instantnoodlepPkg`; DXE binaries extracted from the 8 Pro's own `xbl` with UEFIReader |
| Loader | `platform_instantnoodle`, picked by fb base; store = `userdata` LBA 4133800..30513146 | New `platform_instantnoodlep` with its own fb base and the 8 Pro's `userdata` GUIDs and LBAs |
| Audio / haptics / battery / slider | 2x TFA9874 (SE15), AW8697, bq27541 gauge (SE16 0x55), 2x IST8801 | Probably similar, but values (I2C pins, calibration, firmware, battery capacity) must come from the 8 Pro DT, `persist` and vendor firmware |
| RAM | 8 GB | 8 or 12 GB, depends on the SKU |

## Steps

1. **Collect** from the phone while it runs LineageOS with root (needs adb): live FDT, `/proc`
   files, dmesg, by-name partitions and the partition backups listed below.
2. **Audit**: diff the 8 Pro's live DT against the 8's (`instantnoodle-live.dts` is in
   `ios-instantnoodle/research/` on the author's machine, not in this clone). Fill the table above.
3. **Scaffold** with `common/new-device.sh instantnoodlep <live.dtb>`: profile, loader platform,
   boot-arg board overrides.
4. **UEFI**: `instantnoodlepPkg` from `instantnoodlePkg` plus the 8 Pro's own DXE binaries, then a
   `fastboot boot` of the UEFI alone (the loader screen and log show up).
5. **RAM-only kernel boot** with `cpus=8` and no drivers, as on the 8. Nothing is flashed.
6. **Display geometry** for the 8 Pro, then touch, then keys. Each behind a boot-arg, read-only first.
7. **Peripherals** in the same order the 8 followed: cpufreq, GPU, audio, haptics, battery, Wi-Fi.
8. **Install to flash** (`userdata` and `boot_b`) only at the end, and only with your explicit
   approval for each write.

## What I need from you

Required:
- **Phone plugged in over USB** with adb authorised and root (`adb root` or `su`), ideally in
  LineageOS. No device was visible to adb or fastboot when I checked.
- **Which 8 Pro variant** (IN2020 / IN2023 / IN2025, 8 or 12 GB).
- **Decision on iOS 16.5 vs 16.7.16** (above).
- **Disk space**: this Mac has about 41 GB free (96% used). The IPSW, extractions, images, Mesa and
  the Mu-Silicium clone (~3 GB) need roughly 40-60 GB. An external drive would help.
- **Approval to wipe `userdata`**: the OnePlus 8 install replaces Android entirely. I will not touch
  it without a fresh yes for each flash write.

What I will pull from the phone (read-only, kept out of git):
`/sys/firmware/fdt`, `/proc/{iomem,interrupts,cpuinfo,cmdline,meminfo}`, dmesg, `getprop`,
`/dev/block/by-name`, and images of `xbl*`, `abl*`, `boot*`, `dtbo*`, `vbmeta*`, `persist`,
`modem*`, `devcfg`, `tz`, `hyp`, `vendor` (or `/vendor/firmware`), the GPT. All except `userdata`.

Nice to have, not required: LineageOS `android_kernel_oneplus_sm8250` and the 8 Pro DT sources, as a
cross-check on panel init and touch parameters. The live FDT should already contain most of it.

## Host tooling missing on this Mac

`7z` (23 or newer, needed to read APFS), `lld-link`, `gh`, `ipsw`, `mkbootimg`. `nix` is absent, so
the NixOS-style `nix-shell` commands in the docs must become Homebrew installs. Present: clang, dtc,
lz4, qemu, meson, ninja, python3, adb, fastboot. The kernel build needs `theos` or cctools plus the
iPhoneOS16.5 SDK.

## Safety rules inherited from the HackiOS docs

Never flash or write any partition without explicit per-write approval. Never touch ABL, XBL, TZ,
the slots or the bootloader lock. Never run `./xl enter` before loading the device profile. On
the 8, `touchscreen=1`, DCS sleep-in (0x10) and some register reads of the display SMMU crash the
phone into Qualcomm CrashDump (hold Power + Vol Up to leave it). Unplug USB before a reset that
should end in fastboot.

---

# Addendum 2026-10-26: audit of the attached phone (read-only)

Phone: **OnePlus 8 Pro IN2023 (EU), project 19811, 12 GB RAM (RAM to 0x37fffffff), LineageOS 22
(Android 15) with Magisk root, slot `_b`, bootloader unlocked (`orange`)**. `userdata` = sda23, 236 GB.
Same partition layout as the OnePlus 8 (sda/sdb..sdf LUNs, `xbl_a/b` on sdb1/sdc1, `boot_b` sde35).
Pulled into `op8pro-data/` (outside the repos, never to be committed): live FDT
`dtb/instantnoodlep-live.dtb`, decompiled `research/instantnoodlep-live.dts`, `/proc` files, getprop,
by-name map, I2C device list. `dmesg` is empty on this kernel (blu_spark), so nothing from it.
Partition backup (all but `userdata`, SHA-256 checked against the phone) runs via
`op8pro-data/backup-partitions.sh`, log in `op8pro-data/backup/backup.log`.

## Same as the OnePlus 8 (reuse the existing board code)
- SoC SM8250 `kona`, 4x A55 (0x805) + 4x A77 (0xd0d), GIC/timer/UFS (`1d84000`) addresses.
- `cont_splash_region@9c000000` size `0x2300000`, same as the 8. Memory banks as the 8 plus the 12 GB
  upper bank. Loader's `fb_base` 0x9c000000 is therefore **not** unique: the loader picks its platform
  by that address, so `platform_instantnoodlep` needs another discriminator (e.g. the UEFI-reported
  framebuffer width/height).
- Touch bus: `i2c@a94000` (SE13), address **0x48**, reset TLMM 38, IRQ TLMM 39, rails `pm8150_l13`
  (3.008 V) and `pm8150a_l1` (1.8 V). Identical wiring to the 8's S6SY761.
- Speakers 2x TFA9874 (0x34/0x35), haptics AW8697 (0x5a), gauge bq27541 (0x55), QCA6390 Wi-Fi (PCIe),
  alert slider (IST8801 hall pair 0x18/0x19 on i2c-3; MXM1120 pair is an alternate), NFC, fsa4480.

## Different (the real work)
1. **Display.** Panel "samsung AMS678UW01" (`ANA6706`), DSI command mode, DSC, **with a Pixelworks
   `iris5` coprocessor** (`pxlw,iris-chip-enable`, I2C 7-0026) in the path. Timings in the DT:
   timing@0 1080x2376 60 Hz, @1 1080x2376 120 Hz, @2 1440x3168 120 Hz, @3 1440x3168 60 Hz. Which mode
   ABL left running is **not known**; the 8's code assumed 1080x2400 at stride 4320. First task on
   hardware: `fb_probe=1` to dump MDP/DSC/INTF state. The 90 Hz DCS switch sequence, brightness
   command (max level 0x7ff, `bl_ctrl_dcs`) and the Iris setup must be re-derived from this DT. Two
   1440x3168 buffers (2 x 18.25 MB) just fit the 36.7 MB splash region.
2. **Touch IC.** `sec-s6sy791` (S6SY791), not S6SY761; panel coords 0x59f x 0xc60 (1439 x 3168),
   tx/rx 17x38. The 8's bit-banged driver (`instantnoodle_touch.c`) needs the IC id, coordinate scale
   and possibly register differences checked. The 8 Pro DT also lists a Synaptics S3908 @0x4b (an
   alternate; Android binds the Samsung one).
3. **Battery/charging.** Extra chips on i2c-8: `chgpump-charger`, `wlchg-rx-chip`, `bq2597x`; i2c-10
   `stm8s-fastcg`. Dual-cell pack. Only the gauge is needed for a level display.
4. **UEFI package, loader platform, storage LBAs** (userdata 236 GB: the 8's `store_last` does not apply).

## Host state
Installed: 7-Zip 26.05 (`7z` -> `7zz` in ~/bin), `lld`. Workspace `hackios-op8pro/` = kernel branch
`instantnoodlep` (from `origin/instantnoodle`) + userland from `iphont-xnu-userland` (`common/setup.sh`).
iOS 16.5 (20F66) is being taken out of Apple's IPSW by `ios165/fetch.sh` (log `hackios-op8pro/logs/
ios165-fetch.log`); needs `SSL_CERT_FILE=/etc/ssl/cert.pem` with the python.org Python.

---

# Addendum 2: UEFI package and loader platform (2026-10-26)

## Decisions
- **Dual boot (owner request).** Android stays. Design = the POCO's, not the OnePlus 8's: `darwin` is cut off the
  end of `userdata`; the UEFI+loader go into the spare slot's `boot` (boot_a); the loader menu offers Android and
  flips the A/B bytes in the GPT. **Blocker found:** this phone's `/data` is f2fs on `dm-43` with **metadata
  encryption** (raw `userdata` is random bytes, no f2fs superblock). `tools/userdata-carve.py` expects a plain
  f2fs superblock, so it must be fed the *decrypted* view (`/dev/block/dm-43`) and the shrink done in one Android
  session; still to be adapted and tested. Needs a backup of /data and per-step approval. Not needed for the
  first RAM-only boots (`fastboot boot`, nothing written).
- **Display, measured not guessed.** `abl_log`: `DisplayDxe: Resolution 1080x2376 (1 intf)`, panel id 00E7B701,
  `Iris_ResetExec` (ABL sets the Iris chip up). So the splash is **1080x2376, stride 4320** (the 8's stride), not
  QHD. The 8's geometry code follows the loader-reported framebuffer, so it largely carries over. 120 Hz
  (timing@1) and the MDP push path are NOT assumed: the loader entry passes `fb_push=0 fb_hz=60`.
- **DTB for the boot image:** the three `kona ... SoC 19811` variants of the phone's own `boot_b` (dtb_idx=3 is
  v2.1/19811) plus the same 173-byte RTIC tail the 8's image has.

## Done (all uncommitted, in worktree `hackios-op8pro/`, branch `instantnoodlep`)
- `uefi/files/Platforms/OnePlus/instantnoodlepPkg/` (copy of instantnoodlePkg: new GUID, SMBIOS "8 Pro",
  `PcdFrameBufferHeight` 2376), `uefi/files/Binaries/instantnoodlep/` (still the 8's/kebab's DXEs),
  `uefi/files/Resources/{DTBs,Configs}/instantnoodlep.*`, `common/devices/instantnoodlep.conf`.
- `xnuloader/loader.c`: `platform_instantnoodlep` (+ `fb_height` discriminator, the 8 now `fb_height=2400`),
  `PLAT_IS_KONA()` for the PMIC reset/SPMI paths and the menu entries (`flashboot.c`). Loader builds clean.
- Kernel: `instantnoodle_board()` and the framebuffer driver's `instantnoodle()` accept `instantnoodlep`;
  new `instantnoodlep_board()` for the 8 Pro-only code (touch IC, panel switch). Kernel NOT rebuilt yet.
- iOS 16.5 (20F66) extracted by `ios165/fetch.sh` (patched for BSD `wc -l` padding: macOS only).

## Not done / next
1. Backup reaches `xbl_b`; then extract the 8 Pro's own DXEs with UEFIReader (.NET) and replace the five USB
   drivers (and compare with the 8's BOOT.XF.3.2-00295 set) in `Binaries/instantnoodlep`.
2. `uefi/apply.sh` (Mu-Silicium ~3 GB) and build `instantnoodlep` for a `SILICIUM_UEFI.fd`. macOS toolchain is
   uncertain; plan = Docker (Linux arm64 container), Docker Desktop is installed but not running.
3. Build the kernel (`xnu/apply.sh`, theos/cctools + iPhoneOS16.5 SDK; Docker also an option) and images
   (`REST=1 userland/build.sh`: ~12 GB), then the **first RAM-only boot test** (owner is told first).
4. S6SY791 touch, then 120 Hz/MDP push, GPU, audio, haptics, battery.

---

# Addendum 3: first UEFI + loader image (2026-10-26) -- built, NOT yet booted

- Docker image `hackios-uefi-builder` (docker/Dockerfile, Ubuntu 24.04 arm64). UEFIReader (fc2e9e3)
  built and run **inside the container only**; pip requirements installed into `Mu-Silicium/.venv-build`.
- Extracted this phone's `xbl_b` (BOOT.XF.3.2-00358-SM8250-1; xbl_a is the same build) into
  `op8pro-data/uefi-extract/`. Nothing in its 92 .efi equals the OnePlus 8 / kebab baseline, so the baseline is kept
  and only the five USB drivers (UsbPwrCtrl, UsbfnDwc3, UsbDevice, UsbConfig, UsbInit) are this phone's own,
  the same recipe the OnePlus 8 uses. Fallback if the USB link does not start: the OnePlus 8's five.
- `Mu-Silicium` cloned at BASE + port applied (5.6 GB). Build: `docker/build-uefi.sh instantnoodlep`
  (28 s, 48 images verified). `SILICIUM_UEFI.fd` = 0x300000 as in the profile.
- `out/instantnoodlep-boot.img` (2.6 MB, sha256 769a98f6...): Android boot image v1, board HACKiOS, gzip(BootShim +
  UEFI FD + XNUP payload with loader.efi --usb) + the phone's own three 19811 DTBs. No kernel, no iOS inside.
  Boot with `fastboot boot` only (RAM; nothing is written). Expected: the loader's screen on the panel and
  a USB link `xl` can talk to. Platform match relies on the UEFI reporting 1080x2376 (PcdFrameBufferHeight).
- Risks to watch on the first boot: the UEFI never reaches the loader (fastboot logo / back to bootloader);
  loader picks the OnePlus 8 platform or none (the screen's first lines name the platform); USB link missing.
  CrashDump: hold Power + Volume Up. A cable left in after a reset can keep ABL's fastboot off USB: unplug,
  enter the mode, replug.

---

# Addendum 4: why the UEFI crashes (2026-10-26) -- ROOT CAUSE FOUND

Method that worked: DEBUG UEFI (debug text is drawn into the boot framebuffer by FrameBufferSerialPortLib), phone
films/reports the screen, and `tools/sahara_dump.py` reads RAM over the CrashDump USB port
(05c6:900e, Sahara memory-debug; the table also lists KMSG.txt, RST_STAT, PMIC_PON, CD_STRCT...). The framebuffer
at 0x9C000000 holds the last text. Bisect before that: stub-only (spin), shim-copy FD, shim-copy payload all PASS
(no CrashDump); the OnePlus 8's own UEFI also crashes with this phone's DTB; USB driver set is irrelevant.

Screen text at the crash (framebuffer dump, `op8pro-data/dumps/dump3/fb_text_bottom.png`):
  Loading driver ... DiskIoDxe, PartitionDxe, EnglishDxe, SdccDxe, **UFSDxe.efi**
  ufs_error_log: 28b
  ASSERT_EFI_ERROR (Status = Device Error)
  ERROR: C90000002:V03000007 IO 0D35CD8E-97EA-4F9A-96AF-0F0D89F76567   (= UFSDxe's FILE_GUID)
  ASSERT UFS.c +1004: !EFI_ERROR (RETURN_STATUS 0x8000000000000007)

=> The baseline (OnePlus 8T/8) UFSDxe cannot initialise this phone's UFS (Samsung KLUEG8UHDB, boot LUN 2 under
build 00358). In DEBUG the ASSERT parks the CPU, the watchdog resets into CrashDump ~14 s later; in RELEASE the
assert is compiled out and it goes wrong silently (black screen). Retracted: an earlier misreading of the log ring at
0x9FFF7000 (that is the *stock* bootloader's ring, spam from its own ButtonsDxe poll in fastboot).
Test images (RAM-only): out/instantnoodlep-DEBUG-A.img = this phone's own UFSDxe (00358) from xbl_b;
out/instantnoodlep-DEBUG-B.img = no UFSDxe at all. Both DEBUG so the next failure is visible.
Mistakes noted: baseline UFSDxe is also what the OnePlus 8 uses (its firmware is 00295, boot LUN 1).

---

# Addendum 5: MILESTONE -- UEFI + HackiOS loader run on the OnePlus 8 Pro (2026-10-26 17:26)

`fastboot boot out/instantnoodlep-DEBUG-B.img` (RAM only, nothing written): after ~18 s the phone enumerates as USB
`xnuloader` (18d1:d00d) and stays up. `fastboot getvar product` = `xnuloader`. Loader log (`fastboot oem log` +
`get_staged`; saved as hackios-op8pro/logs/loader-8pro-1.log):

    == XNULOADER v3 ==   CurrentEL=1 MIDR=51df805e CNTFRQ=19200000
    display 1080 x 2376 at 9c000000
    platform instantnoodlep
    pon: power-on reason 21 ...        usb: started

Memory map as the loader sees it: 12 GB (11515 MB free), firmware carveouts reserved below 0xA0000000 as in the live DT.

What B changed vs the failing builds: **UFSDxe removed** (apriori + DXE lists of instantnoodlepPkg). Both the baseline and
this phone's own build-00358 UFSDxe assert `Device Error` in UFS.c when they re-initialise UFS after the stock
bootloader used it. Everything else (the 8's USB set, ButtonsDxe, the kebab baseline) runs.

TODO for the build tree: make "no UFSDxe" the committed state of instantnoodlepPkg (Include/APRIORI.inc, DXE.inc in
uefi/files; the Mu-Silicium copy was restored after the experiment) and build a RELEASE image from it. Installed
dual boot will need storage access without UEFI UFS (kernel's own UFS driver `ufs=2`, or a UFS bring-up that
tolerates the warm controller).

Next: (1) kernel build (theos + iPhoneOS16.5 SDK in the container; third-party downloads need approval), (2) iOS
images (`REST=1 userland/build.sh`, ~12 GB), (3) `xl ramdisk` + `xl run` with cpus=8 (xl needs llvm-objcopy for its
own image build: use the container or `brew install llvm`), (4) drivers one by one: S6SY791 touch, 120 Hz/MDP push,
GPU, audio, haptics, battery, Wi-Fi.
Notes: never run bare `xl enter`/`xl log` unless the profile is loaded under bash; xl rebuilds a boot image there and
needs llvm-objcopy; use `fastboot oem log`/`get_staged FILE` for read-only log access.

---

# Addendum 6: kernel and iOS images built (2026-10-26) -- not yet run

Third-party fetches, all inside Docker (owner-approved): theos (clone) + `L1ghtmann/llvm-project` iOS toolchain
(aarch64) + `theos/sdks` iPhoneOS16.5.sdk -> `op8pro-data/theos/` (mounted only into containers); Apple XNU tag
`xnu-8796.121.2` and the other documented sources via `xnu/apply.sh` / `xnu-deps/setup.sh`.
Build happens in a Docker *volume* (`hk-ws`, case-sensitive Linux fs): a bind mount of this Mac's case-insensitive disk
would corrupt XNU's tree. Scripts: `docker/{build-kernel,rebuild-kernel,rebuild-incr,build-userland}.sh`,
image `hackios-kernel-builder` (Dockerfile.kernel). Fixes needed on the way: order stage1 -> apply -> stage2;
libncurses-dev (tcsh); **gawk** (mawk breaks XNU's makesyscalls: `\(` in init_sysent.c); a `timedatectl` stub (UTC) for
userland/build.sh. The Mac itself cannot build: no iPhoneOS SDK (Command Line Tools only).

Artifacts (host): `hackios-op8pro/out/kernel/mach.development.bcm2837` (15.9 MB; post-link `-k` errors are the documented
ones), `.unstripped`; `hackios-op8pro/userland/{root.apfs 4 MB, system.apfs 2.49 GB}` (iOS 16.5 20F66, REST=1, no GL,
no firmware: gpu/wifi/haptic/audio absent).

Safety finding (from this phone's live DT): bus map matches the OnePlus 8 for touch (SE13), gauge (SE16, 0x55) and
speakers (SE15), but the **alert-slider hall sensors are on SE1 here, while the 8's driver bit-bangs SE5 (gpio4/5, 0x18/0x19),
which is this phone's charger bus** (chgpump-charger 0x19, bq2597x, wireless-charge RX). `instantnoodle_slider_init()` now
returns on `instantnoodlep_board()`. Touch: S6SY791 here vs S6SY761: run with `touchscreen=0` until a driver exists.

First-run recipe (RAM only, owner must be told and watching):
    XL_KERNEL=out/kernel/mach.development.bcm2837   xl ramdisk userland/system.apfs   (2.5 GB into RAM, ~80 s)
    XL_SECS=600 XL_CMDLINE="-v cpus=8 bootcpu=7 touchscreen=0 slider=0" xl run
(loader board_args add fb_push=0 fb_hz=60). macOS lacks flock/llvm-objcopy that xl uses; drive `fastboot` directly or
install them.

---

# Addendum 7: MILESTONE -- iOS 16.5 HOME SCREEN ON THE ONEPLUS 8 PRO (2026-10-26 17:51)

RAM-only run, nothing written to the phone. Sequence: `fastboot boot out/instantnoodlep-boot-RELEASE.img` (UEFI without
UFSDxe + loader; loader up in ~8 s, `platform instantnoodlep`), `xl ramdisk userland/system.apfs` (2376 MB in 92 s),
`xl run` with kernel `out/kernel/mach.development.bcm2837` and
`XL_CMDLINE="-v cpus=8 bootcpu=7 touchscreen=0 slider=0"` (loader adds board=instantnoodlep fb_push=0 fb_hz=60). xl needs
macOS shims for flock and `stat -c` (scratchpad shims dir); run it under bash with `load_device instantnoodlep`.

Observed in the kernel log (`logs/ios-run1.live.log`): scheduler edge on 3 clusters, launchd ~100 s into a log with
hundreds of services, backboardd up (1125x2436 main display, apple-logo image loaded), SpringBoard spawned and building the
cover sheet. Framebuffer snapshot via `fastboot oem screenshot` + `get_staged` (`logs/ios-run1-screen1.png`): the iOS
home screen with widgets, dock and apps, clock 4:51, battery 100%.

Not working / not done: touch (no S6SY791 driver; touchscreen=0), panel rate (60 Hz, firmware autorefresh), GPU (software
rendering), audio, haptics, Wi-Fi, charging state, sensors; storage install (dual boot) not started.
Open: whether the panel shows the same picture (fb_push=0 means the firmware's scan-out of the framebuffer): owner to confirm.

---

# Addendum 8: TOUCH WORKS on the OnePlus 8 Pro (2026-10-26 ~18:33, owner-confirmed: "I opened Notes")

Change: `xnu/files/bsd/dev/instantnoodle_touch.c` -- panel size per board (the 8 Pro: 1440 x 3169, from the live DT's
`touchpanel,panel-coords = <0x59f 0xc60>`), log line names the variant; everything else is the OnePlus 8's S6SY761 driver
unchanged: bit-banged I2C on SE13 (gpio36/37) at 0x48, reset gpio38, IRQ gpio39 (TLMM summary interrupt), rails ldoc1 1.8 V and
ldoa13 3.008 V via RPMh, 8-byte sec_ts events. So the S6SY791 speaks the S6SY761's event protocol at least for touches.
Run: kernel `out/kernel/mach.development.bcm2837` (sha256 a44c3396...), `XL_CMDLINE="-v cpus=8 bootcpu=7 slider=0"` (no
touchscreen= argument), run log logs/182933-run.log; RAM only.

Notes:
- The driver's kernel printf lines (`touch: ...`) do not reach the loader's live/final log: the evidence is iOS reacting.
  If exact device-id/boot-status values are wanted, a log path is needed (e.g. hk_tl / loader_log_line).
- With -v the console text is drawn over the GUI during the first-boot CoreData/LaunchServices work; use XL_CMDLINE="cpus=8
  bootcpu=7 slider=0" (no -v) or wait a few minutes for a clean screen.
- A cable-in reset leaves the stock ABL without USB (off-mode charging): unplug, hold Power+VolDown, replug once in fastboot.
- Not yet checked: coordinate accuracy at the edges, multitouch, the panel's 120 Hz timing, wake from screen-off.

---

# Addendum 9: display probe, safety guards, and the GPU plan (2026-10-26 18:40)

`fb_probe=1` (read-only; MDSS registers + SMMU *global* registers) on the 8 Pro, `logs/ios-run3.live.log`:
- Same pipeline as the OnePlus 8: CTL0 -> VIG0 (0x5000) + VIG1 (0x7000), each 540 x 2376, stride 4320 (0x10e0), both fetching
  0x9c000000 -> LM0/LM1 -> DSC0+DSC1 (dual) -> INTF1, command mode, tear check on, **autorefresh on** (0x80000001), INTF1
  frame counter ~60-70/s: the panel runs timing@0 (1080x2376 @ 60 Hz). INTF_SEL=INTF1 only; no INTF2.
- **The display's apps-SMMU stream (id 0x820) is context bank 5 here (the OnePlus 8's: 4).** Never read that context bank or
  its tables (CrashDump). A second framebuffer at 0x9ca00000 inside cont_splash_region (0x2300000) is *assumed* mapped as on the 8:
  test `fb_push=1` (one buffer) before `fb_push=2`.
- Panel: Samsung ANA6706 ("AMS678UW01") behind a Pixelworks iris5 chip (ABL runs Iris_ResetExec); brightness DCS 0x51, max 0x7ff.
- This panel's own refresh switch (live DT, qcom,mdss-dsi-timing-switch-command, state dsi_hs_mode):
    timing@0 FHD 60 Hz : f0 5a5a | compression 01 | 60 02 | ba 02 | f7 0f | 2a 0000 0437 | 2b 0000 0947 | f0 a5a5
    timing@1 FHD 120 Hz: f0 5a5a | compression 01 | 60 22 | ba 02 | f7 0f | 2a 0000 0437 | 2b 0000 0947 | b9 01 | b0 01 | b9 0c 54 00 0b ... (read the whole property)
    timing@2 QHD 120 Hz: ... 60 20 | ba 03 | 2a 0000 059f | 2b 0000 0c5f ;  timing@3 QHD 60 Hz: ... 60 00
  The OnePlus 8's k60Hz/k90Hz tables are the ANA6705's and must NEVER be sent here.
Guards added (kernel MiniIOMobileFramebuffer.cpp, rebuilt: out/kernel/..., sha256 e94e9ec9...):
  `instantnoodlep()`; `pushMode()` = 0 on this board unless `fb_push=` is on the command line; no 90 Hz default; `panelRate()`
  returns at once on this board. Loader image `out/instantnoodlep-boot-RELEASE.img` rebuilt without fb_push/fb_hz board args
  (sha256 1bd7f66d...); the phone's resident loader still has the old one until the next `fastboot boot`.

GPU plan (owner asked for GPU + Metal, to make the phone responsive):
  1. firmware from this phone's Android: /vendor/firmware/a650_{gmu.bin,sqe.fw,zap.*} -> fw-gpu/ (adb pull, read-only);
  2. Mesa 25.0.7 via tools/mk-mesa.sh (third-party fetch: needs approval), built for iOS with the theos toolchain in the container;
  3. `GL=1 REST=1 userland/build.sh` image, run with `gpu=1` (+ `fb_push`, `fb_gpu` for GPU scaling) -- the kernel GPU driver has
     only been verified on the OnePlus 8's TZ (00295); this phone's TZ is 00358 (zap-shader load via PAS is the risk: CrashDump);
  4. Metal (`metal=1/2`) is experimental upstream; try only after GL works. "Fully implement Metal" is not a realistic goal.

---

# Addendum 10: Mesa, GPU firmware and GL images ready (2026-10-26 ~18:50) -- GPU not yet started

- Mesa 25.0.7 (pinned, sha256-checked) built in the container: `xnu-deps/build/mesa/out/libOSMesa.8.dylib` 14.8 MB, freedreno +
  softpipe, 1296 gl* functions (volume hk-ws; script docker/build-mesa.sh). AIR/Metal pieces (`AIR=1`) NOT built.
- GPU firmware pulled read-only from the phone's /vendor/firmware (LineageOS): a650_gmu.bin 41548, a650_sqe.fw 31632,
  a650_zap.{b00,b01,b02,mdt,elf}; sizes match the phone; in hackios-op8pro/fw-gpu (git-ignored, never committed).
- This GPU: "Adreno650v3", chipid 0x6050002 (= the driver's kona value), six power levels 587/525/490/441.6/400/305 MHz.
- `GL=1 REST=1 userland/build.sh` in the container with fw-gpu: root.apfs 10.8 MB (Mesa + gl libs + 7 firmware files);
  system.apfs unchanged (2.49 GB). Copied to hackios-op8pro/userland/.
- Driver facts (docs/subsystems/gpu.md): `kona = instantnoodle_board()` (true here); staged bring-up `adreno=N` runs steps 1..N
  and stops, reads first (1-3 reads; 4-5 CX power; 11-12 TrustZone/zap check; 13-17 GMU; 18-21 3D core, zap load, CP; 22 test picture);
  the documented killer is TrustZone's PAS on a cold start (fixed upstream on the 8's TZ 00295; this phone's TZ is newer).
- Other places still match the exact board name "instantnoodle" and are therefore OFF on this phone: hk_power.c (system sleep),
  hk_buttons.c (power/volume keys). Not a safety problem; a feature gap.
Test ladder (each a RAM-only run, each announced to the owner): adreno=3 -> 5 -> 9 -> 12 -> 17 -> 21, then gpu=1; fb_push=1/2 and
GPU scaling (fb_gpu) only after gpu=1 works; Metal (metal=1/2) last.

---

# Addendum 11: GPU ladder -- rungs adreno=3 and adreno=9 PASSED on the OnePlus 8 Pro (2026-10-26 18:55-19:01)

Both RAM-only, kernel out/kernel/mach.development.bcm2837 (e94e9ec9...), loader image 1bd7f66d..., resident system.apfs; logs
hackios-op8pro/logs/185458-run.log (adreno=3), logs/gpu-run2.now.log (adreno=9 adreno_skip=0x1c00).
- Step 2: qfprom speed bin 0x10 -> "speed bin 0: an Adreno 650". RPMh has gfx.lvl/cx.lvl/mx.lvl (levels as the OnePlus 8's).
- Step 4: gpu_cx_gdsc on (cx_gdscr 0x222001 -> 0xf8222000, hw_ctrl 0x80001fe0): "CX is on".
- Step 5 clocks measured by the SoC counter: cxo 19200144 Hz, cx_gmu 199999224 Hz (200 MHz asked), gx_gmu 0 Hz (off, as it
  should be), ahb ~47-56 MHz ("the register bus of the GPU block has its clock").
- Steps 6-9: GMU registers readable (cm3_sysreset 1, rpmh_power_state 0xf), GPU SMMU global + context-bank-0 reads fine (cb0 tcr
  0x738e24e7, fsr 0). No FAILED, no hang, no CrashDump. iOS booted normally in software alongside.
Note: `xl run` may print an empty `result:` early when the loader is busy and stop following; the kernel keeps running to its
loader_return_after. Do not poll `fastboot` while `xl run` is starting (it then thinks the phone is not in the loader and
tries to rebuild a boot image).
Next rung: adreno=16 adreno_skip=0x1c00 (steps 13-16: GMU address space + SMMU context, GMU firmware loaded and STARTED, RPMh
connection, HFI queues). Then 17 (GMU asked for 3D core power), 21 (3D core, zap shader via TrustZone PAS: the documented crash
risk), then gpu=1.

---

# Addendum 12: GPU ladder -- adreno=16 PASSED (2026-10-26 19:03): the GMU firmware RUNS on the OnePlus 8 Pro

Run `adreno=16 adreno_skip=0x1c00`, RAM-only, log logs/gpu-run3.live.log (run log logs/190245-run.log). Steps 1-9 and 13-16 done, 0 FAILED:
- 13: GMU address space + SMMU context 1 (stream 5): the first writes to the SMMU, global ones through the hypervisor -- accepted.
- 14: a650_gmu.bin loaded (first 16K of code/data through the GMU's registers, the rest into memory we made).
- 15: "THE GMU'S FIRMWARE RUNS" (cm3_fw_init_result 0x100, hfi_ctrl_status 0x1).
- 16: pdc gpu_enable on, the GMU's RPMh sleep/wake program written (rscc seq_mem), "THE GMU'S MESSAGE QUEUES ARE UP"
  (hfi_version_info 0x20000001). The end-of-ladder teardown sends "slumber" (GMU did not answer -- expected with nothing asked of it;
  the system goes on starting). No hang, no CrashDump.
Next rung: adreno=17 (ask the GMU, over HFI, for the 3D core's power: GX domain and the gfx.lvl vote), then 21 (3D core context,
ring, microcode, ZAP SHADER through TrustZone PAS -- the documented crash risk -- then CP start), then gpu=1 and fb_gpu.

---

# Addendum 13: GPU ladder -- adreno=17 PASSED (2026-10-26 19:08): the 3D core has power and answers

`adreno=17 adreno_skip=0x1c00`, RAM-only, logs/gpu-run4.live.log. 0 FAILED. Step 17: tables (speeds, supply levels, bus votes) and
"start" sent to the GMU, "the GMU accepts the speed wanted" (525 MHz), the 3D core's clock **measured at 525001432 Hz**, GX powered by
the GMU over HFI, 3D core registers (0x3d00000) read for the first time: "THE 3D CORE HAS POWER AND ANSWERS". RPMh bus resource
addresses logged: SH0 0x50004, MC0 0x50000, ACV 0x5007c, CN0 0x50074 (differ from Linux's ACV/CN0 values; same as the 8's? not compared).
Remaining steps (driver table): 18 the 3D core's address space (ring + microcode a650_sqe.fw); 19 the ZAP SHADER (TrustZone PAS:
header first, then the image: the documented CrashDump risk on a newer TZ); 20 CP start; 21 first commands / out of secure mode; 22 a frame.
Plan: adreno=18 (no TZ), then 19 (zap), then 21, then gpu=1.

---

# Addendum 14: GPU ladder -- adreno=18 PASSED (2026-10-26 19:11)

`adreno=18 adreno_skip=0x1c00`: the 3D core's memory made (ring at 0x1000000, a650_sqe.fw 31632 bytes read from /dev/md1, a scratch page at
0x1200000), SMMU context 0 given the tables and the core's requests (stream 0) sent through it (SMR write 0x84010000 accepted by the
hypervisor), rbbm_status 0x1. 0 FAILED, alive 19+ s. Lesson (again): never start a run while the previous kernel's loader_return_after has
not expired -- `xl: upload failed` (harmless); wait for `fastboot getvar result` = "returned".
Next: adreno=19 = THE ZAP SHADER through TrustZone PAS (header first, then image). The documented CrashDump risk.

---

# Addendum 15: BUG FOUND BEFORE THE ZAP-SHADER RUNG -- hard-coded zap memory was the OnePlus 8's (2026-10-26 19:15)

`hk_adreno.c` had `ZAP_MEMORY = kona ? 0x8c51a000 : ...` = the OnePlus 8's `pil_gpu_region@8c51a000`. This phone's live DT has
`pil_gpu_region@8e21a000` (0x2000, removed-dma-pool, no-map; alias `pil_gpu_mem`; node `qcom,kgsl-hyp` pas-id 0x0d, firmware
`a650_zap`). On this phone 0x8c51a000 lies inside the firmware's `removed_region@80b00000` (0xcd00000): step 19 would have told
TrustZone to treat that memory as the zap shader's and written the image into it. Fixed: `pro = instantnoodlep_board()`;
`ZAP_MEMORY = kona ? (pro ? 0x8e21a000 : 0x8c51a000) : 0x8b71a000`. Kernel rebuilt (sha256 cf656230...). It is the only fixed
reserved-memory address in hk_adreno.c (the rest is SoC MMIO or memory we allocate; cmd_db@80860000 is the same on both).
Lesson for the whole port: every `reserved-memory` constant in drivers copied from the OnePlus 8 must be checked against this
phone's DT (audio/modem/Wi-Fi carveouts differ too: pil_wlan_fw_region@8e100000 here, etc.).

---

# Addendum 16: GPU ladder -- adreno=19 PASSED: TrustZone authenticated and started the zap shader (2026-10-26 19:16)

`adreno=19 adreno_skip=0x1c00`, kernel cf656230... (corrected ZAP_MEMORY 0x8e21a000), RAM-only, logs/gpu-run6.live.log. 0 FAILED, no CrashDump,
alive 20+ s. The sequence (PAS id 0xd), every call answered 0 by this phone's newer TrustZone:
  smc 0x42000201 (header/signature: a650_zap.mdt, 3 segments) -> 0; smc 0x42000206 (give up any old shader) -> 0 (safe after the
  header); smc 0x42000201 again -> 0; smc 0x42000202 (where the image goes: 0x8e21a000, 0x1000) -> 0; the 1676-byte a650_zap.b02 written at
  0x8e21a000 (the reserved memory read 00 00 00 00 before); smc 0x42000205 (authenticate and run) -> 0:
  "TRUSTZONE HAS AUTHENTICATED AND STARTED THE ZAP SHADER".
The documented CrashDump risk did not occur with the correct memory. Next: adreno=21 (steps 20-21: command processor start; first commands;
the 3D core out of secure mode), then 22 (a three-colour picture drawn by the GPU onto the panel framebuffer), then gpu=1.

---

# Addendum 17: GPU ladder -- adreno=21 PASSED: the Adreno 650 EXECUTES COMMANDS on the OnePlus 8 Pro (2026-10-26 19:19)

`adreno=21 adreno_skip=0x1c00`, kernel cf656230..., RAM-only, logs/gpu-run7.live.log. 0 FAILED, alive 20+ s, phone back in the loader.
- Step 20: the 3D core's registers written as Linux does (address mode 64, GBIF QSB side, QoS, hardware clock gating on: 49 registers, UCHE ranges),
  CP started with a650_sqe.fw: "THE 3D CORE TAKES COMMANDS".
- Step 21: the core commanded out of secure mode (it asks the zap shader), then to write a word into a page of ours: the page reads
  0x600dc0de -- "THE 3D CORE WRITES OUR MEMORY ON COMMAND". So this phone's TrustZone-run zap shader works and the GPU is usable.
Remaining: step 22 (a three-colour picture drawn by the GPU onto the boot framebuffer, i.e. visible on the panel), then `gpu=1` (the same steps 1-9, 13-21
kept running for the window server: HKGPU), then `GL=1` rendering by Mesa/freedreno, `fb_push`/`fb_gpu`, DCVS, and only then Metal.

---

# Addendum 18: GPU ladder -- adreno=22: the GPU drew a picture, verified in memory (2026-10-26 19:23)

`adreno=22 adreno_skip=0x1c00`, logs/gpu-run8.live.log, 0 FAILED, alive 20+ s. The 3D core was given 10 MB of memory (0x10000000..0x109f5fff) and a ring of 123
words and filled three bands; the driver read the pixels back: first pixel 00 80 ff ff (BGRA orange, as told), middle ff ff ff ff (white), last row ff 80 00 ff (blue);
the words the core writes when its caches are in memory matched what it was told (0xc0c0c0c, 0xd0e0d0e). Logged: "A PICTURE DRAWN BY THE GPU IS ON THE PANEL:
orange, white, blue from the top". (A later loader snapshot shows iOS's own Apple logo: iOS boots in software beside this test and draws over it.)
The whole GPU bring-up chain 1-22 is now verified on this phone. Next: gpu=1 (steps 1-9, 13-21 kept running for the window server through HKGPU) with the GL
image (root.apfs with Mesa + fw), then watch whether CoreAnimation draws on the GPU; fb_push/fb_gpu and DCVS after that.

---

# Addendum 19: MILESTONE -- iOS 16.5 rendered by the Adreno 650 (gpu=1) on the OnePlus 8 Pro (2026-10-26 19:26-19:31)

`XL_CMDLINE="cpus=8 bootcpu=7 slider=0 gpu=1"`, kernel cf656230..., GL image (root.apfs with Mesa 25.0.7 + a650 firmware), loader image 1bd7f66d..., RAM-only,
run log logs/gpu-run9.live.log. adreno steps 1-9 and 13-21 all done, 0 FAILED, 0 panics; userland: "hk-gl: an OpenGL ES 3 context for CoreAnimation, by Mesa,
off-screen (OSMesa): OpenGL ES 3.2 Mesa 25.0.7; FD650". Live GL stats: ~115-140 draws/frame, ~11.7-14.7 ms per frame at frame 2220-2260.
Observed in the loader's framebuffer snapshots: the iOS home screen at ~80 s with a mostly black wallpaper (partial orange arc), blank white widgets and a dark
translucent dock (early frame or GL renderer gaps: weather/calendar widget content missing; vibrancy/backdrop is the upstream's known gap, see docs/research/ios-userland.md);
at ~140 s the Settings app opening (owner touched), list drawn correctly. Touch + app launch + GPU all working together.
Still to do for "responsive": fb_push + fb_gpu (GPU scaling, tear-free push: display context bank 5 risk), DCVS/boost defaults, check the wallpaper/widget rendering
vs the software renderer, then Metal (experimental, `metal=1/2`, AIR=1 Mesa build not done).

---

# Addendum 20: remaining work order (owner: "go on with the rest"), and the display-push guard (2026-10-26 19:35)

Owner feedback: gpu=1 "felt pretty responsive, some missed frames, usable". Order: (1) display push + GPU scaling, (2) 120 Hz with THIS panel's commands,
(3) power/volume keys + sleep (hk_buttons.c, hk_power.c match "instantnoodle" exactly), (4) Metal (AIR=1 Mesa build, metal=1/2), (5) audio / haptics / Wi-Fi
(each: check every reserved-memory/firmware constant against this phone's DT first -- the zap-shader address bug), (6) dual-boot install (every flash write needs a fresh OK;
userdata is metadata-encrypted: the carve tool must work on /dev/block/dm-N and the UEFI storage needs a no-UFSDxe answer).
Guard added: on instantnoodlep NO panel DCS packet (brightness 0x51, display off/on 0x28/0x29) is sent unless `fb_dcs=1`: the DMA reads its page through the display
SMMU context bank 5 (never read), and the mapping of the whole cont_splash_region there is unverified. Kernel bff02b0e... (also: pushMode()=0 unless fb_push=, no 60/90 Hz packets).
First push test: gpu=1 fb_push=1 (one buffer: MDP registers + the boot framebuffer only), then fb_push=2 (second buffer 0x9ca00000 inside the splash region: the
unverified mapping), then fb_dcs=1, then 120 Hz.

---

# Addendum 21: fb_push=1 WORKS on the OnePlus 8 Pro (2026-10-26 19:35-19:39)

`gpu=1 fb_push=1`, kernel bff02b0e..., RAM-only, logs/push-run1.live.log. Log: "display: panelRate(60) skipped: not the OnePlus 8's panel" (guard worked), "autorefresh
0x80000001 off after 2800 us; each frame is pushed through CTL0 on the panel's TE (interrupt)", "the frame clock follows the panel's TE, measured 16666 us apart". No panic.
No panel DCS was sent (fb_dcs not given). Loader snapshot at ~80 s of uptime: the full GPU-rendered home screen (complete wallpaper, translucent dock, Calendar widget);
the Weather widget is blank white where the software renderer shows "No weather data" (small GL renderer gap). Next: fb_push=2 (second buffer in the splash region: the unverified SMMU mapping).

---

# Addendum 22: fb_push=2 + GPU scaling WORK on the OnePlus 8 Pro (2026-10-26 19:41-19:45) -- owner: "scrolling feels a bit smoother with fb_push=2"

`gpu=1 fb_push=2`, kernel bff02b0e..., RAM-only, logs/push-run2.live.log. "framebuffers 0x9c000000 and 0x9ca00000 as surfaces 4 and 5 for the GPU (rows of 4352 bytes; the pipes had
0x10e0)", autorefresh off, TE 16666 us. The second buffer 0x9ca00000 (inside cont_splash_region) is fetched by the MDP through the display SMMU without a fault: so the
SMMU maps at least both framebuffers; the DSI DMA page used by fb_dcs (0x9c9ff000, between them) is therefore very likely mapped too (not proven). No panic, GL frames 13-15 ms.
NOTE for screenshots: with fb_push=2 the loader reports `screen: 1080 2376 4352`; convert with the pitch from `fastboot getvar screen` (my first conversion used 4320 and showed diagonal
streaks: not a display fault). The widgets in the snapshot were mid-swipe. GPU-rendered home screen otherwise complete.
Next: fb_dcs=1 (brightness 0x51 and display off/on 0x28/0x29 through DSI DMA), then 120 Hz with this panel's own timing@1 commands.

---

# Addendum 23: fb_dcs=1 works (brightness) and the 120 Hz switch is written (2026-10-26 19:46-19:55)

Run `gpu=1 fb_push=2 fb_dcs=1` (logs/dcs-run1.live.log, no panic). Owner: "brightness seems to work in settings": the DCS 0x51 packet sent through DSI0's command DMA
(page 0x9c9ff000, via the display SMMU) reaches the panel through the Iris chip. So that page IS mapped and the Iris passes DCS through. Not yet exercised: display off/on
(0x28/0x29: needs iOS to lock the screen; the power key is not wired on this board yet).
120 Hz (kernel b83f7b67...): `panelRate()` for instantnoodlep sends THIS panel's own timing-switch commands from its live DT (qcom,mdss-dsi-timing-switch-command, dsi_hs_mode, same
DSI clock 0x3135a800 in both modes): 60 Hz = f0 5a5a | 07 compression 01 | 60 02 | ba 02 | f7 0f | 2a 0000 0437 | 2b 0000 0947 | f0 a5a5 (wait 6 ms);
120 Hz = ... 60 22 ... + b9 01 | b0 01 | b9 0c 54 00 0b | f0 a5a5 (wait 7 ms). dsiPacket() now also takes type 0x07 (DSC compression mode). Opt-in: `fb_hz=120` AND `fb_dcs=1` AND push
on; INTF1_TE_VSYNC count is scaled by 60/hz like the 8's 90 Hz path; the original is restored for the loader (gTeVsync) and when going back to 60.
Unknown: whether the Iris chip needs a mode change of its own at 120 Hz (the vendor driver has an iris path); if the picture breaks the recovery is a reboot (ABL re-inits the panel).

---

# Addendum 24: first 120 Hz attempt -- NO panel command was sent (my ordering bug), fixed (2026-10-26 19:54-19:58)

`gpu=1 fb_push=2 fb_dcs=1 fb_hz=120` (kernel b83f7b67...), logs/hz120-run1.live.log: the log shows "the frame clock follows the panel's TE, measured 8333 us apart" and
`ca_fps=120`, but NO "own switch ... commands sent" line. Cause: in the instantnoodlep branch of panelRate() INTF1_TE_VSYNC (the display engine's internal vsync count) was scaled by
60/120 BEFORE teRate() measured the TE: the counter then ran at twice the rate, the measurement read ~120 Hz, and the "already there" early return fired silently. The panel itself was
probably still at 60 Hz (not confirmed) while the display engine's clock ran at 120. Fixed: measure first (as the OnePlus 8's original), and the early return now logs "TE already N Hz,
nothing sent". Kernel 17279c59... Lesson: a silent return must always log; and a rate "measured" from a counter we just reprogrammed proves nothing.

CORRECTION to Addendum 24 (19:59): the reason no panel command was sent is that pushStart() only calls panelRate() for 60 and 90 (`if (refreshHz() == 90 || refreshHz() == 60)`);
with fb_hz=120 it was never called, and "the frame clock follows the panel's TE, measured 8333 us apart" is the CONFIGURED period (NSEC_PER_SEC / refreshHz()), not a measurement.
The ordering bug in my panelRate() branch was real too and is fixed. Call site now: `|| (refreshHz() == 120 && instantnoodlep())`. Kernel rebuilt (see out/kernel).

---

# Addendum 25: 120 Hz SWITCH WORKS on the OnePlus 8 Pro (2026-10-26 20:02) -- by log; the owner has not yet looked at the panel

`gpu=1 fb_push=2 fb_dcs=1 fb_hz=120`, kernel b8096d86..., logs/hz120-run3.live.log. "display: 120 Hz, this panel's own switch: 11 commands sent (ctrl 0x1f5 trig 0x4 dma 0x14000000);
TE 60 Hz before, 120 Hz after" -- a real measurement (taken before the internal vsync counter is rescaled). Framebuffer snapshot (pitch 4352) shows the complete home screen, no push errors,
GL frames 2.4-2.8 ms. NOT verified: that the PANEL shows a clean picture and really refreshes at 120 Hz (only the owner can see it; the TE counter says so). Three bugs on the way, all mine and now fixed:
(1) ordering in panelRate (scaled the vsync count before measuring), (2) the call site `pushStart()` only called panelRate() for 60/90 so 120 was never tried, (3) "TE measured" in the log
line "the frame clock follows the panel's TE, measured N us apart" is the configured period, not a measurement.
The panel keeps its rate across runs: ALWAYS pass an explicit fb_hz (60 or 120) with fb_dcs=1, otherwise the driver and the panel disagree. A reboot (ABL) resets it to 60.

---

# Addendum 26: 60 Hz switch-back and the key reader on the OnePlus 8 Pro (2026-10-26 20:05, unattended run)

Kernel ed67b9fe... (keys enabled for instantnoodlep: hk_buttons.c `kona_board()`), `gpu=1 fb_push=2 fb_dcs=1 fb_hz=60`, logs/keys-run1.live.log:
- "display: 60 Hz, this panel's own switch: 8 commands sent (...); TE 120 Hz before, 60 Hz after" -- the panel went back; with Addendum 25 the switch is verified in BOTH directions by
  the panel's own TE counter (the owner has still not looked at the panel at 120 Hz).
- buttons: PMIC arbiter 0x50010000 (512 channels); PM8150 power-on block on channel 13 (type 1, subtype 4, as on the OnePlus 8), GPIO6 channel 25, GPIO7 channel 26 (type 0x10 subtype 0x11);
  power status 0x4 = released; volume up (GPIO6): input, pull-up, level high = released; volume down (GPIO7): pull 4 = pull-DOWN, level high: "not an enabled input with a pull-up ... left alone"
  (the driver never writes the PMIC) -> the volume-down key is not read; 1191 bus reads, 8 us each, 1 dropped. Key presses not exercised (nobody at the phone). System sleep (hk_power.c) stays OFF here
  (board name check still "instantnoodle" only): it powers cores down and needs a human to wake it.

---

# Addendum 27: Metal on the OnePlus 8 Pro, ADSP address fix, user observations (2026-10-26 21:06)

**Metal (RAM-only, by log, kernel a3c39af3..., `gpu=1 fb_push=2 fb_dcs=1 fb_hz=60`):**
- `metal=1` (discovery bypassed; `metal_drv_test` adds the device itself): "Adreno 650 (chip 0x6050002, HK)", blit tests pass, 4-sample draw resolves, 0 of 420 device methods unanswered,
  the 274-function default.metallib loads, and Apple's own `clearFragment20` draw reproduces the softpipe reference words (2458616957 2930221209 3401825461 3873429713).
- `metal=2 metal_profile=1` (whole system through Metal): `metal_sync_test` in a fresh process: ALL PASSED -- fence, event order, shared event held until the CPU signals it, signal visible to
  listener, heap (size accounting, aliasable, texture), MSAA resolve into a shader-read/write-only texture (texel b g r a: 0 128 255 255). logs/metal3/sync-m2.log.
  NOT good yet: a framebuffer screenshot of the system under `metal=2` mid-scroll in Settings shows duplicated and misplaced rows ("General", "Control Center" repeated, tile fragments):
  the visual result is NOT clean. Not investigated yet (compare with the clean `gpu=1` picture; the owner is the judge of the panel).
- `metal_sync_test` under `metal=1` finds no device in ANY process ("device none" even 150 s after the boot): that mode bypasses discovery on purpose. Use `metal=2` for it.
- `metal_sync_test` now re-executes itself until a device exists (userland/src/metal_sync_test.m); `userland/rc.jobs.metalsync` runs it alone; `userland/rc.jobs.metal8pro` is the first-look list.
- Boot time: with `gpu=1` alone (no metal) the Apple logo stays up for 50+ s after the kernel starts and the home screen is there at ~100 s (screenshots every 10 s, logs/base/). The owner saw 2+ minutes
  during the Metal runs; the baseline is not fast either. Not yet timed under `metal=1`/`metal=2`.

**Constants checked against this phone's live DT:**
- ADSP memory: the 8's `pil_adsp_region@8fe00000` is NOT the Pro's: the Pro has `pil_adsp_region@91b00000` (0x2500000). Fixed in `hk_adsp.c` (`instantnoodlep_board()` -> 0x91b00000). Not yet run.
- Wi-Fi: Pro has `pil_wlan_fw_region@8e100000`, 1 MB; `hk_wifi.c` uses `WPSS_MEMORY 0x9ae00000` for the 8's DSP path (not checked on the Pro yet; the PCIe QCA6390 path is the one in use).
- Audio/haptics: the Pro has the same parts on the same buses as the 8 (2x TFA9874 at 2-0034/2-0035, FSA4480 at 2-0042, AW8697 at 6-005a), so the 8's drivers apply unchanged.
- Sleep: `hk_power.c` enables sleep only for board name "instantnoodle"; on the Pro it needs `syssleep=1` and a human to wake it. Not tried.

**Firmware staged (never committed; `fw-audio/`, from the backup `modem_b.img`):** `adsp.mdt`, `adsp.b02`..`b18` (15 MB). The vendor partition of slot b was located by reading the LP metadata of
the SECOND metadata slot (slot index 1 = b; the first slot's extents for `_b` point at stale data) and extracted as EROFS in the Docker container (scratchpad `vendor/`).

**Owner's observations:** the alert slider does not work (the driver skips the Pro on purpose: `instantnoodle_slider.c` returns for `instantnoodlep_board()`, and runs pass `slider=0`);
volume up works, volume down does not (GPIO7 is pull-down; the driver never writes the PMIC) -- consistent with Addendum 26.

---

# Addendum 28: audio DSP, haptics and the Wi-Fi radio on the OnePlus 8 Pro (2026-10-26 21:20, RAM-only, by log)

Kernel a3c39af3... (ADSP address fix), image `userland/root-wifi.apfs` (GL=1 REST=1 ADSP=1, fw-audio/fw-haptic/fw-wifi), `gpu=1 fb_push=2 fb_dcs=1 fb_hz=60 adsp=1 sound=2 haptic=5 wifi=1`.
- **Audio DSP:** "THE AUDIO DSP IS UP AND LINKED, its audio services up (APR), 4587 ms ... node 5, 8 services" with `pil_adsp_region@91b00000` (the Pro's own; the 8's 0x8fe00000 is wrong here).
  Firmware: `adsp.*` of the backed-up `modem_b.img` (Lineage's, 2024). `sound=2` (port + stream with a test tone, amplifiers OFF) ran silent; the SMMU context 3 / stream 0x1801 setup took on the Pro.
  NOT done: `sound=4 sound_amps=1` (AUDIBLE, the TFA9874s; `tfa98xx.cnt` is staged in fw-audio/ from `odm_b`/firmware -- on this phone it is in /odm/firmware, not /vendor/firmware). Needs the owner's OK and not at night.
- **Haptics:** "the chip at 0x5a is 0x97, an AW8697", OnePlus's configuration written, offset calibration done, Android's waveforms (`aw8697_haptic_170.bin`, from /odm/firmware) in the chip RAM read back as written,
  and 16+ taps and 8 vibrations played by iOS's events. By log only: nobody reported feeling them yet.
- **Wi-Fi:** WLAN_EN gpio20, PERST gpio79, WAKE gpio81 equal the 8's. PCIe 0 link up in 15 ms (5 GT/s x1), device 17cb:1101, BAR 0 at 0x60300000 (1 MB) -- the same as on the 8.
  Then "lx: THE DRIVER IS NOT BUILT IN: its object was not there when this kernel was built (tools/mk-ath11k.sh)". BLOCKED on that build, which downloads Linux 7.0.14 (150 MB, cdn.kernel.org)
  and FreeBSD 15.1 src.txz (253 MB, download.freebsd.org); needs the owner's OK.
  Firmware staged in fw-wifi/ (never committed): `amss.bin`, `m3.bin` (linux-firmware ath11k/QCA6390/hw2.0, downloaded from git.kernel.org with the owner's OK), `12wlan.b0c` = the data segment (56180 bytes
  at ELF offset 0x400) of `image/qca6390/bdwlan.e0e` of the modem image (this phone's Android log said "Europe PVT ... 11wlan.b0e"), `mac` (from persist; not printed). Unverified that this board data is accepted.
  `board-2.bin` (231980 bytes) was also fetched and is not used.
- Locating files on this phone: `super.img` has THREE metadata slots; slot index 1 is `_b` (the active one): `vendor_b` at sector 989184 (1060096 sectors), `odm_b` at 22036480 (172552 sectors); both EROFS.

---

# Addendum 29: slider, volume keys, Wi-Fi radio and speakers on the OnePlus 8 Pro (2026-10-26 21:50, RAM-only, owner present)

Kernel (out/kernel/mach.development.bcm2837, 16490824 bytes: ath11k linked in), image `userland/root-sound2.apfs`, `gpu=1 fb_push=2 fb_dcs=1 fb_hz=60 adsp=1 sound=4 sound_amps=1 sound_below=96 haptic=5 wifi=1`.
- **Alert slider WORKS** (owner: "alert slider is working fine"; log: IST8801 up 0x19 and down 0x18 answer, gain 34/33, positions "in the middle: silent / down: ring / up: Do Not Disturb" with the sensor
  readings). The driver had been kept off the Pro by my wrong reading of the bus names: the Pro's hall sensors are on i2c@984000 = qupv3_se1 (gpio4/5, the same pins the 8's driver bit-bangs); its
  chargers are on i2c@994000 = se5 (gpio12/13). `instantnoodle_slider.c` now runs on both boards. Mapping of position names to iOS's ring/silent switch is the 8's and was not checked against the
  physical slider's printed positions.
- **Volume down WORKS** (owner; log "7 presses"). The firmware leaves GPIO7 as an enabled input with a PULL-DOWN (reg 0xc642 = 4; level reads high always). The device tree wants bias-pull-up
  (gpio_keys, key_vol_down_default). `hk_buttons.c pin_input()` now makes ONE announced write on the Pro only: 0xc642 <- 0 (pull-up 30 uA) through `hk_spmi_write()` (owner-checked: the channel is
  written by system 0 = apps, as Android), read back; `volume_pull=0` disables it. Volume up needed nothing. Power key: 0 presses seen (not pressed during the run?) -- unverified.
- **Wi-Fi radio UP** ("THE RADIO IS UP, 1.7 s after the kernel began"; scans hear 4-12 access points). Needed: (1) the ath11k object (tools/mk-ath11k.sh in Docker; Linux 7.0.14 + FreeBSD 15.1 src, with the
  owner's OK) plus ONE missing function I wrote, `linuxkpi_ieee80211_get_tid` in wifi/lx/src/lx_80211.c; (2) linux-firmware's `amss.bin`/`m3.bin` (fw build 2021-11-04); (3) the board data as the WHOLE ELF
  `image/qca6390/bdwlan.e0e` of the modem image (named fw-wifi/12wlan.b0c): the raw data segment of it (first try) let the firmware drop its QRTR service right after the download. This phone's own
  Android log says "Europe PVT ... Downloading BDF" of the same family (type ELF). NOT done: joining a network (needs fw-wifi/network: name and passphrase, the owner's), the data path, sleep.
- **Speakers:** the first run said "sound: no /fw/tfa98xx.cnt": `hk_adsp_fw_file()` looked on the root only; fixed to look in the small image's fw/ first, and `userland/build.sh` (ADSP=1) now puts
  `tfa98xx.cnt`/`aw882xx_acf.bin` of ../fw-audio there. Second run: "the amplifiers are started ... iOS plays", peaks 58% of full scale (limiter -7.3 dB at most), 0 underflows, parks when idle and
  restarts. What the owner HEARD in the second run: pending (the first run had no amplifier config, so it was silent: owner "does not seem working").
- Run mechanics learned: a run's log is gone from the loader when the kernel returns: fetch it (`fastboot oem log`, `get_staged`) DURING the run (the first 5 s is stale from the previous run);
  `xl run` returns at once here ("result: " empty) and does not follow; sourcing common/lib.sh needs bash, not zsh; macOS needs the flock shim and llvm-objcopy on PATH.

---

# Addendum 30: Wi-Fi joined and Safari loaded a page on the OnePlus 8 Pro (2026-10-26 21:50, RAM-only, owner present)

Same run as Addendum 29 (logs/all/wifi-joined.log, kept). The owner joined their WPA2 network from iOS Settings (no fw-wifi/network was given) and loaded apple.com in Safari: the page loaded
(owner's report). Log: channel 40 (5200 MHz) at -55 dBm, WPA2/CCMP, "AUTHENTICATION accepts", "ASSOCIATION accepts (status 0)", keys given to the driver 35 ms later, "THE NETWORK IS JOINED:
frames can pass", block ack 64 frames, wifid sees rssi -55 dBm. The 8's Wi-Fi station code (wifi/lx) ran unchanged on the Pro with the Pro's own board data. Not measured: throughput, WPA3, roaming,
2.4 GHz, Wi-Fi across a system sleep, and the radio address used was this phone's own (fw-wifi/mac, never printed).
Speakers: the owner heard the iOS ringtones (second run), so the TFA9874 path works on the Pro with Android's own amplifier configuration.

---

# Addendum 31: the known-good booting configuration, and what is set aside (2026-10-26 23:05)

**Known-good (booted iOS with Wi-Fi joined, speakers, haptics, slider and both volume keys, owner-verified):**
- kernel `out/kernel/mach.development.bcm2837` = `.next` (sha256 d8e5ece8...: ath11k linked, ADSP address 0x91b00000, `hk_adsp_fw_file` looks in the small image, slider on the Pro, GPIO7 pull-up write);
- loader `xnuloader/loader.efi` = `.before-persist` (sha256 e4486cb2...);
- image `userland/root-sound2.apfs` (GL=1 REST=1 ADSP=1, fw-gpu/fw-audio/fw-haptic/fw-wifi with the whole-ELF board data) + `userland/system.apfs` uploaded with `xl ramdisk`;
- boot-args `cpus=8 bootcpu=7 gpu=1 fb_push=2 fb_dcs=1 fb_hz=60 adsp=1 sound=4 sound_amps=1 sound_below=96 haptic=5 wifi=1`.
**Set aside (not part of the baseline; kept as files):** the persistent data volume experiment `hk_data=3` (loader `xnuloader/loader.efi.hkdata-experiment`, kernel `out/kernel/mach.development.bcm2837.hkdata-experiment`,
source changes in loader.c and hk_ufs.c; it is inert unless `hk_data=3 hkdata=3` is on the command line; untested on the phone: the first tries failed for reasons found and fixed in the source -- io_submap is 8 MB so the kept
region lives inside the kernel window after the resident image now); dual boot (the UEFI's UFS driver cannot re-initialise this phone's UFS, see Addendum 5; `tools/userdata-carve.py --wipe`, `darwin-write.py` DARWIN_INITIAL,
`common/devices/instantnoodlep.conf` FLASH_* are prepared, nothing was written to the phone's flash at any time).
A run of the baseline needs, in this order after the phone is in stock fastboot: `xl enter` (builds the loader image), `xl ramdisk userland/system.apfs` (93 s; a loader that was reloaded has no resident image, and a kernel started
without one returns at once with a log cut off mid-line), then `xl run` with `XL_KERNEL`/`XL_RAMDISK`/`XL_CMDLINE` as above.

---

# Addendum 32: what the rest of the roadmap needs, from the live device tree (2026-10-26 23:20, no phone; census only, nothing run)

Committed: kernel repo `3279180` and userland repo `821401c` (branch instantnoodlep; the Pro's extracted UEFI driver blobs `uefi/files/Binaries/instantnoodlep/` and `Resources/DTBs/instantnoodlep.dtb` are deliberately NOT committed:
vendor files), top-level repo `fc4ea25` (plan notes + Docker scripts; whitelist .gitignore).

**Camera (T07).** The existing `hk_camera.c` (4264 lines) is the POCO X5 Pro's (SM7325): its clock controller (camcc-yupik), PM8008i regulators, CCI pins and CSI blocks are all that SoC's. The Pro has Titan 480
(`qcom,csid480`, `qcom,csid-lite480`, ife0/ife1/ife-lite0/1 at 0xacb4000..., six CSIPHYs at 0xac6a000..0xac74000 step 0x2000) and two CCIs (0xac4f000, 0xac50000). Five `qcom,cam-sensor` nodes (reg 0..4):
cci0: sensor@0 (csiphy 0, master 0, actuator+eeprom+OIS, MCLK0 = gpio 94 (0x5e), reset gpio 145 (0x91), custom gpio 26), sensor@1 (csiphy 1, master 1, actuator+eeprom, MCLK2 = gpio 95, reset gpio 144), sensor@4 (csiphy 4, master 1,
eeprom, MCLK3 = gpio 98, reset gpio 92, vdig gpio 74); cci1: sensor@3 (csiphy 2, actuator+eeprom+OIS), sensor@2. Supplies are RPMh/PMIC regulators (phandles 0x391 vio, 0x2c5 vana, 0x38b/0x38c/0x38e vdig, 0x251 cam_clk gdsc).
The sensors' own names, slave addresses and power sequences are NOT in the device tree: they are in Android's `/vendor/lib64/camera/com.qti.sensormodule.*.bin` (slot b's vendor_b, EROFS, extractable from the backup `super.img`
at sector 989184 of the second metadata slot). A camera is therefore a NEW driver (kona camcc clocks, CCI, regulators, CSIPHY/CSID480 bring-up) following the `cam=1..4` ladder of hk_camera.c; days, not hours, with
hardware at hand at every rung. Not started.

**Bluetooth.** The radio is the QCA6390's (DT: `qca,bt-reset-gpio` gpio 21, `qca,bt-sw-ctrl-gpio` gpio 124, vdd-aon/dig/rfa1/rfa2 supplies shared with Wi-Fi, `btfmslim_slave`). Android talks HCI to it over a
UART serial engine and loads firmware (Qualcomm "ROME" TLV): LOCATED in the backed-up `bluetooth_b.img` (1 MB, FAT: `image/htbtfw20.tlv` 205836 bytes, `htnv20.bin` 5815 bytes, `htbtfw20.ver`, plus the 10-series files); not in vendor_b's /vendor/firmware.
iOS's `bluetoothd` talks to Apple's IOKit Bluetooth transport (`IOBluetoothHCIController` family), which this port has no stand-in for: Settings shows Bluetooth "unavailable" because no such service exists. Needs
(1) a UART HCI transport + firmware download in the kernel, (2) an IOKit service of the shape bluetoothd expects (found by reverse engineering `bluetoothd`/`libBluetooth`). Not started; a research task of its own.

**Cellular (T06).** Unchanged from docs/TODO.md: the modem subsystem (PAS, `qcom,mss@4080000`) is not started and iOS's CommCenter wants Apple's baseband drivers (`AppleBasebandPCI`, ARI) that do not exist here; the
phone's modem speaks QMI/RMNet. A first rung is possible (start the MSS with the backed-up `modem_b.img` firmware the way hk_adsp.c starts the audio DSP, then read QMI services) but a working iOS-visible
cellular stack (SIM, network, data) needs a QMI-to-ARI translation layer: not feasible as an unattended task.

**Native resolution.** iOS runs as an iPhone X (1125x2436 @3x, `backboardd: size {1125,2436}`) and the panel is driven in its 1080x2376 FHD+ mode, GPU-scaled (fb_push=2). The panel's DT timings include QHD+
and FHD+ modes behind the Iris chip and dual DSC; driving the panel at QHD+ needs the mode's DSI/DSC/MDP configuration and a change of what iOS is told its display is. A display-path task with panel risk; not started.

**Sleep/power.** `hk_power.c` enables system sleep only for board "instantnoodle" by default; the Pro can use `syssleep=1` (the same boot-arg), plus `sleep_max=` for a timed wake. Not tried: it needs the phone, the power key
(0 presses seen so far: unverified on the Pro) and someone to rescue it if it does not wake.

**Dual boot.** Prepared (carve --wipe, darwin-write DARWIN_INITIAL, profile FLASH_*) but blocked: the UEFI cannot use the phone's UFS (UFSDxe asserts Device Error on re-initialisation, Addendum 5), so the loader has no flash access.
A fix needs the cause found in Qualcomm's closed UFSDxe (the Pro's own is `op8pro-data/uefi-extract/xbl_b/.../UFSDxe.efi`; its asserts are `!EFI_ERROR(EFI_DEVICE_ERROR)` calls in init, no line 1004 in this build) or
a UFS bring-up of ours that adopts the controller as ABL left it. Nothing was written to the phone's flash at any time.

---

# Addendum 33: Android's own view of the hardware, camera sensors identified, first Bluetooth steps (2026-10-27 01:00)

**Android log (`/dev/kmsg` via adb root on the LineageOS boot; saved in op8pro-data/android-logs/, not committed):** all five cameras probe: slot 0 slave 0x34 sensor id 0x689 (IMX689 main), slot 1 slave 0x20 id 0x586
(IMX586 ultra-wide), slot 2 slave 0x20 id 0x471 (IMX471 front), slot 3 slave 0x5a id 0x30d5 (Samsung S5K3M5, the 8 MP tele), slot 4 slave 0x6e id 0x5035 (GalaxyCore GC5035, 5 MP). Modem boots (baseband `Q_V1_P14`),
the SIM is present but PIN-locked (`gsm.sim.state PIN_REQUIRED`; the owner has given the PIN; not recorded here). ADSP loads at 0x91b00000 on Android too (confirms the fix of Addendum 27). Error-level noise is benign:
the PMIC haptics node (`qcom,haptics@c000`, unused: the phone has the AW8697), the absent WiGig radio (msm_11ad), the wireless-charge receiver's firmware retry, the unplugged DisplayPort audio.
**Sensor files (backup `super.img`, slot-b `odm_b`, EROFS, extracted to op8pro-data/cam-extract/, not committed):** `/odm/lib64/camera/com.qti.sensormodule.{semco_imx689,semco_imx689_MP,semco_imx586,semco_imx586_BG,truly_imx471,semco_s5k3m5,shine_gc5035}.bin`
("QTI Chromatix Header" files, the format hk_camera.c already parses), `com.qti_instantnoodlep.sensor.*.so`, and EEPROM/actuator libs in /vendor. Lineage's kernel source (LineageOS/android_kernel_oneplus_sm8250, branch lineage-22.1, sparse
clone in op8pro-data/lineage-kernel: techpack/camera, drivers/bluetooth, drivers/soc/qcom, the 19811 DTS; downloaded with the owner's OK) is the reference for the kona camera blocks. No camera code written: see Addendum 32.

**Bluetooth, first steps (RAM-only runs):**
1. iOS's `bluetoothd`, `BlueTool`, `bluetoothuserd`, `BluetoothUIService` are in `userland/mkjobs.py`'s DENY list ('bluetooth', 'BTServer'); `ALLOW=bluetooth` at image build lets them through (not made the default).
2. bluetoothd then said: `marconi-bt/bluetooth: Unable to match bluetooth service in device tree`, `Not a valid MAC address (all 00s)`, `No bluetooth on this device`, exit(0). Settings' "unavailable" is this.
3. The loader's device tree now has nodes `bluetooth` and `marconi-bt` (device_type bluetooth, `local-mac-address` 02:52:44:57:00:02, a locally administered stand-in) both under /arm-io and at the root (xnuloader/loader.c;
   kept as xnuloader/loader.efi.btnode / the committed source). With them bluetoothd STAYS RUNNING and spawns `BlueTool` (pid 54, then 99, 124...), which exits with status 1 each time ("dirty state").
4. BlueTool is Apple's Broadcom-only controller process: it matches IOKit classes `AppleBTMgmt`, `AppleBTHci`, `AppleBluetoothModule` (PCIe: `AppleConvergedTransport`) and knows only BCM4350/4355/4357/4377/4378 modules with
   their `.hcd`/`.bin` firmware. The phone's controller is Qualcomm's (QCA6390, ROME TLV firmware `htbtfw20.tlv` + `htnv20.bin` in the backed-up `bluetooth_b.img`). A working Bluetooth therefore needs an emulated Broadcom
   controller: kernel IOKit services `AppleBTHci`/`AppleBTMgmt` of the shape BlueTool uses, answering Broadcom's vendor commands and bridging standard HCI to the QCA6390 over its UART (GPIO 21 reset, 124 sw-ctrl; firmware download
   by the ROME protocol). Not started; the first thing to find is what BlueTool says before it exits (next run: job logs captured with OSLOG="... com.apple.BlueTool com.apple.bluetoothd").

---

# Addendum 34: Bluetooth stack progress, Metal=2 screenshots, status of the day's roadmap (2026-10-27 01:25, RAM-only, owner away)

**Bluetooth (logs/cyc/best.log, kernel .next, image with `ALLOW=bluetooth OSLOG="com.apple.backboardd com.apple.BlueTool com.apple.bluetoothd"`):**
- With the loader's `bluetooth`/`marconi-bt` device-tree nodes (xnuloader/loader.c, committed), `bluetoothd` runs: "marconi-bt: Got local-mac-address: 02:52:44:57:00:02", "HCI Transport is set to H4BC", it serves XPC
  (`kCBMsgIdLocalDeviceAddCallbacksMsg` from the MBFramework) and launchd keeps spawning `BlueTool`.
- `BlueTool` (the controller process) prints: platform hash fd079b29... (MobileGestalt kMGQProductHash; this hash IS in BlueTool's table, entry "BCM4357B1 ... Hercules" with a MUR (Murata) and a USI firmware), `chip = (null)`,
  `Manufacturer is -1` (MGCopyAnswer kMGQWifiVendor has no answer: BlueTool picks the Murata or USI file by it) -> `getFirmwarePosForPlatform fwPos = -1` -> `NO_FW_FOUND`, "No WiFi - Iteration 0/1", then "BlueTool timed out! Exiting...", exit(1), respawned.
- Next steps, in order: (1) give MobileGestalt a WifiVendor answer (an `inject_*.c` hook on MGCopyAnswer in BlueTool, or the device-tree property MG reads: not found by string search, MG keys are obfuscated); (2) BlueTool will then try to open its
  transport through IOKit `AppleBTHci`/`AppleBTMgmt`/`AppleBluetoothModule` and download Broadcom `.hcd` firmware, so (3) a kernel-side stand-in controller is needed: the QCA6390's UART HCI (ROME firmware from `bluetooth_b.img`) behind
  an emulated Broadcom front. Steps 2-3 are the multi-day part. The jobs stay DENY-listed by default (`ALLOW=bluetooth` for experiments); Settings still shows Bluetooth unavailable.

**Metal (`metal=2 metal_profile=1`, kernel .next, image root-sound2): three framebuffer screenshots at about 90, 120 and 150 s (logs/cyc2/) show a CLEAN home screen** (icons, widgets, dock; the Weather widget goes from a white placeholder to "No weather data").
The duplicated Settings rows of Addendum 27 were seen mid-scroll in one capture and did not reproduce here: possibly a capture taken between the GPU's scaling pass and the push, not a rendering fault. Unverified on the panel.

**Cellular:** the SIM is PIN-locked (owner supplied the PIN; to be entered in iOS, never written down). Nothing built: CommCenter is DENY-listed and needs Apple's baseband drivers, see Addendum 32.
**Camera:** sensors, slave addresses, sensor-module files and the Lineage kernel's camera techpack are in hand (Addendum 33); no driver code yet.

---

# Addendum 35: the camera driver's first two rungs on the OnePlus 8 Pro (2026-10-27 08:00, RAM-only, partly with the owner present)

New `xnu/files/bsd/dev/instantnoodle_camera.c` (registered in `xnu/hackios.patch`; the POCO driver hk_camera.c hands over on board=instantnoodlep). Facts from Lineage's `kona-oem-camera-instantnoodlep_t0.dtsi`, camcc-kona.c, gcc-kona.c, pinctrl-kona.c.
- **cam=1 (a look, reading only): WORKS, verified.** All five sensors' pins are plain inputs at rest; `gcc_camera_ahb_clk` (GCC 0xb008) is OFF after the boot chain, so the camcc is not read until it is on (a camcc read with it off is a bus error).
- **cam=2 (cam_slot=0 IMX689, 1 IMX586): the power-up works up to the I2C read, verified from logs.** In order: reset low; RPMh `ldof7` 1.8 V (I/O); analog switches GPIO 156 and 26; RPMh `ldof1` 1.104 V (digital); RPMh `smpa5` 2.04 V (the device
  tree's cam_v_custom1: shared with the Wi-Fi RF rail); RPMh `mmcx.lvl` level 64; GCC camera AHB clock on; the camcc answers; titan_top power domain on; MCLK0 at 19.2 MHz (branch `0x1`, running); GPIO 94 to cam_mclk; reset high; 10 ms; bus scan of the
  CCI0 master-0 pins (GPIO 101/102, bit-banged); chip ID. RESULT: the bus idles high but NO address answers (scan 0x08..0x77 found none, not even an EEPROM), so the module is not alive yet: not found why (the sensor module file's own
  power sequence is in Android's "Parameter Parser V2.0.0" format, which tools/camsensor.py does not read; the next things to compare are Android's real rail and pin states while the camera is open, and the order and delays).
- **Two bugs found on the way, fixed:** (1) `rpmh_entry()` (instantnoodle_touch.c) used `strlcpy` into 8 bytes: names of 8 characters, like `mmcx.lvl`, were cut to 7 and never found; (2) the first cam=2 power-down turned the GCC camera AHB clock off and released the MMCX vote
  right behind the titan_top collapse and the phone went to CrashDump (read from the dump: the log ring ended at "power down"). Now the collapse is waited for and the clock and the vote are left alone (`cam_restore=1` gives them back); the phone survived the reruns.
- **CrashDump recovery learned:** `tools/sahara_dump.py --reset` reads ranges and then sends Sahara RESET; it only works in a fresh Sahara session, which a `pyusb` device reset (`dev.reset()`) restores. The kernel's log ring is at 0xf7c00000 + (the loader prints "kernel returned: log at ...").
- Pins of slot 0 (confirmed from the Lineage DTS: the live-DT `gpios` property is NOT the pin list and the live-DT pinctrl phandle names are off by one block): MCLK0 gpio94, reset gpio145, avdd gpio156, avdd2 gpio26 (output-high), vaf gpio93; slot 1 (IMX586): MCLK1 gpio95? (rear_2 clock gpio95), reset gpio144, avdd gpio159.
  The table in the source carries slots 0 and 1 only; slots 2-4 are in the file's header comment.
- Capturing `say()` lines: they go to the loader's 32 KB log ring, which later drivers' output pushes out; snapshot every 3-4 s right after the line is printed (scratchpad runsnap.sh) or read the kernel's ring from a dump.

---

# Addendum 36: the Bluetooth hardware driver (design from Android's sources; written offline, built, NOT run on the phone)

New `xnu/files/bsd/dev/instantnoodle_bt.c` (registered in `xnu/hackios.patch`, called from `hk_input_init()` after `hk_adsp_init()`, gated to `instantnoodlep_board()`), boot-args `bt=1` (look, reading only) and `bt=2` (power on, engine looked at). Built into
`out/kernel/mach.development.bcm2837.bt` (build script `docker/rebuild-bt.sh`; 0 errors). Sources: Lineage `drivers/bluetooth/{bluetooth-power.c,hci_qca.c,btqca.c,btqca.h}`, `drivers/tty/serial/msm_geni_serial.c`, `include/linux/qcom-geni-se.h`,
`drivers/clk/qcom/gcc-kona.c`, `pinctrl-kona.c`, and this phone's live DT.

**The hardware.** One chip, the QCA6390 (Wi-Fi on PCIe 0, already driven by instantnoodle_wifi.c), Bluetooth on a UART: `qcom,qup_uart@998000`, compatible `qcom,msm-geni-serial-hs` = serial engine 6 of QUPv3 wrapper 0 (wrapper node `qcom,qupv3_0_geni_se@9c0000`, iommus
stream 0x5a3, fastmap: only a DMA path would need the SMMU; Android's driver uses the FIFO path for this port). Pins (all in the TLMM north tile, function 1 = `qup6`): GPIO 16 CTS, 17 RFR, 18 TX, 19 RX; the engine's interrupt SPI 0x25f and a wake-up on GPIO 19, wake byte 0xfd. Control: BT_EN =
GPIO 21 (`qca,bt-reset-gpio`): low 50 ms, high, 50 ms (bluetooth-power.c `bt_configure_gpios`); GPIO 124 (`qca,bt-sw-ctrl-gpio`) is an INPUT the controller drives high once it is up (Android reads it right after the 50 ms and complains when it is low). No `wl-reset-gpio`
property here, so no WLAN_EN interlock. Rails (all RPMh votes by cmd-db name): aon pm8150 S6 0.95 V `smpa6`, dig pm8009 S2 0.95 V `smpf2`, rfa1 pm8150 S5 1.9-2.04 V `smpa5`, rfa2 pm8150a S8 1.35 V `smpc8` (the same four Wi-Fi powers) and Bluetooth's own asd pm8150 L16
3.024-3.304 V `ldoa16` (10 mA). Clocks (GCC, kona offsets from gcc-kona.c): `gcc_qupv3_wrap_0_m_ahb_clk` halt 0x17004, `gcc_qupv3_wrap_0_s_ahb_clk` halt 0x17008, `gcc_qupv3_wrap0_s6_clk` halt 0x1772c (root generator cmd 0x17730), all three enabled by bits 6, 7, 16 of the APCS vote
register GCC+0x52008 (BRANCH_HALT_VOTED); halt bit 31 set = off.

**The UART engine without Linux's help.** No engine firmware is loaded by Android's kernel for this port: `geni_se_read_proto()` reads `GENI_FW_REVISION_RO` (0x68) bits 15:8 and the driver requires 2 (UART): the protocol firmware is put there by the boot chain (TrustZone's QUP firmware loader assigns engines),
so on our boot it should already be so; `bt=1` reads exactly that (protocol, version, `GENI_IF_FIFO_DISABLE_RO` 0x64, `SE_GENI_DMA_MODE_EN` 0x258, `SE_HW_PARAM_0/1` 0xe24/0xe28, status 0x40) once the three clocks run. UNKNOWN until `bt=1` is read on the phone: (a) whether the three clocks run at all on
our boot (ABL/UEFI may have left them off; then `bt=2` votes them); (b) the engine's protocol (the repo's notes say the engines' FIFO path is disabled on these phones for I2C: if `GENI_IF_FIFO_DISABLE_RO` says disabled for engine 6 the UART needs the engine's own DMA (SE_DMA_TX_PTR 0xc30.., behind the QUP wrapper's SMMU stream
0x5a3: a context to set up as the audio and Wi-Fi drivers do) instead of the FIFO; (c) the exact FIFO/watermark/stale-timeout/baud-divider sequence, taken from `msm_geni_serial_port_setup`/`set_termios` of msm_geni_serial.c when (a) and (b) are known: 8N1, hardware flow control (RFR/CTS), SE clock 100 MHz-class
source divided down (`GENI_SER_M_CLK_CFG` 0x48 = div << 4 | 1), RX stale count, 115200 first.

**The ROME protocol (btqca.c, hci_qca.c), what the next rung does over that UART.** H4 framing (01 command, 02 ACL, 04 event). (1) HCI_Reset 01 03 0c 00 at 115200; (2) read version: vendor command opcode 0xfc00, parameter 0x19: the answer (event 0xff) is `edl_event_hdr{cresp 0, rtype 2}` +
`rome_version{product_id u32, patch_ver u16, rome_ver u16, soc_id u32}`; (3) the patch TLV (`htbtfw20.tlv`, 205836 bytes: header type 1, length 205832; patch header total 205800, data 205764, format 1, download_mode 3, product 0x10, rom build 0x200, patch version 0x4509, entry 0) is sent WHOLE, header included, in
segments of 243 bytes as vendor command 0xfc00 with parameters {0x1e, segment length, data} (848 segments); with download_mode 3 (skip VSE+CC) only the last (or a short) segment is acknowledged (event 0xff `{cresp 0, rtype 4, result 0}`), the others are not; (4) 10 ms; (5) the NVM TLV (`htnv20.bin`, 5815 bytes, type 2,
51 tags) the same way, after Android's edits of tag 17 (HCI transport: byte 0 |= 0x80 inband sleep, byte 2 = baud code) and tag 27 (deep sleep, byte 0 |= 1) -- we would leave sleep off; (6) HCI_Reset; (7) baud: vendor command 01 48 fc 01 <code> (code 0 115200, 5 230400, 7 460800, 10 921600, 13 2000000, 14 3000000,
17 3200000), the controller changes 300 ms after, then the host follows. Local address: vendor command 0xfc0b {0x01, tag 0x02, 6 bytes} (the phone's is in persist/the NVM; not read). Firmware: `op8pro-data/bt-fw/` (extracted from the backed-up `bluetooth_b.img`, git-ignored, vendor files: never commit); the version file says `BTFM.HST.2.0.0.c3-00476-QCACHROMZ-1`.

**What iOS needs on top (not this file).** BlueTool wants the Broadcom controller (addendum 34); this driver is the hardware half of the stand-in: HCI over this UART with the ROME firmware loaded. The Broadcom face (the `AppleBTHci`/`AppleBTMgmt` IOKit services BlueTool opens, Broadcom vendor commands answered, HCD download accepted and
ignored) is a separate piece.

**Ladder and test.** `bt=1`: no write at all (the engine's registers are read only when its three clocks run). `bt=2`: powers the rails (refused when `wifi=` is also on: the rails and the vote register are the Wi-Fi driver's then), BT_EN low 50 ms / high / 50 ms, votes only the engine's clock bits that were off, gives pins 16-19 to qup6,
reads the engine again and the sw-ctrl line, then puts everything back (the four shared rails stay on, as the camera driver does with smpa5; ldoa16 is given back). Next rungs (not written): bt=3 engine setup and HCI_Reset/version read, bt=4 firmware download and baud change.
Test when the phone is back (loader up, iOS image resident, `wifi=` NOT in the arguments):
`KERNEL=out/kernel/mach.development.bcm2837.bt` with extra boot-args `bt=1 bt_after=8` first, then `bt=2 bt_after=8`; read the `bt:` lines (they are in the loader's 32 KB log ring: snapshot every 3-4 s right after, scratchpad runsnap.sh).

---

# Addendum 37: Android's real power sequences read from its module files; what the bus answers (2026-10-27 09:30, owner present, RAM-only)

- `tools/camsensor.py` now reads the phone's "Parameter Parser V2.0.0" sensor module files (all seven parse). IMX689 (slot 0, slave 0x34 8-bit = 0x1a): pwrup VANA(1ms) -> CUSTOM_REG1(0) -> VDIG(1) -> VIO(1) -> MCLK 19.2 MHz(1) -> RESET high(1);
  id register 0x16 = 0x689, 16-bit addresses. IMX586: VANA, REG1, REG2, GPIO1, VDIG, VIO, MCLK 19.2 MHz, RESET. S5K3M5 (id reg 0x0, 0x30d5): VANA, VDIG, VIO, MCLK 19.2, RESET. GC5035 (slave 0x6e, 8-bit register 0xf0, 0x5035):
  VIO, VDIG, VANA, MCLK 24 MHz, RESET. IMX471: VANA, VDIG, VIO, MCLK 19.2, RESET. Types: Qualcomm's cam_sensor_power_seq_type (6 = CUSTOM_REG1, 10 = CUSTOM_GPIO1).
- Android's regulator states with the REAR MAIN camera open (sysfs /sys/class/regulator, owner opened it): BOB 3.3 V (2 more users), pm8009_l1 1.104 V (2 users: sensor VDIG and the OIS), pm8009_l5 2.8 V (OIS VAF), pm8009_l7 1.8 V (3 users),
  pm8150_s5 2.04 V (custom1), titan_top_gdsc, ife_1_gdsc. (selfie camera: pm8009_l3 1.056 V and l7.) cam=2 now makes exactly these.
- Android's camera driver (techpack/camera cam_sensor_util.c): GPIO-backed supplies are the "reg gpio" of VANA/VDIG/VIO/VAF types; the request table's flags 0 = output initially LOW, so GPIO 26 (custom1) stays LOW for the IMX689 (its sequence has no
  CUSTOM_GPIO step) although its pinctrl state says output-high. The module's OIS (LC898128) and actuator are powered with the sensor (OIS: ldof1, ldof7, ldof5 2.8 V + VAF GPIO 93; actuator VAF = the BOB).
- RESULT on the phone (bit-banged CCI0 master 0, GPIO 101/102, scan 0x08-0x77 after reset high): with the VAF rail on, **0x0c (the actuator) and 0x24 (the OIS) ANSWER**: the bus, the pins, the clock domain and the supplies are right. The sensor (0x1a) and
  the module's EEPROM (0x50) still do not. Not yet tried: more delay after reset (Android waits 1 ms), MCLK check on the pin, the sensor at 400 kHz, powering the EEPROM/sensor through `cam_vaf` differently, reading GPIO levels on Android (no debugfs, no /dev/mem there).
  Open question for the next session: which line differs from Android for the sensor and EEPROM specifically (both sit on the module's other half: VANA/BOB + custom rail); a measurement of the rails is not possible from here, a different probe order (OIS first, then sensor) is cheap.
- Android-side tools used: `adb shell su -c "timeout 6 cat /dev/kmsg"` (the whole ring, camera lines CAM_INFO/CAM_ERR: probe results, ACQUIRE_DEV/START_DEV order, OIS power-up), sysfs regulator snapshots (scratch script regsnap.sh), `dumpsys media.camera`, Aperture started with
  `am start -n org.lineageos.aperture/.CameraLauncher`.

---

# Addendum 38: the first runs of the Bluetooth driver and a sixth camera probe (2026-10-27 10:00, RAM-only, owner present)

Kernel `out/kernel/mach.development.bcm2837.cam3` (camera + the Bluetooth fork's instantnoodle_bt.c). Both rungs read from the loader's log ring (snapshots every 3-4 s).
- **bt=1 (read-only look): works, verified.** Engine 6's GCC state after our boot chain: wrapper 0 master AHB clock RUNNING, slave AHB clock OFF (`0x80000002`), engine 6's clock OFF (`0x80004220`), vote register `0x063c807f`, engine 6's root generator `cmd 0x80000000 cfg 0x00100000`.
  Pins 16-19 (the UART) are plain inputs at rest (CTS level 1, RFR 0, TX pull-up 1, RX 0), BT_EN (pin 21) low, sw-ctrl (pin 124) 1 with Wi-Fi up. The driver left the engine's registers alone because its clocks are not all running.
- **bt=2 (needs no `wifi=`): ran, nothing crashed, everything put back.** RPMh rails smpa6 950, smpf2 950, smpa5 1900, smpc8 1350 (shared with Wi-Fi) and ldoa16 3300 mV were voted; BT_EN pulsed low 50 ms then high: **the controller's sw-ctrl line (pin 124) went to 1** (it was 0 with Wi-Fi off), i.e. the QCA6390 woke.
  The vote register bits `0x10080` were set: engine 6's clock came ON (`0x00004220`), but the wrapper's slave AHB clock stayed OFF (`0x80000002`), so the driver, by design, still did not read the engine ("the engine's clocks do not come on"). Open: whether the slave AHB clock needs a delay, another vote bit
  or the wrapper's other parent (read gcc-kona.c `qupv3_wrap_0_s_ahb_clk`: its halt register and vote bit against what the driver reads); the engine's registers (firmware protocol, FIFO mode, DMA mode) are still unread.
- **Camera, sixth probe:** the same run scanned the sensor's bus 15 ms and 115 ms after reset high, then 30 ms after a reset pulse with MCLK running: 0x0c (actuator) and 0x24 (OIS) every time, never 0x1a or 0x50.

---

# Addendum 39: Bluetooth UART engine read on the OnePlus 8 Pro (2026-10-27 10:40, RAM-only, owner present)

Fix: `gcc_qupv3_wrap_0_s_ahb_clk` has hardware clock gating (gcc-kona.c: halt/hwcg reg 0x17008, hwcg_bit 1, vote bit 7 of 0x52008): after the vote its status reads `0x80000002` ("off") while idle and it wakes at the first access, as Linux relies on. The driver's "all clocks running" test now accepts voted+gated (instantnoodle_bt.c).
`bt=2` (no `wifi=`) then read engine 6 (the BT UART, GPIO 16-19 function 1): **init cfg revision 0xa, firmware revision 0x203 = protocol 2 (UART), version 3; slave firmware 0x203; FIFO interface NOT disabled (0), DMA mode 0**, hardware parameters 0x20102864 0x20204800, main clock cfg 0x11, IOS 1.
So the serial engine already runs UART firmware, in FIFO mode: no GPI/DMA is needed, and no QUP firmware has to be loaded by us. Everything was put back (vote register 0x063c807f as found). **Next rung (bt=3):** the engine's clock rate (root generator cfg of 0x17730 for 7.37 MHz / 48 MHz sources: reference drivers/tty/serial/msm_geni_serial.c of the Lineage kernel,
not yet in the sparse checkout), the baud divider and UART configuration registers, then HCI Reset and the ROME version request to the QCA6390 at 115200 baud with hardware flow control; then the firmware download (design: Addendum 36).

---

# Addendum 40: the QCA6390 answers over the UART (bt=3, 2026-10-27 ~11:10, RAM-only, owner present)

Kernel `out/kernel/mach.development.bcm2837.bt3` (`bt=3 bt_after=20`, no `wifi=`). **Verified on the phone:** Bluetooth UART engine 6 set up as a polled FIFO UART from msm_geni_serial.c's register sequence; HCI_Reset (`01 03 0c 00`) was answered with Command Complete `04 0e 04 01 03 0c 00`, and the ROME version request (`01 00 fc 01 19`) with `04 0e 12 01 00 fc 00 | 19 0c 10 00 00 00 2b 0d 00 02 00 02 0a 40` = product id 0x10, patch version 0x0d2b, ROM version 0x0200, SoC id 0x400a0200 (btqca.c's layout; it matches the backup's firmware files: product 0x10, ROM 0x200; the file's patch is 0x4509, so the controller runs ROM firmware and wants the patch).
- **The engine's root generator is not in DFS mode here** (DFS command 0x8020, bit 0 clear): the boot chain left it on the 19.2 MHz crystal. bt=3 sets it to 7.3728 MHz (GPLL0 even, M/N 384/15625; GPLL0 locked at 600 MHz, user control 0x103) and puts the old values back. The engine's divider (GENI_SER_M/S_CLK_CFG = SER_CLK_EN | div << 4) then sets the baud.
- **The engine samples at 16x, not 32x:** the chip answered with divider 4 (7.3728 MHz / 4 / 16 = 115200), and at divider 2 (the Android driver's UART_OVERSAMPLING 32 formula for 115200) the reply was garbage (`60 00 60 00 00 fe`: it is 230400 real). The sweep's labels in the log are msm_geni_serial.c's formula, half the real rate. The other rates tried (3000000/921600/460800/2000000/3200000/230400 labels) got no answer at all (the controller's ROM is at 115200 real).
- **A crash, and its cause:** a run that wrote the engine's registers while its GENI_SER_M_CLK_CFG was 0 (left so by my own earlier run's `uart_stop`: RAM-only state outlives a run) went to CrashDump at the writes after GENI_CGC_CTRL. The serial clock must be on first; the code now does that. Recovery: the Sahara `--reset` did not work this time; the owner force-rebooted (Power+VolUp). The loader's log ring was dumped from CrashDump (`op8pro-data/dumps/btcrash3`, 0xf7cd0000) and showed the last "bt:" line.
- **Run procedure learned:** after `xl enter` the loader is `idle` with NO resident image; `xl run` then returns at once (the kernel has no root). `./xl ramdisk` (default userland/system.apfs, 93 s) first; `userland/root-sound2.apfs` is only the payload ramdisk.
- **Next rungs:** (1) the firmware download as btqca.c does (htbtfw20.tlv in 243-byte segments, htnv20.bin, then HCI_Reset, baud change with 0xfc48) from the backup's bluetooth partition; the files are vendor firmware: they must go to the phone from `op8pro-data/bt-fw/` at run time (not committed, not in the kernel image: a loader payload or the ramdisk); (2) the iOS side (BlueTool wants a Broadcom controller: an IOKit AppleBTHci shim and MobileGestalt WifiVendor), which is the larger part.

---

# Addendum 41: the QCA6390's firmware downloaded over the UART (bt=4, 2026-10-27 ~12:00, RAM-only, owner present)

Kernel `out/kernel/mach.development.bcm2837.bt4` (`bt=4 bt_after=20`, no `wifi=`). **Verified on the phone:** the ROME patch (`htbtfw20.tlv`, 205836 bytes: 848 segments of 243) and NVM (`htnv20.bin`, 5815 bytes: 24 segments) from the phone's own backup were downloaded as btqca.c does it, every acknowledgement `04 0e 05 01 00 fc 00 1e` (command complete, status 0, request type echoed). **Afterwards the ROME version reads patch 0x4509 (before the download: 0x0d2b), ROM 0x200, SoC 0x400a0200**; HCI_Reset answers `04 0e 04 01 03 0c 00`; HCI_Read_Local_Version: HCI version 0x0b (Bluetooth 5.2), manufacturer 0x001d (Qualcomm), LMP subversion 0x4508; BD_ADDR is the NVM's unprogrammed default (not the phone's: not recorded). The firmware is runtime state of the chip: it is gone at BT_EN low.
- **What it took (twelve runs, the log of why):** (1) the version request's answer is a command complete `04 0e 12 01 00 fc 00 | 19 0c | product(4) patch(2) ROM(2) SoC(4)` (a newer dialect than btqca.c's, with a length byte); (2) a patch segment's/NVM segment's answer ends in the echoed request type 0x1e, which is not an error; (3) **the HCI_Reset must be sent right after the NVM at the rate of the download (115200 real)**: it makes the controller run the patch and take the NVM's baud rate (tag 17 byte 1 = 0x11 = 3200000 in btqca.h's table); only then does the host change to 3.2 Mbaud. Doing it the other way (host to 3.2 M, 8 s of silence, then a wake-up byte) got a firmware crash report (`04 ff fd 01 08 00 ...`, stack addresses 0x1000xxxx), and bytes sent at wrong rates made the controller flood 0xfd/0xfe (in-band sleep wake-up / sleep indications, hci_qca.c: 0xfd WAKE_IND, answered with 0xfc WAKE_ACK); (4) the engine's UART firmware samples at 16x (addendum 40), so the real rates are root-generator clock / divider / 16: 3.2 Mbaud = 51.2 MHz (GPLL0 even 300 MHz x 64/375), divider 1; (5) the serial clock's state outlives a run: a command left active by a previous run (status 0x41000) is cancelled/aborted with the clock on before the setup.
- **How the firmware reaches the phone without being in git:** `docker/rebuild-bt.sh` turns `op8pro-data/bt-fw/*` (mounted at /btfw) into the generated header `bsd/dev/instantnoodle_btfw.h` in the container's copy of the kernel tree; `instantnoodle_bt.c` includes it only if it exists (`__has_include`). The kernel image built that way holds vendor firmware (`out/kernel/*.bt4` is untracked: never publish it).
- **Next:** the iOS side. BlueTool/bluetoothd expect a Broadcom controller through IOKit AppleBTHci/AppleBTMgmt (HCI transport "H4BC"); the pieces are (a) a kernel HCI transport that keeps this UART up (interrupt-driven or a polled thread, IBS handled, the chip's wake/sleep), exposing /dev node(s) or an IOKit service the userland can open; (b) BlueTool's vendor commands (Broadcom's) answered or translated for a Qualcomm controller; (c) MobileGestalt's WifiVendor/BT answers. That is the larger project; the hardware is now proven.

---

# Addendum 42: the iOS side of Bluetooth: the plan, and its first step (bt=5, /dev/hk-bt) (2026-10-27 ~13:00, RAM-only, owner present)

**Step 1 done and verified on the phone: the controller as a packet device.** `bt=5` (= bt=4's firmware download, then the controller stays): a worker thread owns the UART engine; `/dev/hk-bt` takes and gives whole HCI packets in the H4 framing (type byte 01 command / 02 ACL / 03 SCO / 04 event; one packet a write, one a read, a read waits 2 s and returns 0 bytes when nothing came; one opener at a time). Receive is polled each millisecond, so the engine runs WITHOUT "RFR open" (the controller's flow control follows the FIFO watermark; the transmit side honours the controller's RTS: bt_cts defaults on at bt=5), which makes 3.2 Mbaud workable with a 16-byte FIFO. In-band sleep: a 0xfd from the controller is answered with 0xfc between packets; before the first packet the host sends 0xfd and waits for 0xfc; the host never sends 0xfe so the controller's receiver stays awake (power management is a later step). Self-test through the node's own calls: HCI_Reset, Read_Local_Version, Read_Buffer_Size (ACL 1024 bytes x 7, SCO 240 x 4), LE_Read_Buffer_Size (251 bytes x 16), Read_Local_Supported_Features, Read_Local_Supported_Commands, and **100 long events back to back: 100 whole 71-byte events, 0 bytes lost or mis-framed**; 7 of 7. Throughput is bounded by the 1 ms poll (16 bytes a millisecond at most): enough for control traffic and LE; Classic audio/A2DP would need the engine's interrupt or its DMA (a later rung, not needed to bring Bluetooth up in Settings).

**What iOS's own Bluetooth stack does (facts from `strings` of ios165/rootfs/usr/sbin/BlueTool, bluetoothd and addendum 34; the transport choice is NOT yet confirmed by disassembly: the REA tool's safety check failed repeatedly today).**
- `BlueTool` (the controller process) contains THREE transports: (1) its own **UART** one (`btuart_tran`, an H4 reader/writer; options `-d <device>` "open the specified UART device", `-b/-B` baud, `-s/-S` speed from the device tree, environment `BT_UART` and `BT_BAUDRATE` override the device and the rate; "Opening %s @ %d baud"; "BlueTool: ERROR -- unable to autobaud chip"; the IOKit name `com.apple.uart.bluetooth`); (2) **PCIe** through `/usr/lib/AppleConvergedTransport.dylib` (this is the one the iPhone X's BCM4357B1 "Hercules" uses: AppleBCMWLANBusInterfacePCIe, ACIPCInterface...); (3) **AppleBT**: IOKit user clients `AppleBTHci` ("AppleBT HCI device") and `AppleBTMgmt` (applebt_hci_connect / open / close / reset / exit_power_sleep via IOConnectCallMethod). It logs "DeviceTree transport = %d", "DeviceTree speed = %d" and reads the DT properties `transport-encoding` and `transport-speed`; the control nodes `/dev/btpoweroff`, `/dev/btreset`, `/dev/btwake` exist for power/reset/wake. bluetoothd printed "HCI Transport is set to H4BC" with our loader's `bluetooth` DT node (addendum 34).
- Its chip logic is Broadcom's: platform table keyed by MobileGestalt's product hash (this userland is an iPhone10,3: BCM4357B1 Hercules, Murata or USI), a firmware file chosen by MobileGestalt's WifiVendor answer (we have none: "Manufacturer is -1", `NO_FW_FOUND`), Broadcom `.hcd` patch RAM download (`/etc/bluetool/aci_boot.hcd`, `aci_init.hcd`, and per-module files), HCI vendor commands (OGF 0x3f). None of that can run on a Qualcomm controller unchanged.

**The design: a Broadcom-shaped front over /dev/hk-bt, in user space.** Two ways were weighed: (A) an IOKit stand-in (AppleBTHci/AppleBTMgmt user clients, IOKit C++ in our kernel: large, many selectors to guess); (B) **make BlueTool take its UART transport and put a translating daemon on the other end of a pty/tty**: BlueTool opens a tty (`BT_UART=/dev/ttys...`, speed from `BT_BAUDRATE`), a daemon `hkbtd` (C, our userland, a launchd-style job beside BlueTool) holds the other end, speaks H4 on it, and relays to `/dev/hk-bt`: all standard HCI passes through, **Broadcom's vendor commands are answered by the daemon without touching the Qualcomm controller** (Read_BD_ADDR ok, Update_UART_Baud_Rate (0xfc18) ok, Download_Minidriver (0xfc2e) / Write_RAM (0xfc4c) / Launch_RAM (0xfc4e) accepted and ignored: "HCD download succeeded", Read_Local_Version faked to a Broadcom identity if BlueTool insists, Write_BD_ADDR from the loader's `local-mac-address`), and unknown vendor opcodes get a Command Complete "unknown command" (status 1) to learn from. (B) needs only: a transport value in the device tree that picks the UART path (to be found: the `transport` property's values in BlueTool's table), `BT_UART` in the job's environment (our job runner sets environments; iOS files stay untouched), and the MobileGestalt WifiVendor answer (an `inject_*.c` hook on MGCopyAnswer in BlueTool, as the other hooks are done; the key is obfuscated: find it by the call BlueTool makes before "Manufacturer is -1"). If BlueTool's table says Hercules is PCIe-only, (B) needs the platform hash to be one whose entry is UART (the iPhone 7-era "BCM4350 UART 37.4 MHz ..." entries exist in the same table: faking ProductHash/WifiVendor to one of those is the other lever) or (A).

**The steps, smallest first (each testable on its own):**
1. DONE: `/dev/hk-bt` + worker + self-test (bt=5).
2. `hkbt_test` (userland, tiny): open /dev/hk-bt from iOS user space, HCI_Reset, Read_Local_Version, print: proves the node from user space (needs one small-image rebuild, ADSP-less; the image already has bt in `ALLOW=bluetooth` experiments).
3. Disassemble BlueTool's transport selection (REA: the function with "DeviceTree transport = %d"): which values pick UART, and what the UART path asks of the device (a path, termios, ioctls, `com.apple.uart.bluetooth` lookups), then the platform table's entry for this hash and its transport and firmware handling; decide (A) or (B) with facts.
4. `hkbtd` (the pty bridge + Broadcom stand-in): first only forwarding, with every vendor command logged, run under `BlueTool` by hand if the job runner allows (`BlueTool -d <tty>`), to see its dialogue.
5. MobileGestalt WifiVendor/BT answers (inject hook) and the DT `transport`/`transport-speed` values in xnuloader/loader.c.
6. bluetoothd up with a controller: Settings shows Bluetooth; scanning (LE advertisements first: nothing to pair).
7. Later: interrupt/DMA receive for Classic audio throughput, BT power management (the in-band sleep from the host side, BT_EN off with Settings' switch), the Wi-Fi/Bluetooth coexistence lines, HCI_Reset on resume.

**Step 2 done and verified: the controller from iOS user space (2026-10-27 ~14:00, RAM-only).** `userland/src/bt_test.c` (new, userland repo; built by build.sh into /hackios/bin/bt_test; job list `userland/rc.jobs.bt`; small image built in the container with `RC_JOBS=rc.jobs.bt` through `docker/build-userland-gl-adsp.sh`, copied to `userland/root-bt3.apfs`) opens `/dev/hk-bt` as an ordinary iOS process and does HCI_Reset (`04 0e 04 01 03 0c 00`, status 0), HCI_Read_Local_Version (HCI 0x0b = Bluetooth 5.2, manufacturer 0x1d = Qualcomm, LMP subversion 0x4508) and, with `scan`, a passive LE scan of 8 s: **463 advertising reports in 8 s** (RSSI -46 to -109 dBm; a first run saw 368): the radio, the antenna path and the receive path all work. The node appears 51-52 s after the kernel starts the firmware download (bt_after 20 s + about 30 s). Run: kernel `out/kernel/mach.development.bcm2837.bt5`, boot-args `bt=5 bt_after=20`, image `userland/root-bt3.apfs` (payload; the resident system image is the usual one).
- **Bug found by the first run, fixed:** the node was created before the kernel's own self-test, so bt_test and the self-test were two readers of one ring and took each other's answers (the reset answer was lost, 6 of 7 tests). The node is now created after the self-test; one reader at a time is the rule (bt_open refuses a second opener; the self-test is the kernel's own reader).
- **Step 3 done: BlueTool's transport selection** (disassembled by a separate agent with `otool`/`objdump`; REA's `open_binary` was unusable this session: stuck "cleanup_incomplete" on a phantom Hopper document, with either provider; full report with addresses: `hackios-op8pro/logs/bluetool-transport-analysis.md`, untracked). Findings, OBSERVED unless marked:
  - `get_devicetree_transport` (0x10003e384) / `get_devicetree_speed` (0x10003e22c): IORegistry service named **"marconi-bt", else "bluetooth"**, property `transport-encoding` / `transport-speed`, both 4-byte binary data (u32), searched under "IODeviceTree". **Dispatcher: 6 = AppleBT (IOKit AppleBTHci/AppleBTMgmt), 7 = PCIE (AppleConvergedTransport.dylib), anything else, including the "property missing" sentinel 8, = UART.** So our loader's `bluetooth`/`marconi-bt` nodes (no transport-encoding) already select the UART transport.
  - **The UART transport is not a tty.** Its open (0x10001dc34) is a Darwin kernel-control socket: `socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL)`, `ioctl(CTLIOCGINFO)` with the control NAME (default `com.apple.uart.bluetooth`; `-d <name>`, `-D` = env `BT_UART` else the default), `connect()`; the baud/configuration is a private get/setsockopt (opcode 0x40484813) at SYSPROTO_CONTROL level; getopt `hd:Ds:Sc`, default rate 115200, `-S` = the DT speed (0 -> 2,400,000). So **a pty is not a drop-in** (the agent says no tty call is used; CAVEAT from my own check: the binary also imports `open` and `cfsetspeed`, so some tty path may exist, e.g. for a path-like `-d`: UNVERIFIED). What replaces the pty: **our kernel registers a kernel control named `com.apple.uart.bluetooth`** (`ctl_register`, bsd/kern/kern_control.c is in our XNU; `ctl_enqueuedata` to BlueTool, `ctl_send` callback from it), carrying the H4 byte stream to and from the hk-bt rings, and answering the private baud/config sockopt with success. Then the Broadcom vendor commands can be answered in the kernel (or passed to a user daemon through /dev/hk-bt-side channel) as designed in step 4.
  - Platform table: entries registered through `register_platform(hash, chipName, vendor, ..., hcdFilename, flag)` (0x100014cf4); a second packed table (magic "BRCMcfgD" blobs in `__const`, 9.3 MB) was not resolved; whether the Hercules entry is PCIe-only is UNRESOLVED (the transport comes from the DT property above, not from the table, as far as seen).
  - WifiVendor: `MGCopyAnswer(CFSTR("WifiVendor"), NULL)` (plain key), a CFString, compared byte-wise against vendor names (murata/usi/tdk...) into a small integer ("Manufacturer is %d"; -1 when no match). Our MobileGestalt hook must answer a string containing one of them.
  - Launch: `com.apple.BlueTool.plist` runs `/usr/sbin/BlueTool -R` as `mobile` (Mach service com.apple.BlueTool), no transport arguments or environment; bluetoothd's "H4BC" does not come from the DT: it reads env `BT_HCI_TRANSPORT` (colon-separated names, 9-entry table at 0x1007fd3e8) else an int 0-7 from a plist (source not identified).
  - Dead ends: the .hcd download state machine (what BlueTool checks after "Launch RAM") was not traced; the plist key bluetoothd reads for its transport is unknown.
- **Next (step 4):** `ctl_register("com.apple.uart.bluetooth")` in instantnoodle_bt.c over the same rings as /dev/hk-bt, logging every packet BlueTool sends (and answering with Command Complete "unknown command" for Broadcom vendor opcodes), then the MobileGestalt WifiVendor hook; run with `ALLOW=bluetooth` and read BlueTool's dialogue from the log.

---

# Addendum 43: bluetoothd runs over the Qualcomm controller; iOS reports Bluetooth ON (bt=6 + BLUETOOTH=1, 2026-10-27 ~13:50, RAM-only, owner present)

**Verified on the phone: iOS's own `bluetoothd` drives the QCA6390 through the kernel control `com.apple.uart.bluetooth`, runs its whole HCI initialisation, and reports CoreBluetooth powered on (`Power state changed. PowerState:1 managerState:5`, `kCBMsgIdLocalDeviceStatusEvent` to Settings, mediaserverd, carkitd, wifi.manager, sharingd ...).** Kernel `out/kernel/mach.development.bcm2837.bt6` with `bt=6 bt_after=2`; image `userland/root-bt5.apfs` built in the container with `BLUETOOTH=1 OSLOG="com.apple.backboardd com.apple.bluetoothd com.hackios.bluetool"`.
- **The shortcut found in the upstream HackiOS author's repo** (`iphont-xnu-userland` c4379c2, 2026-10-27 03:16, the user pulled it; no kernel-side file came with it, the commit mentions a `hk_bt.c` that is not published): **bluetoothd talks HCI itself; BlueTool is only a boot helper** (bluetoothd asks the Mach service `com.apple.BlueTool` to run `RunBuiltinScript` init / boot / deepsleep). So no Broadcom emulation and no MobileGestalt WifiVendor hook are needed (my `inject_bluetool.c` idea is dropped). Their pieces, ported: `userland/src/btboot.c` (a job `com.hackios.bluetool` holds the Mach service name and answers the three scripts: init = release /dev/btpoweroff, deepsleep = hold it, boot = wait for sysctl `kern.hackios.bt` = "state up", here up to 90 s), `mkjobs.py` `BLUETOOTH=1` (lets bluetoothd, bluetoothuserd, BTServer in, keeps `com.apple.BlueTool` denied, sets bluetoothd's environment `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,3000000`), `src/inject_bt.c` (a library inside bluetoothd that patches private function-pointer tables at fixed 20F66 addresses, with UUID and instruction-word checks: the generic vendor descriptor, no Broadcom VSCs, no multi-advertising, the unsupported optional init step "Write Default Erroneous Data Reporting" and the periodic-advertiser query omitted), `src/bt_scan.m` (a CoreBluetooth discovery probe), `rc.jobs.btaudio`.
- **What my side added:** kernel `kern.hackios.bt` ("state off|starting|up|FAILED"; "starting" from the first moment so the helper waits) and `/dev/btpoweroff` (noted only); and the two bugs found with a watcher: (1) `inject_bt.c` checked `board=instantnoodle ` and refused ("instantnoodle board not confirmed") on `instantnoodlep`: now a prefix match; (2) **bluetoothd opens the control as a SOCK_STREAM socket** (`socket(PF_SYSTEM, SOCK_STREAM, SYSPROTO_CONTROL)`, `ioctl(CTLIOCGINFO)` by name, `connect`), and `connect` failed with EPROTOTYPE ("Protocol wrong type for socket") until the control was registered with `CTL_FLAG_REG_SOCK_STREAM`. The watcher (`bt-watch:` lines from inject_bt.c: open/socket/connect/ioctl/sockopt calls, 80 lines) stays in.
- **The dialogue (all standard HCI, the Qualcomm controller answers; Broadcom vendor opcodes get "unknown command"):** Reset, Read_Local_Version/Commands/BD_ADDR/Features, Read_Buffer_Size, LE features/states/buffer, Set_Event_Mask (+LE), Write_Local_Name "iOS", Write_Extended_Inquiry_Response, Write_Page/Inquiry scan, Write_Class_of_Device, LE_Set_Random_Address, LE accept-list/resolving-list commands, LE_Set_Scan_Parameters and LE_Set_Scan_Enable (scan running when the log ends), Write_Authenticated_Payload_Timeout. bluetoothd's socket options at control level: getsockopt 0x40487413 (72 bytes, read back zeros) and setsockopt 0x80487414 (72 bytes; first word 5, a 0x8b00 field: the line settings, accepted).
- **Not yet seen:** a paired device, Settings' Bluetooth page in the UI (the run was 4 minutes and unattended), Classic discovery, A2DP, the Bluetooth audio HAL plug-in (upstream copies Apple's `BTAudioHALPlugin.driver` for BLUETOOTH=1; not done here), power-off through /dev/btpoweroff (the controller stays on for the run), receive throughput (polled 1 ms).
- **Next:** a long interactive run with this image (default job list) for the owner to open Settings > Bluetooth and see nearby devices; then `bt_scan` for a CoreBluetooth discovery; pairing an accessory; BT audio (HAL plug-in + SCO/A2DP over the polled UART: probably needs the interrupt/DMA receive).

**Owner's test, same day (one-hour boot, kernel with event counters): Settings > Bluetooth lists the owner's other phone under Other Devices (REPORTED by the owner; first attempt, a 4-minute unattended-run, showed none).** The kernel's own counters agree: over the first ~80 s of scanning the controller sent about 4,000 LE advertising reports (event 0x3e, sub-event 2) and Classic inquiries completed 6 times (event 0x01) with Extended Inquiry Results (0x2f) among them; no ACL data yet; the in-band wake-ups (0xfd) answered 60 times; no drops, no bad bytes. Not yet tried: pairing, a connection (ACL in/out), Bluetooth audio, the Settings switch off/on (through /dev/btpoweroff, not acted on), receive throughput under load.


---

# Addendum 44: Bluetooth works: discovery, pairing and A2DP audio on a JBL speaker (owner-verified, 2026-10-27 ~13:00, RAM-only)

**The owner reports (REPORTED, not measured): Bluetooth sound now works through the JBL Charge 2+ ("Bluetooth sound now works"), after earlier tests of discovery (another phone listed), pairing with an Android phone, and A2DP signalling.** Bluetooth is considered done for now. What it took after addendum 43:
- Wi-Fi and Bluetooth together: `bt=5` and up run beside `wifi=1` (same rail votes, vote register by bits); `bt_after=45` so Wi-Fi's bring-up is over first. Both worked in the same boot.
- L2CAP/ACL logging showed the JBL connecting on SDP, RFCOMM, AVCTP and AVDTP (signalling and the media channel); bluetoothd published the A2DP device, but "unable to sendMsg due to no available xpc client connections": **Apple's `BTAudioHALPlugin.driver` (an AudioServerPlugIn) was not in the audio server's plug-in directory.** In a RAM boot that directory is the **system image's** `System/Library/Audio/Plug-Ins/HAL`; the first attempt put it in the small image's `/hackios/hal` only (the installed boot's). `build.sh` BLUETOOTH=1 now puts it in both (the system image's copy needs `rm -rf` first: `cp` onto the extracted tree's shared files fails). The system image was rebuilt (`userland/system-bt.apfs`, 2.49 GB) and uploaded (`xl ramdisk`, 93 s).
- The plug-in came from `ios165/rootfs.dmg` (`7z x -oios165/rootfs ios165/rootfs.dmg System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver`).
- Other Apple plug-ins not shipped (all audio: AirPlayHalogen + two clock drivers, AppleUSBAudio, CarPlayHalogen, BasebandVoice, AppleAOPAudioPlugin, OctaviaHalogen, RemoteInput bundles), plus placeholder apps' extensions and kext plug-ins; all other plug-in folders (265 of 302) are in the image.
- **Findings that stay open:** the local name shows "(null)" in Settings > Bluetooth even with the host name set at boot (bluetoothd had said "Failed to read local device hostname - using default iOS" and told the controller "iOS"); the paired JBL's device type is blank (owner chooses it by hand; likely an accessory database from MobileAsset, which is not available); the Android "Bluetooth share" photo cannot work (no Object Push in iOS); **Wi-Fi WPA3 failed**: `lx: SAE: NO CONFIRM from the access point` on an Android 16 hotspot (H2E, -41 dBm), WPA2 on the same hotspot joined after one or two tries (T18).
- Owner's earlier "YouTube: Sorry, there was an error licensing this video" was a cookies issue (not a bug).
- **Roadmap additions (owner):** finish camera support (T07), make the UI smooth (T37: many dropped frames; may be the RAM build), native panel resolution (T38, started next).

## Addendum 45: iOS display size (2026-10-27)
- Owner's decision: render as the iPhone closest to the panel's real QHD+ (1440x3168): iPhone 16 Pro Max, 1320x2868 @3x (about 1.25% stretch). Native 1080x2376 rendering was dropped.
- Native-size rendering showed UIKit's pink "rdar:45025538" status bar: the status bar picks its layout class by matching screen width/notch against known iPhones, else `_UIStatusBarVisualProvider_Fallback`. Changing the artwork subtype changes the whole layout class, so it is not used.
- `userland/src/inject_ui.c` wraps `+[_UIStatusBar visualProviderSubclassForScreen:visualProviderInfo:]`: UIKit's answer is kept; for the Fallback it answers Split1284/1242/1170/1125 by `_nativeDisplayBounds` width. First run with `fbwidth=1320 fbheight=2868`: clock and battery drawn, no pink bar (owner-visible screenshot `logs/res/x16a.png`, untracked). Wi-Fi radio up (`en0`) and Bluetooth scanning in the same run (the earlier display tests simply lacked `wifi=1 bt=6 bt_after=45`).
- Injection log lines of the library are written to `/dev/console`; they were not in the 1 MB ring retained at the snapshot. Not yet checked inside Settings/Safari.
- QHD+ panel mode (timing@2/@3 behind the Iris chip, dual DSC, MDP, 2 x 18.25 MB buffers in the splash region) not started: the panel stays FHD+.
- QHD+ scoping (live DT): DSI clock 825 MHz -> 1.457 GHz (PHY PLL), DSC 8 -> 10 bits per component with slices 720x96 (PPS changes), halves 540 -> 720 wide / 2376 -> 3168 high, Iris chip mode unknown. A multi-block display-path rewrite with CrashDump risk; no blind writes were made. Next: read-only PLL/DSC/INTF dump (see docs/TODO.md T38).
- QHD+ read-only dump (`fb_probe=2`, no crash): DSC0/1 identical, 1080x2376, slice 540x72, chunk 0x20e; DSI0 stream word count 0x439 x 2376 lines (QHD+ would be 0x5a1 x 3168); INTF1 timing registers zero (command mode); DSI0 PLL/PHY and DISPCC captured in `logs/res/probe2.log` (untracked). Details and next steps: docs/TODO.md T38.
- DSC derivation (`fb_dsc=2`): the VESA DSC 1.1 rules and the DT's numbers reproduce all 148 words of the firmware's FHD+ encoder registers (initial dec/xmit 526/512, scale 32/7/1789, nfl/slice bpg 347/362, offsets 6144/4336, flatness 3/12/2, rc 6/11/11/3/3, tables); the write of the same values read back clean and the picture was unaffected (owner-run). QHD+ sets and PPS are computed but unverified (10 bpc tables, 10 bpp offsets). Details: docs/TODO.md T38; logs `logs/res/dsc1.log` (untracked).
- First QHD+ attempt (`fb_qhd=1`, throwaway RAM run, owner-run): panel switch (timing@3) + PPS accepted, pipeline resized, but no TE and no frame (CTL flush pending, INTF1 frame counter static); restoring FHD+ (switch, PPS, saved registers) did not bring the panel back: black until the owner's force-reboot. The pattern was never seen. Unknowns: Iris chip mode, a panel reset/display-off-on cycle, PPS order, DSC values. See docs/TODO.md T38. Log `logs/res/qhd1.log` (untracked).

- QHD+ ladder (`fb_qhd=2`, 2026-10-27, owner present, RAM-only; Mac host `tools/macdev/`). **Stage 1** (panel switch to timing@3 + PPS type 0x0a, pipeline untouched, 10 s): TE (INTF1 read-pointer counter, `teRate()`) stays 60 Hz after every packet and through the hold; the panel does NOT lose its TE on the switch (the first attempt never measured it). **Stage 2** (plus every pipeline block resized to 1440 x 3168, nothing pushed): TE 60 Hz for 10 s, restore clean. So the first attempt's "no frame" is in the push, not the panel. **Stage 3** (`fb_qhd_flush=1`: flush mask also LM0/LM1 bits 6/7, DSC bit 22 + CTL_DSC_FLUSH 0x2188 = 3, INTF bit 31 + CTL_INTF_FLUSH 0x2110 = 2, offsets from Linux dpu_hw_ctl, CTL0 base 0x2000): the first push COMPLETES (PP0 done intr 0x100; the first attempt flushed only VIG0/VIG1/CTL, so the changed mixer/DSC/INTF registers never latched and the flush stayed pending) and the owner sees garbled snow = the panel's DSC decoder disagrees with the encoder. Later pushes in the loop did not complete (flush 0x804200c3 pending: the loop restarts a frame every 500 ms, not yet looked at). After that run the display path is stuck (DSI0 command DMA: "DSI packet 0 did not complete" in the next run, no reboot between) -> needs the owner's reset and "Developer mode" (loader reload, big image upload again).
- **The device tree holds the panel's own DSC sets** (qcom,mdss-dsi-on-command: DCS 0x9e + 88 bytes, now `kQhd60Pps`/`kQhd120Pps` in MiniIOMobileFramebuffer.cpp, decoded: timing@3 60 Hz 10 bpc 10 bpp slices 720x16: version 0x11, first-line bpg 9, nfl 1229, slice bpg 1494, offsets 5632/4332, initial xmit/dec 410/616, scale 25/465/14, rc limits 15/15, min qp[3] 6; timing@2 120 Hz 10 bpc 8 bpp slices 720x96: version 0x12, first-line 15, nfl 324, slice bpg 204, offsets 6144/4336, xmit/dec 512/706, scale 32/2402/10, limits 15/15, min qp[2] 5). My derived 10 bpc sets differed (first-line 12, initial offset 6144, limits 11, min qp[3] 5): the likely cause of the snow. FHD+ (timing@0) PPS: first-line 15/nfl 433 in the on-command vs 12/347 in the firmware's encoder (the panel must take the later dynamic type 0x0a PPS, Android logs "PPS not parsed from DTSI, generated dynamically"). Next run: `fb_qhd_pps=2` (encoders and type 0x0a from the panel's set) + `fb_qhd_9e=1` (also its DCS 0x9e command after `ba 03`).
- Mac build/run setup (this host is a Mac, not the NixOS box): Docker images `hackios-uefi-builder`/`hackios-kernel-builder` rebuilt from `docker`, Docker volumes `hk-ws` (kernel tree + build; main), `hk-ws-cam`, `hk-ws-pwr` (copies for the camera and power sessions), `theos` (toolchain, streamed in with tar: Docker's own mount of the SMB share is ~50 ms per file); `tools/macdev/kbuild WS TAG files...` streams changed files and builds incrementally (seconds); `tools/macdev/phone`/`phone-run` lock the one phone; xl needs `flock`/`timeout` stand-ins (`tools/macdev/shim`) and returns early on this Mac (`result:` empty while running), `phone-run` waits properly. The kernel built here has no ath11k (no Wi-Fi) until `tools/mk-ath11k.sh` is run in the container.

# Addendum 47: IMX689 ANSWERS -- the camera pin 156 was an eGPIO (2026-10-27 16:28, owner present, RAM-only)

**Verified on the phone (log `logs/cam12.live`, kernel `out/kernel/mach.development.bcm2837.cam12`, `cam=2 cam_after=10`): after the fix the bit-banged CCI0 master 0 scan finds 0x0c (actuator), 0x1a (the IMX689 sensor) and 0x24 (OIS), and the chip ID register 0x0016 reads 0x0689 as it should.** Before, 0x1a never answered.
- Cause: gpio 156 (the sensor's analog supply switch, Android's `cam_sensor_rear_0_avdd_active`) is an **eGPIO**: bit 11 of its TLMM config register reads 1 (the earlier probe logged `cfg 0xa00` for it and `0x200` for the plain pins), and the vendor pin driver (`pinctrl-msm.c` `msm_pinmux_set_mux`, `pinctrl-kona.c`: `egpio_present = 11`, `egpio_enable = 12`) sets bit 12 whenever it muxes such a pin; without it the application processor does not own the pin and an output never leaves 0 (it "read low although driven high" at every drive strength). `instantnoodle_i2c.c` `configure()` and `instantnoodle_gpio_function()` wrote the whole register and so cleared bit 12; they now set bit 12 when bit 11 is present (`egpio_enable()`); cfg after power-up is `0x1a00`, level 1. Shared code: every pin of this kind benefits (the same bug may have hidden other eGPIO pins).
- The module's EEPROM (0x50) is still not in the scan (it shares the sensor's supplies in Android's DT; probably another gate or address). The tile table in `instantnoodle_camera.c` has pin 78 (slot 2, front) in WEST; the vendor `pinctrl-kona.c` says NORTH: check before the front camera.
- Next: the other sensors' chip IDs (IMX586, S5K3M5, IMX471, GC5035) with the same fix; the EEPROM; then CSIPHY/CSID/IFE and the ISP in userland (long).
- **Host hazard (Mac):** `xl ramdisk`/`xl run`/`xl enter` call `enter()`, which when the phone is in the stock fastboot rebuilds `xnu-usb.img` WITHOUT the device profile (redwood's UEFI, 1.4 MB) and boots it: the phone hangs and the good image is overwritten. Happened once (a failed first `fastboot boot` of my own, then `xl ramdisk`). Use `tools/macdev/enter` (boots `out/xnu-usb-8pro.img`, rebuilt from the RELEASE image's own parts: shim 176 B + FD sha e763ceed + DTB + the loader payload, 2586624 bytes) and `tools/macdev/ramdisk`; never `xl enter`.
- QHD+ (T38) is aborted for now by the owner's decision; its probe code stays behind `fb_qhd=2` and is inert by default.

# Addendum 48: Camera.app launches (UI hook removed from it); the sensor probe kills the kernel in the full config (2026-10-27 16:30-17:35, owner partly present, RAM-only)

- **Camera icon:** the loader's `cam=5` already adds the `/arm-io/isp` node (MobileGestalt still-camera / rear-facing-camera), so SpringBoard shows Camera.app; on the 8 Pro the kernel treats `cam=5` as `cam=2` (`instantnoodlep_camera_init`: `cam_level = level > 2 ? 2 : level`), i.e. the sensor probe `cam_after` seconds after boot (default 20; `cam_after=3000` postpones it past the run).
- **Camera.app crashed at launch (SIGSEGV, respawn loop, 114 times in a few minutes). Cause found with `exctrace=1`** (the trace now also goes to the loader's own log, `user_exception_trace.c` `trace_say`, because the console is flooded): EXC_BAD_ACCESS, protection failure, address = sp - 0x20, on the guard page of a stack that ran out (pc CoreFoundation + 0x7e664, lr libsystem_malloc + 0x1aa74; `./iphont symbolize --slide 0x42ba8000 ...`): runaway recursion. **`libhackios-ui` (inject_ui.c) was injected into Camera**; without it Camera.app launches and stays up (owner-verified: "does not crash anymore"). `userland/build.sh` INJECT list no longer has Camera; for the run a repacked image `userland/root-ui3nocam.apfs` (7zz x of root-ui3.apfs, /etc/inject.conf edited, `tools/apfs/mkapfs.py -z --plain fw -o ... hackios=DIR`; same size as the original). Why inject_ui recurses in Camera: not looked into. **The viewfinder is black** (owner): the 8 Pro kernel has no `/dev/hk-camera` frame path yet (CSIPHY, CSID, IFE, the IMX689 init table, the ioctl API of `camera_device.c`); the Android sources for those are in `op8pro-data/lineage-kernel/techpack/camera/drivers/`.
- **Regression I caused and reverted:** my global "set eGPIO bit 12 when bit 11 is present" in `instantnoodle_i2c.c` `configure()`/`instantnoodle_gpio_function()` broke Bluetooth (bt=5 "no answer", TX stalled). The camera driver has its own `egpio_fix()` (the camera session wrote the same fix); the shared function is as before. And a Mac-built kernel needs the Bluetooth firmware header (`tools/macdev/kbuild` now generates `instantnoodle_btfw.h` from `/ws/btfw`, streamed from `op8pro-data/bt-fw`) and `ath11k.o` (`tools/mk-ath11k.sh` in the container, a minute) to be the full config: kernel `out/kernel/mach.development.bcm2837.cam15`/`cam16`/`cam17` are (Wi-Fi, Bluetooth verified up with cam15; GPU).
- **OPEN: the sensor probe (`cam=5 cam_slot=99 cam_after=N`, or `cam=2`) kills the kernel in the full config** (kernel returns to the loader at the moment the thread starts: after the first line, after pin 94, after the reset-pin write; with `adsp=1`, without it, probe at 8 s, 30 s, 100 s, 120 s; also with only `gpu=1 fb_push=2 ...`); in the minimal boot (`cpus=8 bootcpu=7 cam=2 cam_after=10`, no gpu) it works (chip ID 0x0689 read). The last printed line is not reliable (the log is lossy). **How to read the panic: after a run ends the loader holds the composed log (the kernel's whole log, panic text last) in its staging buffer; `fastboot get_staged FILE` at once, WITHOUT `oem log` (which recomposes the staging buffer from the loader's own log and caps it at 32 KB)**; `tools/macdev/logpoll.py` does that now (`OUT.final`). The last run did not return to the loader at all: the phone left USB (reset or power-off), the owner has to bring it back (stock fastboot).
- Upstream repos (`iphont-xnu-*`, `hackios-xnu-*`) have no OnePlus 8 camera driver beyond ours.
- (2026-10-27 ~17:55) **Fixed: the probe panic.** `pin_base` mapped a whole 1 MB TLMM tile (the kernel's I/O window is 8 MB and was full in the full config; `camera_thread -> pin_base -> io_map -> kmem_alloc` KERN_NO_SPACE); `instantnoodle_i2c.c` now maps 16 KB chunks (kernel `cam18`/`cam19`). **Full config, verified: IMX689 chip ID 0x0689 (0x0c, 0x1a, 0x24 on the bus), Bluetooth up.** The IMX586 (slot 1) probe ended in a CrashDump right after its first bus scan (EEPROM 0x50/0x54/0x58/0x5c answer on CCI master 1 = pins 103/104, the sensor 0x10 does not: PM8009 GPIO 1, smpa5, ldoc7 are not powered); `cam_slot=99` now skips slot 1. The IMX471 (slot 2): bus scan on master 1 shows the same 0x50.. EEPROMs, chip ID at 0x10 NO ANSWER; then CrashDump 0.3 s into its power-down.
- **CrashDump reader on this Mac:** `brew install libusb`, a venv with pyusb (`$SCRATCH/venv`), `DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib venv/bin/python3 tools/sahara_dump.py OUTDIR 0xf7d00000:0x200000 ...` (reads only; `logs/res/crashdump1/`): the kernel's log ring (physical 0xf7d00000.., timestamps `[ 281.495]`) is intact in RAM after a hard crash: `strings` it and sort by timestamp for the last lines.

# Addendum 49: camera frame path, rung A WORKS: the CSID's test pattern into DRAM on the OnePlus 8 Pro (2026-10-27 ~18:50, full config, owner present)

Spec: `hackios-op8pro/docs/research/camera-frame-path-sm8250.md` (written by a research subagent from Qualcomm's own driver for this phone and redwood's hk_camera.c). Code: `instantnoodle_camera.c` `fp_tpg()` (boot-arg `cam=3`, `cam_after=N`, `cam_step=N` to stop after step N; kernel `out/kernel/mach.development.bcm2837.cam25`). **Verified on the phone in the full config (Wi-Fi, Bluetooth, GPU, audio up):**
- camcc PLL0 locks at 1200 MHz (MX at 192, MMCX 64 from the sensor power-up), all RCGs update, every branch comes on; first reads: CPAS camera version 0x40800 (Titan 480, as the Android driver demands), IFE-lite 0 hw 0x10000001, CSID-lite 0 hw 0x20000001. (IFE-lite 0 at 0xacd9000, CSID-lite 0 at +0x200 of the same mapping; bus version 3: WM0 at 0x1c00, frame-done = BUS_IRQ_STATUS0 bit 4.)
- Memory: the kernel has NO contiguous memory below 4 GB (not even at the very start of the boot) and the apps SMMU faults unmatched streams (sCR0 0x200406: USFCFG), so the IFE-lite sees the buffer through a context of ours (free context 6, tables of ours, IOVA 0x40000000, ported from hk_camera.c smmu_up). **The hypervisor refuses one combined match register (stream 0x800 mask 0x441: reads back 0) but takes four single-stream ones: 0x800, 0x840, 0xc00, 0xc40 (the odd ones 0x801/0x841/0xc01/0xc41 refused); the write master's stream is among the accepted: "the SMMU has refused the IFE nothing".**
- Test pattern 1600x1200 RAW10 (CSID RX_CFG0 0x32103, RDI0_CFG0 0x82abf007, TPG VC 0xa): CSID counted 6554 packets, 0 ECC/CRC errors, **bytes 2400000/2400000**, IFE bus irq 0x11 (frame done + register update), buffer starts ff ff ff ff 00 ... (colour bars).
- Next (rung B): CSIPHY 0 C-PHY (the IMX689 is 3-trio C-PHY: spec 3.5), the sensor's `init` + a mode table (module has no 2x2 binned main stream: mode 4 = 2000x1128 4x4 binned), stream on 0x0100, then `/dev/hk-camera` for iOS. `tools/camsensor.py` has to emit the V2 mode tables (delegated).

## Addendum 52 (2026-10-27 evening): camera sessions merged, kernel `merged1`
- **Back camera:** IMX689 streaming in iOS (`/dev/hk-camera`, cam=5); the switch to the front camera and back works (`fpd_sensor_index`, cam31); flashlight (torch=3) verified by the owner. Autofocus: new `instantnoodle_lens.c`, an AKM AK7377 at 0x0c (reg 0x02 mode: 0x20 sleep, 0x40 standby, 0x00 active; 16-bit position word at reg 0x00 = 10-bit code << 6; code 0 = closest). iOS position = 1000 - code (0 furthest). Owner: autofocus finds focus but slowly, tap-to-focus did not: the sweep stopped at AF_FAR 768 (= code 232), so the close-up end was out of reach: AF_FAR is 1000 now (userland `camera_device.c`). OIS (LC898128) not touched: its Android driver rewrites the chip's flash.
- **Front camera:** the IMX471 answers (chip ID 0x0471 at 0x10) on CCI master 1 = GPIO **107/108** (103/104 and the 0x50..0x5c devices belong to the IMX586/GC5035 modules); four-lane D-PHY on CSIPHY 3 into the same CSID-lite/IFE-lite chain, mode `res1` 2328x1748 RAW10 30 fps, CSID 0 ECC/CRC errors, exact byte count; a real picture via `xl camera`. Slot 3 (S5K3M5) moved to GPIO 105/106, untested. If the front sensor does not stream, the main sensor's frames serve its place.
- **Userland (built on this Mac: `tools/macdev/ubuild`):** viewfinder ISP about 4x cheaper (decode and lens shading per cell row), frames paced to 30/60 fps; flash for photos (`FlashMode` ForceFlashOn/Auto lights the LED; key inferred from strings, unverified); `timing:` log lines. **Video recording cannot work as is:** no H.264 encoder (VideoToolbox: h263/jpeg only), software HEVC 1.4-2 fps; needs an own encoder behind VideoToolbox (docs/TODO.md T25).
- **Merged build:** kernel `out/kernel/mach.development.bcm2837.merged1` (tree `hk-ws-cam`, Wi-Fi/BT firmware built in; `bsd/dev/instantnoodle_lens.c` added to `xnu/hackios.patch` and `tools/macdev/kbuild`); small image `userland/root-merged1.apfs` (= root-ui3front + new libhackios-media.dylib); run: `tools/macdev/run-camera [SECS] [EXTRA ARGS]`. Not yet seen by the owner: front camera picture (orientation `turned=true`, colour: no tuning, Bayer BGGR guessed), flash during a photo, tap-to-focus with AF_FAR 1000.

## Addendum 53 (2026-10-27 night): dual boot, first measurement; tap-to-focus on the roadmap
- Owner test of `merged2`: front and back camera, autofocus, flash and the flashlight all work; **tap-to-focus does not** (docs/TODO.md T07 roadmap). The first run of `merged1` CrashDumped: Camera.app had taken SMMU context 4 / match registers 5.. before the sound driver's first claim, and the audio DSP gave up at the first sound ("audio_process"); the camera now starts above (context 6, match registers 7..: `FP_FIRST_CTX`, `FP_FIRST_MATCH`), the layout of every run that worked.
- **Dual boot, UFS (read only, kernel `ufs1`, `ufs_peek=1`):** the host controller at 0x1d84000 is warm and usable after the stock bootloader: CAP 0x1587031f, VER 0x300 (UFSHCI 3.0), HCE 1, HCS 0x10f, UTRLBA 0x9a131000 (running, doorbell 0), UTMRLBA 0x9a132000. UEFI's own UFSDxe fails because it re-initialises it; a driver that adopts it as it is (no reset, list swapped as hk_ufshc.c does) is the way. Plan and gates: docs/TODO.md T40. Nothing was written to flash.
- **Addendum 53, UFS DMA measurements (`ufs8=1..3`, read only):** list swap works; every DMA command ends in IS bit 17 with the descriptor not fetched, also with ABL's own slot/UCD/data buffer; clocks all on; the apps SMMU (hypervisor-owned: sCR0 and masked match-register writes are ignored) has no UFS stream entry; the controller is wedged after the first error and the phone CrashDumps. Next: single-stream bypass entries, then a full own bring-up (HC reset, device reset, QMP PHY, link startup); see docs/TODO.md T40.
- **Addendum 53, UFS SOLVED for reading (2026-10-27 night):** the DMA failure was the apps SMMU (UFS stream 0xe0 unmatched, USFCFG). A context of ours (camera-style tables, IOVA 0x40000000) with SMR 0xe0 -> context (the hypervisor takes translate S2CRs, not bypass), IOVAs in the lists, then START STOP UNIT (device unit ACTIVE, LUN 0 START) and the reads work: GPT of LUN 0 printed (userdata blocks 4133800..61753338, last partition, 219.8 GiB). Details and the next steps: docs/TODO.md T40. No flash write was done.

## Addendum 54 (2026-10-27 night): dual-boot partition made, iOS boots with its root on the flash
- userdata erased (owner: no backup, nothing on the phone) and shrunk with `tools/userdata-carve.py --wipe`: userdata 155.80 GiB, new `darwin` (sda24) the last 64 GiB of LUN 0; GPT written from Android (adb root, dd, read back identical), `fastboot erase userdata`, Android 14 booted with 166 GB. The old table: `op8pro-data/gpt-backup-20261027/`.
- darwin: the iOS root volume (rootfs.dmg's 3.apfs) at block 0, the OS cryptex at 1374208, the App cryptex at 2039808 (checksums compared after the write).
- `ufs_8pro=1` is in the profile's BOOTARGS and FLASH_BOOTARGS. The flash-root boot works in a RAM run (`xl ramdisk userland/flashboot.apfs`, payload `userland/root-flash1.apfs`, `root_apfs=ufs_darwin`): home screen up, owner-verified; mediaserverd crash loop to look at. Next: the write path (persistent /private/var), then a boot without the computer (the loader has no UFS: see docs/TODO.md T40).


- (2026-10-27 18:05) **The CrashDumps after the IMX586 and IMX471 probes (owner hands-off: not the app): stale `did_vaf` from the IMX689's power-up made the next sensor's power-down drive its "VAF pin" = 0, NORTH tile pin 0, not ours -> CrashDump** (read from the RAM: the kernel's log ring ends at "power down" of slot 2). `power_up` clears the `did_*` flags now and `power_down`/`show_control_pins` never touch pin 0 (kernel `cam20`).

# Addendum 51: /dev/hk-camera for iOS, the back camera works in the Camera app (2026-10-27 ~19:45, owner present)

`cam=5` on the 8 Pro (`instantnoodle_camera.c` `fpd_*`, kernel `cam31`): `/dev/hk-camera` is made at the start of the boot (ring of 3 frames in contiguous memory taken then), a server thread powers the camera and the frame path up on the first read (sensor `init`+`res4` over the bit-banged bus, now at the pins' own speed: `struct instantnoodle_i2c.fast`), streams IMX689 mode 4 (2000x1128 RAW10, BGGR guess) frame after frame into the ring through the SMMU context, serves `read()` (header + frame), `write()` exposure commands (a synthetic linear gain register, 256 = 1x, converted to Sony's code 1024 - 1024/gain; group hold), stops after 5 s without a read into a one-minute STANDBY (rails, clocks, SMMU stay; the next read re-arms CSID/IFE/PHY and streams at once), then down; leaves the last raw frame for the host at each stop (`xl camera`). **Owner-verified: the Camera app shows the back camera** (grainy, focus stuck close, viewfinder 3-5 s to load before the I2C speed-up and standby; faster after). Known gaps: autofocus (the lens driver at I2C 0x0c answers but is not DW9714: registers 0x02=0x20, 0x03=0x13, rest 0; the main module also has the LC898128 OIS at 0x24), the front camera (IMX471 does not answer at 0x10 on CCI master 1 even with Android's AVDD drive 16 mA + pull-up for gpio 24; only 0x50/0x54/0x58/0x5c answer; HK_CAMERA_SENSOR 2 is served with the main sensor's frames so that the app does not hang), flash for photos, video recording/saving and lag (userland: no compile environment on this Mac), colour (Bayer order, tuning: camtuning.py rejects the 8 Pro's tuning file), grain (4x4 binned 2000x1128 mode, no noise reduction).
- The perf session's finding: `cpufreq=1` (default on now in the kernel) and the owner's rule: every boot/flash command line carries `cpufreq=1` (+ `torch=3 cam=5` here): `common/devices/instantnoodlep.conf` BOOTARGS/FLASH_BOOTARGS; `tools/macdev/baseline.args` for the Mac run scripts.
