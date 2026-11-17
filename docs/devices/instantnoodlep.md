# OnePlus 8 Pro (instantnoodlep, IN2023) — Snapdragon 865 (SM8250, "kona")

The device this repository is about. Profile `common/devices/instantnoodlep.conf`, UEFI `uefi/files/Platforms/OnePlus/instantnoodlepPkg`,
loader platform `platform_instantnoodlep` (`xnuloader/loader.c`). Same SoC as the OnePlus 8, so most drivers are shared and gated on
`board=instantnoodle*` (files `instantnoodle_*.c`); what is specific to the Pro is listed below. The OnePlus 8's page,
[instantnoodle.md](instantnoodle.md), holds the shared details (Wi-Fi, audio, GPU, firmware extraction) and is the better first read.
The day-by-day record of this port is [../../INSTANTNOODLEP-PLAN.md](../../INSTANTNOODLEP-PLAN.md) (addenda).

Differences from the OnePlus 8: panel 1440 x 3168 (run at its FHD+ 1080 x 2376 mode, 120 Hz, DSC; iOS renders at 1290 x 2796 and the GPU scales it),
touch Samsung S6SY791, back camera IMX689 (+ AK7377 lens motor), Bluetooth and Wi-Fi on the QCA6390, an external X55 modem on PCIe 2, and a UEFI
**without a UFS driver** (the kernel has its own, `instantnoodle_ufs.c`).

## Status

Seen working on the phone itself (RAM runs, or the flashed standalone image where it says so). Evidence: [../TODO.md](../TODO.md), [../HANDOFF.md](../HANDOFF.md) section 0, [../KNOWN_ISSUES.md](../KNOWN_ISSUES.md).

| Feature | State |
|---|---|
| Boot to SpringBoard, 8 cores with `cpufreq=1` | works |
| Display 120 Hz (command panel, DSC, push path) | works; the native QHD+ mode does **not** (`fb_qhd`, left the panel black: do not use) |
| Adreno 650 for CoreAnimation, speakers, haptics, flashlight | works (firmware from the phone) |
| Wi-Fi | works in RAM runs; auto-join fixed via `hk_wifijoin` (K44 fixed) |
| Bluetooth | works in RAM runs: inquiry, pairing, A2DP audio (bt=1..6); bt=4 downloads QCA6390 firmware, bt=5 runs beside wifi=1, bt=6 registers kernel control com.apple.uart.bluetooth for BlueTool UART transport ([../subsystems/bluetooth.md](../subsystems/bluetooth.md)) |
| Back camera | works in Camera app (`cam=5`); IMX689 4000x3000 photos, 2000x1128 @ 30 fps video, H.264 recording via `h264enc`, telephoto S5K3M5 (sensor 3) streaming, orientation fixed |
| Front camera | works in Camera app (`cam=5`, sensor 2 IMX471); 2328x1748 2x2 binned preview, orientation handled |
| Cellular | LTE registration, SIM PIN, SMS send/receive, signal bars, operator name in flashed image ([../research/modem-sdx55.md](../research/modem-sdx55.md)); **voice calls work both ways with sound on loudspeaker**; **native IPv6 mobile data over `en1` with router discovery in kernel; mobile data in apps works** |
| Boot from flash, boot menu, persistent `/private/var` | works: UEFI in `boot_a`, iOS volume in a `darwin` partition cut from `userdata`, Android stays on slot B |
| System sleep | off by default (K45) — Wi-Fi radio does not come back after wake; modem link lost |
| Charging | 95% cap, voltage/temperature/time cut-offs, PMIC watchdog; input limit from Type-C advertised current; `adsp=1` required for charging |
| Sensors | SLPI + FastRPC enabled by default (`slpi=1` in loader 15); **accelerometer, gyro, rotation, compass, barometer work**; ALS (`als=1`) and proximity (`prox=1`) no longer crash backboardd; `slpi_dsplog=1` asks sensor DSP for its own log |
| Location | works via modem GNSS + `hk_location` feeding `locationd` ([../research/modem-sdx55.md](../research/modem-sdx55.md)) |
| Display features | Night Shift, True Tone, Auto-Brightness via colour matrices on the display; ALS service for MobileGestalt |
| Weather | Open-Meteo integration via WeatherDaemon v3 JSON model stand-in |
| App Store | reaches Apple (bag 200); walls are anisette, modem, FairPlay DRM ([../research/ios-userland.md](../research/ios-userland.md)) |
| FairPlay | research: app DRM breakable offline (fairplay-decrypt); credential may be extractable from owner's Mac; on-device FairPlay decryptor prototype (`fpdecrypt` target) |
| Not done | headphones/USB-C audio, fingerprint, fast charging, AirDrop, Metal for apps (experimental: `metal=2 metal_profile=1` runs system on Metal, ~293 of 456 system fragment shaders draw) |

## Building for this phone

Nothing of the phone's firmware, and no Apple file, is in this repository. You need: an unlocked bootloader, `adb`, `fastboot`, Docker (or the
toolchain in [../BUILD.md](../BUILD.md)), the iOS 16.5 IPSW (`ios165/fetch.sh`), and your phone's own partitions for the three things below.

1. **Firmware** (GPU, Wi-Fi, audio, haptics, Bluetooth): taken from the phone's vendor partition and `adb pull`, as in [instantnoodle.md](instantnoodle.md#setting-up-ram-only-run);
   the Bluetooth firmware is turned into a header by `docker/rebuild-bt.sh`.
2. **The UEFI** ([../../uefi/README.md](../../uefi/README.md)): `uefi/apply.sh` makes `../Mu-Silicium` and copies the OnePlus 8's driver set in as `Binaries/instantnoodlep`
   (they are the same files). The phone's device tree is cut from your phone's boot partition, which also pins it to your firmware:

   ```sh
   adb exec-out "su -c 'dd if=/dev/block/by-name/boot_b bs=4M'" > boot_b.img        # the active slot's boot partition
   uefi/mk-instantnoodlep-dtb.py boot_b.img ../Mu-Silicium/Resources/DTBs/instantnoodlep.dtb
   ```

   Then build it in the container (`docker/build-uefi.sh instantnoodlep`; the steps are in `uefi/README.md`).
3. **Kernel, userland, image:** `docker/` has the build scripts (`build-kernel.sh`, `build-userland*.sh`, `rebuild-*.sh`); `tools/macdev/` drives them from a Mac
   (`kbuild`, `ubuild`, `mk-standalone`), `tools/stubs/` has the bring-up stubs, and `common/run.sh instantnoodlep` / `common/build.sh instantnoodlep` are the front ends.
   The boot arguments the profile uses, and what each does: [../BOOT_ARGS.md](../BOOT_ARGS.md).

Test in RAM first (`fastboot boot`, nothing is written) and flash only an image that has booted; a wrong image on `boot_a` is recovered from the bootloader
([../INSTALL.md](../INSTALL.md), [../research/boot-chain-and-recovery.md](../research/boot-chain-and-recovery.md)). The CrashDump reader `tools/sahara_dump.py` reads
the phone's RAM when it crashes into Qualcomm's 05c6:900e mode. Back up every partition before you flash (`INSTANTNOODLEP-PLAN.md` describes a read-only way).