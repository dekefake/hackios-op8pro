# Bluetooth (OnePlus 8 Pro)

**Status (owner-verified in RAM runs, 2026-10-27):** discovery (Classic and LE), pairing, and A2DP audio to a JBL Charge 2+ work with iOS's own `bluetoothd`.
Open: local name shows "(null)", a paired speaker's device type is blank, the Settings switch does not power the chip off, receive is polled, calls/sleep unverified
([../TODO.md](../TODO.md) T05, [../KNOWN_ISSUES.md](../KNOWN_ISSUES.md) K05). Background and the run-by-run record: `INSTANTNOODLEP-PLAN.md` addenda 36-44 (top-level repository).

## The hardware
One chip, the QCA6390: Wi-Fi on PCIe (`instantnoodle_wifi.c`), Bluetooth on a UART — serial engine 6 of QUPv3 wrapper 0 (`qcom,qup_uart@998000`, GPIO 16 CTS, 17 RFR, 18 TX,
19 RX, function 1), BT_EN GPIO 21, sw-ctrl GPIO 124 (an input the controller drives). Shared rails (RPMh): smpa6, smpf2, smpa5, smpc8, plus Bluetooth's own ldoa16. The two drivers vote
the same values and never turn the shared rails off.

## The driver: `xnu/files/bsd/dev/instantnoodle_bt.c` (a ladder of boot-args, see [../BOOT_ARGS.md](../BOOT_ARGS.md))
| `bt=` | What it does | Verified |
|---|---|---|
| 1 | reads the engine's clocks and pins | yes |
| 2 | powers the chip as Android does, reads the engine (its firmware is UART, FIFO mode) | yes |
| 3 | sets the engine up as a polled UART and sends HCI Reset and the ROME version request | yes: the controller answers |
| 4 | downloads the ROME patch (205836 bytes, 848 segments of 243) and NVM (5815 bytes, 24 segments) from the phone's own `htbtfw20.tlv`/`htnv20.bin` | yes: patch version 0x0d2b → 0x4509 |
| 5 | keeps the controller up as a packet device, `/dev/hk-bt` (H4 packets, one per read/write); a worker thread polls the UART | yes: 7/7 self-tests, 100 long events whole |
| 6 | registers the kernel control `com.apple.uart.bluetooth` (SOCK_STREAM) for bluetoothd; sysctl `kern.hackios.bt` (state), `/dev/btpoweroff` | yes |

Things learned the hard way (all in the file's header comment too):
- The engine's root generator is **not in DFS mode** on this boot chain: the driver programs GPLL0-even 300 MHz × M/N and the engine's divider itself, and puts the old values back. The engine **samples at 16x**: real baud = clock / divider / 16.
- The serial clock register must be enabled before any other engine write (a write with it at 0 took the phone to CrashDump).
- Firmware order (btqca.c): the patch, the NVM, **HCI_Reset at the download's rate (115200)**, only then the host to 3.2 Mbaud (tag 17 byte 1 = 0x11).
- After the firmware the controller uses Qualcomm in-band sleep: a 0xfd (wake-up) is answered with 0xfc; the host sends 0xfd before the first packet and never sends 0xfe.
- RX runs with the controller's flow control (no "RFR open"), TX honours CTS: a 1 ms poll cannot keep up with 3.2 Mbaud otherwise.
- The firmware is the phone's own and **never in a repository**: `op8pro-data/docker/rebuild-bt.sh` turns `op8pro-data/bt-fw/*` into a generated header (`instantnoodle_btfw.h`, not committed) that `instantnoodle_bt.c` includes if present.

## iOS side (userland `BLUETOOTH=1`; the design is the upstream author's, commit c4379c2 of `iphont-xnu-userland`)
- **bluetoothd talks HCI itself.** `BlueTool` is only a boot helper: bluetoothd asks the Mach service `com.apple.BlueTool` to run `RunBuiltinScript` init/boot/deepsleep. `mkjobs.py` keeps
  `com.apple.BlueTool` denied and a job `com.hackios.bluetool` runs `src/btboot.c` under that service name (boot waits for `kern.hackios.bt` = "state up", up to 180 s).
- `mkjobs.py` sets bluetoothd's `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,3000000`: bluetoothd connects `socket(PF_SYSTEM, SOCK_STREAM, SYSPROTO_CONTROL)` by name (the control **must** be registered with `CTL_FLAG_REG_SOCK_STREAM`).
- `src/inject_bt.c` (library inside bluetoothd) patches private function-pointer tables at fixed 20F66 addresses (generic vendor descriptor, no Broadcom VSCs, no multi-advertising, the unsupported init steps omitted) and logs bluetoothd's open/socket/connect/ioctl calls (`bt-watch:`).
- **Audio:** Apple's unmodified `BTAudioHALPlugin.driver` must be in the **system image's** `System/Library/Audio/Plug-Ins/HAL` (a RAM boot's audio server reads that one; `build.sh` puts it there for BLUETOOTH=1, and into the small image's `/hackios/hal` for the installed boot). Without it bluetoothd logs "unable to sendMsg due to no available xpc client connections" and the sound stays on the speaker.
- `init.c` sets the host name ("OnePlus 8 Pro"); it did **not** fix the "(null)" in Settings.

## Not possible / not done
- Receiving a photo from an Android phone's classic "Bluetooth share": iOS has no Object Push server. AirDrop needs the Wi-Fi side (untried).
- Other Apple audio plug-ins not shipped (AirPlay, USB audio, CarPlay, baseband voice, AOP): [../TODO.md](../TODO.md) notes; only Bluetooth was needed.

## Testing
`userland/rc.jobs.bt` + `src/bt_test.c` (opens `/dev/hk-bt` from iOS user space: Reset, version, an 8 s LE scan), `src/bt_scan.m` (CoreBluetooth discovery probe), the kernel's per-event counters (`bt: stats:` every 5 s) and L2CAP connection lines (`bt: acl`).
Run: `bt=6 bt_after=20` (or 45 with `wifi=1`), image built with `BLUETOOTH=1`, log snapshots every 20 s (the loader keeps only the first 256 KB and last 1 MB of the kernel log).
