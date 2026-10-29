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
| Wi-Fi | works in RAM runs; does not join a saved network by itself (K44) |
| Bluetooth | works in RAM runs: inquiry, pairing, A2DP audio ([../subsystems/bluetooth.md](../subsystems/bluetooth.md)) |
| Back camera | works in the Camera app (`cam=5`); no autofocus control from iOS yet, no front camera, no video recording |
| Cellular | LTE registration, SIM PIN, SMS send, signal bars in the flashed image ([../research/modem-sdx55.md](../research/modem-sdx55.md)); no calls, mobile data, or SMS receive in Messages |
| Boot from flash, boot menu, persistent `/private/var` | works: UEFI in `boot_a`, iOS volume in a `darwin` partition cut from `userdata`, Android stays on slot B |
| System sleep | off by default (K45) |
| Not done | sensors, headphones/USB-C audio, fingerprint, location, fast charging, AirDrop, Metal for apps |

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
