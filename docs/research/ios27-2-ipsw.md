# iOS 27.2 (24B119, iPhone19,2 / 19,3 / 19,7): can it be RAM-booted on the OnePlus 8 Pro? No (2026-10-29)

Asked for: prepare `iPhone19,2,iPhone19,3,iPhone19,7_27.2_24B119_Restore.ipsw` (in the workspace root, 13.7 GB) to boot like iOS 16.5, keeping the 16.5 installation, **RAM boot only, abort if the RAM is too small**. Aborted before touching the phone. All facts below are **observed** in the IPSW (`unzip -l`, `BuildManifest.plist`) or in this repository, except where marked *inferred*.

## Why it stops at RAM
- The phone has **8 GB** of RAM ([../devices/instantnoodle.md](../devices/instantnoodle.md)); the Android log shows model IN2023.
- The IPSW's operating system image `141-68953-104.dmg.aea` (BuildManifest "OS") is **9,529,458,688 bytes** already compressed; the system cryptex `141-68931-108.dmg.aea` 2,415,919,104; `141-68945-108.dmg.aea` 553,648,128; `141-68940-107.dmg.aea` 260,046,848; the app cryptex `141-08824-244.dmg` 16,777,216 and the restore ramdisk `141-10085-240.dmg` 245,366,811. Together about **12.7 GB** compressed, before the kernelcache (24 MB), the firmware and anything decompressed. Even the OS image by itself does not fit in 8 GB.
- The iOS 16.5 port's RAM boot does not hold the system either: the root volume and both cryptexes are read from the `darwin` partition of the flash (T40), the RAM carries a small image. Doing that for 27.2 would need the volume decrypted, written to flash and room for it; the existing 64 GB `darwin` partition holds the 16.5 volume, which was to be kept.

## What else would stop it (not tried, so *inferred* unless said)
- **The images are Apple Encrypted Archives** (`.dmg.aea`, observed): the key is fetched from Apple's servers per file (`aea` / `ipsw` tooling); nothing in this workspace decrypts them.
- **arm64e.** The three product types are A19-class (chip `t8160` in the firmware names, observed). iOS 26 and later userland is arm64e (pointer authentication, ARMv8.3); the Snapdragon 865's Cortex-A77 is ARMv8.2 and has no PAC: `retab`, `blraa`... are undefined there. The 16.5 port runs the **iPhone10,3 (A11, plain arm64)** userland for exactly that reason.
- **The kernel.** The kernelcaches (`kernelcache.release.v63`, `v64`, `v6x-pandia`, observed) are for Apple silicon (PPL/SPTM, exclaves: `exclavecore_bundle` in Firmware, observed). The port is built from open XNU 8796 (iOS 16.5); a 27.2 userland (new dyld, cryptex mounting, new syscalls and Mach interfaces, Secure Enclave and exclave services) needs a matching XNU whose source for this release was not looked for.
- The device-specific firmware in the IPSW (SEP, AOP, DCP, ISP, AGX, baseband `Mav25`) is all for the iPhone's chips and is of no use here.

## If it is ever wanted
A different goal would have to be chosen first: an iOS version whose userland is plain arm64 (the port's rule; iOS 16 is the last for A11) or a PAC emulation in the kernel's undefined-instruction handler for arm64e (slow and a project of its own), a flash root on a new partition (needs the owner's decision on the 16.5 data), and the AEA keys.
