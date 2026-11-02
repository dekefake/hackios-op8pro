# UEFI for redwood and the OnePlus 8

Boot-chain facts (stock ABL, XNUP container, memory addresses): [../docs/research/boot-chain-and-recovery.md](../docs/research/boot-chain-and-recovery.md).

A port of [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) to the POCO X5 Pro 5G
(redwood). The device is not upstream; the port follows `lisa` (the same SoC package, `KodiakPkg`).

You do not need to rebuild the UEFI to run HackiOS -- a ready image is in `prebuilt/uefi`, and `mkxnuimg.py`
takes it by itself.

What is here:

- `BASE` -- the Mu-Silicium commit the port is made on;
- `mu-silicium.patch` -- changes to existing files:
  `BootShim/BootShim.S` (copies our "XNUP" container, which follows the UEFI image, to 0xF0000000),
  `BootManagerLib` (starts the XNU loader from the container instead of the boot menu);
- `files/` -- new files: `Platforms/Xiaomi/redwoodPkg` (memory map, driver list), and
  `Platforms/OnePlus/instantnoodlePkg` for the OnePlus 8 (its UEFI drivers come mostly from `kebab`, the OnePlus 8T
  on the same SoC: `files/Binaries/instantnoodle/README.md`; `uefi/export.sh` saves both packages),
  `Silicon/.../BootManagerLib/XnuLoader.[ch]`, `Resources/Configs/redwood.toml`, `Resources/DTBs/redwood.dtb`
  (the device tree from the phone's vendor_boot), `Binaries/redwood` (UEFI drivers from the firmware
  `OS1.0.3.0.UMSMIXM`, extracted with [UEFIReader](https://github.com/WOA-Project/UEFIReader) from `xbl.img`,
  with Silicium's five usual patches -- see `files/Binaries/redwood/README.md`).

## Building

Packages as in Mu-Silicium's `setup_env.sh` (on Arch: `git base-devel lld nasm clang llvm aarch64-linux-gnu-gcc
python python-pip python-virtualenv`).

```sh
uefi/apply.sh                                   # clones Mu-Silicium (with submodules, ~3 GB) and applies the port
python3 -m venv venv && venv/bin/pip install -r Mu-Silicium/pip-requirements.txt
cd Mu-Silicium
source ../venv/bin/activate
python3 build_uefi.py -d redwood -r RELEASE     # -> Build/redwoodPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
```

A pitfall: the `mu_nasm` dependency is installed through NuGet and needs Mono. To do without Mono, put the
system nasm in its place:

```sh
D=Mu_Basecore/BaseTools/Bin/mu_nasm_extdep
mkdir -p $D/Linux-x86-64
echo 'version: 20016.1.1' > $D/extdep_state.yaml
ln -s /usr/bin/nasm /usr/bin/ndisasm $D/Linux-x86-64/
```

After a build, `mkxnuimg.py` (called by `./xl enter` and `common/build.sh`) takes the UEFI from `Mu-Silicium/`,
and `tools/update-prebuilt.sh` refreshes `prebuilt/uefi`.

`uefi/export.sh` is the reverse: save the changes made in `Mu-Silicium/` back here.

A build from scratch by these instructions has not been re-run since (the tree was built along the way while
the port was made). For a port to another phone, see [../docs/PORTING.md](../docs/PORTING.md). Build with `-d instantnoodle` for the OnePlus 8.

## Building the UEFI in a container (the Mac setup, 2026-10-28)

The OnePlus 8 Pro's UEFI builds in a Linux container in about a minute once its sources are in a Docker volume; the image `hackios-uefi-builder` is `hackios-kernel-builder`
plus `apt install python3-venv python3-pip nasm acpica-tools uuid-dev build-essential git gcc-aarch64-linux-gnu mono-runtime`. Copy only what this phone needs into the volume
`hk-uefi` (`/w/src`; streaming the whole tree from a network share takes hours: the other devices' Binaries and the test/fuzz data of the third-party libraries are 90% of it):
`BootShim`, `Binaries/instantnoodlep`, `Binaries/generic`, `Platforms/OnePlus/instantnoodlepPkg`, `Resources/{Configs,Scripts,ramdisk,MuPatches,Pictures}`, `Resources/DTBs/instantnoodlep.dtb`,
`Conf`, `Silicium-ACPI`, `Silicon`, `Common`, `Mu_Basecore` (without `fuzz`, `test`, `tests`, `UnitTests`, `Docs`, `libspdm`), `build_uefi.py`, `pip-requirements.txt`. In the container:
`python3 -m venv /w/venv; . /w/venv/bin/activate; pip install -r pip-requirements.txt; mkdir -p Build; python3 build_uefi.py -d instantnoodlep -r RELEASE` (an existing `Build` skips stuart's setup).
The result is `Build/instantnoodlepPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd` (3 MB); `UEFI_FD=... tools/macdev/mk-standalone` puts it into a standalone boot image.

## The boot menu (volume up while the phone starts)

`BootManagerLib/HackiosMenu.c` replaces Mu's boot manager menu: iOS (USB debug, default), iOS (no USB debug), Android, Reboot to bootloader, Reboot to recovery, then the
platform's own entries (Internal Storage, USB Storage, ...), at 3 times the console font, centred. The choice among ours is the volatile UEFI variable `HackiosBootMode` (0..4) which the loader
reads (`xnuloader/loader.c`, `hackios_boot_mode()`).
