# TODO — what HackiOS still needs implemented or completed

The development roadmap. It lists **work**; what is *broken for a user right now* is in [KNOWN_ISSUES.md](KNOWN_ISSUES.md) (entries `K..`),
the session state is in [HANDOFF.md](HANDOFF.md). Entries here (`T..`) are cross-referenced, not duplicated. Evidence for every item is the
source tree and the subsystem pages; items built from source comments cite the file.

*Last audited: 2026-10-26 against the working tree (both git trees dirty, see HANDOFF).* Target device is the **OnePlus 8** unless an item
says otherwise; the owner asked not to run redwood checks.

**Status:** Not Started · Partial (some of it works) · In Progress (someone is actively changing it) · Blocked (cannot proceed without something named) ·
Built, Unverified (compiles/QEMU-tested, never seen on the phone) · Complete (listed only where an older document made it look unfinished).
**Priority:** **P0** safety, or without it the phone is not a usable device · **P1** a major missing iOS capability or a verification debt on shipped work ·
**P2** quality, efficiency, convenience · **P3** research or maintenance.
Order inside each section is by priority. Update an item when you change its state; when finished mark Complete and keep the diagnostic findings.

## Index

| ID | Item | Status | Pri |
|---|---|---|---|
| T01 | Verify the unverified power work on the phone | Built, Unverified | P0 |
| T02 | Wi-Fi radio through system sleep | Built, Unverified | P0 |
| T03 | Install the current build / keep installed bundle current | Partial | P1 |
| T04 | Motion, light and proximity sensors (screen rotation, auto-brightness) | Not Started | P1 |
| T05 | Bluetooth | Working in RAM runs (owner-verified); gaps listed | P1 |
| T47 | 8 Pro: Wi-Fi auto-join, sleep, AirDrop, sensors, charging | Open (see section) | P1 |
| T06 | Cellular modem, telephony, SMS | In progress (flashed image 43: LTE registration, SIM PIN, **mobile data on en1 (verified)**, SMS send, SMS receive with iOS notifications (verified: banner, lock screen, Messages list and conversation), bars, RTC date; open: calls/VoLTE/VoWiFi, tethering, IPv6) | P1 |
| T07 | OnePlus 8 cameras | Partial (power-up and bus rungs, sensor never answers) | P1 |
| T08 | Headphones / USB-C audio | Not Started | P1 |
| T09 | Fingerprint / biometric authentication | Not Started | P1 |
| T10 | Metal: from experimental to usable | In Progress | P1 |
| T11 | Heterogeneous CPU scheduling (Edge) | Partial (working, uncommitted) | P1 |
| T12 | Sleep power: SoC low-power states | Blocked | P1 |
| T13 | Battery drain measurement | Not Started | P1 |
| T14 | Location (GPS) | Not Started | P2 |
| T15 | App Store, Apple ID, FairPlay, push (Secure Enclave dependent) | Blocked | P2 |
| T16 | Brightness: prove the light changes | Partial | P2 |
| T17 | CPU time after wake (widgets, chronod) | Partial | P2 |
| T18 | Wi-Fi: 802.11r, TKIP pairwise, roaming, other SAE groups, persistence | Partial | P2 |
| T19 | Fast charging (Warp / PD) | Not Started | P2 |
| T20 | CPU idle power collapse | Not Started | P2 |
| T21 | GPU: IFPC and measured power | Blocked | P2 |
| T22 | CoreAnimation fidelity gaps (sb3, poster switcher, CC glyph opacity, switcher wallpaper) | Partial | P2 |
| T23 | Display: late swaps, cold-start jank | Partial | P2 |
| T24 | Speaker protection (TFADSP) and other microphones | Partial | P2 |
| T25 | Video and Live Photo recording; redwood-only camera parity | Not Started | P2 |
| T26 | Storage: writable APFS, fsck, HFS write speed | Partial | P2 |
| T27 | Boot menu usability (5 s countdown, graphics) | Partial | P2 |
| T28 | Security model: stand-ins protect nothing | Partial | P2 |
| T29 | Remove stand-ins and copies (`hk_wlfw.c`, duplicated DSP code) | Partial | P3 |
| T30 | Redwood re-verification and parity with OnePlus work | Not Started | P3 |
| T31 | Documentation and repository debts | Partial | P3 |
| T32 | Scripted gesture reach, test coverage | Partial | P3 |
| T33 | Thermal service never seen throttling | Built, Unverified | P3 |
| T34 | Sensors that are not sensors: NFC, UWB, Taptic edge cases | Not Started | P3 |
| T35 | Complete (do not reopen): items that older text made look open | Complete | — |
| T36 | iphont agent hardware validation and workflow migration | Partial / Built, Unverified | P1 |
| T37 | UI smoothness: dropped frames, the phone feels slow | Not Started | P1 |
| T38 | Native panel resolution (QHD+ instead of the iPhone X's) | In Progress | P1 |
| T41 | Status bar and lock screen fit the 8 Pro's camera hole | Owner-verified (RAM boot), not flashed | P2 |

---


## P0

### T01 Verify the unverified power work on the phone — Power · Built, Unverified · P0
- **Works:** sleep/wake with timer and power key, panel off/on, governor, touch reset, audio park, GPU collapse (verified; [power.md](subsystems/power.md)).
- **Missing evidence:** shared-interrupt fix in a long session; 3-strikes push fallback; volume-key interrupts and the key thread's dark wait; slider at 300 ms dark;
  USB reconnect fix (`abort_xfer`); charge-state numbering; charger input limit (2000 mA DCP / 1500 CDP) and its save/restore; **off-mode charging** (loader);
  menu entries "Recovery"/"Bootloader"/"Power off" and the cable-out wait; **empty-battery shutdown**; first sound after an unpark.
- **Why P0:** these paths write charger registers and decide whether a flat phone shuts down safely.
- **Files:** `bsd/dev/hk_power.c`, `hk_buttons.c`, `instantnoodle_battery.c`, `xnuloader/flashboot.c`, `loader.c`.
- **Next:** run one `tools/t power` level for the sleep parts; with the owner present: plug a wall charger and read `charger` diagnostics, try off-mode charging,
  `battery_empty_mv=N` above the cell voltage to test the shutdown. Record in power.md and HANDOFF.
- **Refs:** [power.md](subsystems/power.md) "Verification record"; K08.

### T02 Wi-Fi radio through system sleep — Wi-Fi/Power · Built, Unverified · P0
- **Works:** radio up, scan, join, DHCP, Safari; `hk_power_will_sleep()` takes the radio down (network left, firmware stopped, link off) and a thread brings it back.
- **Missing:** a real sleep with the radio on and a network joined, then a wake and rejoin; firmware crash recovery by reset.
- **Files:** `bsd/dev/hk_wifi.c` (wifi-power thread), `instantnoodle_wifi.c`, `hk_power.c`.
- **Blockers:** needs RAM run with `wifi=1` + `sleep_max`; `/private/var` RAM-only so the password must be in `fw/network`.
- **Next:** `tools/t`-style run: join, `power_test sleep 5 2`, check `en0` and Settings.
- **Refs:** [wifi.md](subsystems/wifi.md); K05.

## P1

### T03 Keep the installed bundle current — Boot/Installed · Partial · P1
- **State:** the installed bundle and `boot_b` loader were updated 2026-10-25 14:20 (power rounds 1-4). Later work (GPU collapse, brightness, `ca_ambient`, vmd DENY,
  Wi-Fi) is not installed. The latest userland images were built for tests, not the installed recipe.
- **Next:** `common/flash/install-xl.sh instantnoodle update` after T01 evidence; **requires the owner's explicit OK** (writes the flash). Rebuild with `FLASH_USERLAND_VARS`.
- **Refs:** [INSTALL.md](INSTALL.md), research/boot-chain-and-recovery.md.

### T04 Motion, light and proximity sensors — Sensors · Not Started · P1
- **Why:** no accelerometer means no screen rotation, no CoreMotion, no auto-brightness (and CoreBrightness's 5 s ALS wait stays).
- **Facts:** sensors hang off the audio DSP behind its sensor framework ("SEE") via QMI service 400 over QRTR ([thermal.md](subsystems/thermal.md) has the step table:
  DSP started / GLINK / QRTR / services listed = done (`adsp=1`, `hk_adsp.c`; `sysctl kern.hackios.adsp_services`); QRTR ports of our own on the DSP channel, the service locator and notifier
  the DSP's sensor domain waits for, the sensor registry the DSP reads from Android's partitions, a SEE client, delivery as HID events for CoreMotion = **to do**).
- **Files:** `bsd/dev/hk_adsp.c`, `hk_qmi.c`, `sys/hk_qrtr.h`; new driver; iOS side: a virtual HID accelerometer service in `inject.c` (pattern: the touch service).
- **Blockers:** sensor registry (Android partitions: `persist`/`vendor`) unavailable without Android; OnePlus 8 firmware needs checking.
- **Next:** run `adsp=1` and read `kern.hackios.adsp_services` for service 400 (the previous plan's first question; result not recorded).
- **Refs:** K07.

### T05 Bluetooth — Connectivity · Working in RAM runs · P1
- **Verified by the owner (2026-10-27, RAM runs, OnePlus 8 Pro):** Settings > Bluetooth lists nearby devices (Classic inquiry and LE), pairing with an Android phone works, and **A2DP audio plays on a JBL Charge 2+** (YouTube in Safari). Design and findings: [subsystems/bluetooth.md](subsystems/bluetooth.md).
- **Open:** the local name shows as "(null)" in Settings > Bluetooth (the host name is set by `init`, but Settings does not take it from there); a paired JBL's device type stays blank (the owner picks it by hand; probably an accessory database iOS downloads); the Settings switch does not power the chip off (`/dev/btpoweroff` is only noted); Bluetooth receive is polled (1 ms) and not measured under load; calls/HFP, LE audio, BLE peripherals and sleep/wake are unverified; photo receive from Android's "Bluetooth share" can never work (iOS has no Object Push); AirDrop needs the Wi-Fi side (not tried). WPA3 (SAE) joins failed in the same session with "NO CONFIRM from the access point" (T18).
- **Files:** `xnu/files/bsd/dev/instantnoodle_bt.c`; userland `src/btboot.c`, `src/inject_bt.c`, `src/bt_scan.m`, `src/bt_test.c`, `mkjobs.py` (BLUETOOTH=1), `build.sh`.
- **Refs:** K05; boot-args `bt`, `bt_after`, `bt_nvm`, `bt_skip`, `bt_cts` ([BOOT_ARGS.md](BOOT_ARGS.md)).

### T06 Cellular modem, telephony and SMS — Connectivity · **In progress** (2026-10-28) · P1
- **Facts (corrected 2026-10-28):** the modem is not on the SoC: it is an external Snapdragon X55 on PCIe 2, see [research/modem-sdx55.md](research/modem-sdx55.md). CommCenter (`-noncellular` variant) answers but dies on missing `AppleIPAppender` (SIGSEGV) and `AppleBasebandPCI` (SIGABRT) ([research/ios-userland.md](research/ios-userland.md) §4). `vmd` denied.
- **Done, verified on the phone (RAM runs only; `xnu/files/bsd/dev/instantnoodle_modem.c`, `hk_mhi.c`; boot-arg `modem=N`):** PCIe 2 link to the X55; its SMMU stream; firmware read from the phone's modem_b partition; **the modem booted from the application processor** (boot ROM via BHI, SBL, Sahara with 15 images, mission mode); **all software MHI channels start; LOOPBACK echoes; the modem's two QRTR links (node 2 on QMI0 with version-2 headers, node 3 on DUN with version-1 headers) say HELLO, are answered, and the modem announces its services** (DMS, NAS, WMS, UIM, WDS, VOICE...).
- **2026-10-29, verified on the phone (RAM test kernels, `modem_bringup=1 modem_tftp=1 modem_efs=1 ...`):** the earlier "blockers" were mistakes of mine: the device-tree channel names are DECIMAL (the real table is in the code), and the modem's QRTR router drops a RESUME_TX shorter than 20 bytes, which stalled all its traffic after 10 datagrams. With those fixed: the host serves the modem's **TFTP service** (QRTR 4096, instance word 3, `/readonly/firmware/image/...` from the FAT of `modem_b`, `/readwrite` writes discarded) and its EFS images from the phone's own `mdm1m9kefs1/2/3` partitions (LUN 5, read only); then **QMI works (DMS, UIM, NAS, WMS), the SIM's PIN1 (0000) is verified over UIM, DMS goes ONLINE and the modem REGISTERS on the network (LTE, CS and PS attached) 17 s after power-up; WMS Raw Send answered success (message id 30) for one test SMS to the owner's number (the owner confirmed the SMS arrived).** A kernel service (`modem_service=1`) keeps the sysctls `kern.hackios.cellular_*`/`modem_*` true, sends SMS written to `modem_sms_send` (verified from iOS user space with `hk_cell`) and is in the flashed image 32; the SpringBoard hook shows bars and the network name (seen in the log only). DIAG (F3 messages) is read over MHI channel 4/5 for debugging. Details: [research/modem-sdx55.md](research/modem-sdx55.md).
- **2026-10-29 later: MOBILE DATA WORKS (verified on the phone, RAM runs and flashed image 38/39; `modem_data=1`, `modem_chans=2187`).** The modem's hardware IP channel pair 100/101 is driven by the kernel (no AP IPA needed), the session is made with WDA/WDS (research/modem-sdx55.md "Mobile data over IP_HW0"), and `en1` (hk_net unit 1) carries it: configd's IPConfiguration DHCPs it (`net_test cell`), and from iOS user space name resolution, TCP, HTTPS through CFNetwork and reachability all work over LTE with Wi-Fi not joined; a 5-minute loop of HTTPS fetches ran through the modem's EFS sync. SMS received are kept in `kern.hackios.modem_sms_ring` and `hk_smsd` (launch daemon, mobile) writes them into `/var/mobile/Library/SMS/sms.db` (insertion read back and tested with a fake message that was then purged; a real SMS (two from the owner, a self-test) was received, decoded, acknowledged and stored; Messages shows it and iOS posts a real notification (screenshots), see K47).
- **2026-10-29 (late): native IPv6 + SMS send from Messages, flashed image 43** (kernel `data19`, root `userland/root-cell28.apfs`): the data session is the IPv6 PDN (iOS does IPv4 by CLAT46), `en1` is configured through `net_test v6store`; imagent's SMS send is replaced inside imagent (`libhackios-im`); `hk_smsd` now resends only rows Messages marked failed. Details in research/modem-sdx55.md. **Tethering** (not done): the plan is (1) decide the transport: Bluetooth PAN (bluetoothd's NAP + BNEP, our HCI transport works) or Wi-Fi AP (ath11k AP mode in `wifi/lx`, a WPA2 4-way authenticator, DHCP by `bootpd`); (2) read misd's XPC interface (`/usr/libexec/misd`: keys natType, nat64Param, nat66Param, bridgeType...) and drive it from a small client with the cellular interface as external; (3) PF is in the kernel? (check `pfctl`: not in the image).
- **Open after this:** sending SMS from the Messages app (needs an SMS service behind imagent or a hook of its send path to `kern.hackios.modem_sms_send`), voice calls (QMI VOICE + the ADSP's audio path + CommCenter's side), tethering, IPv6 on en1, the session after a modem restart (written, not exercised), data throughput/power, a user switch and roaming policy, Settings > Cellular (no CommCenter).
- **Still open:** iOS side (CommCenter needs AppleBasebandPCI/IPAppender, nothing speaks to the modem from iOS yet): a kernel service that runs this bring-up after boot and exposes it (device node/sysctl), a stand-in baseband daemon, status-bar bars, SMS receive/send from Messages; voice calls (QMI VOICE + the audio path); mobile data (IPA/MHI hardware channels 100/101); the modem's EFS is read from Android's partitions but never written back (changes are lost at every boot); TFTP `/readwrite` is a write-discarding stub; nothing of this is in the flashed image 30.
- **Reference:** the redwood (POCO X5 Pro) cellular stack in the synced `hackios-xnu-kernel` repo (QMI UIM/NAS/WDS clients, SIM provisioning, status-bar signal hook, data path via `en1`) is the model for everything above the link; its lessons (pd-mapper, tftp, rmtfs, IPA for the data plane) are summarised in the research page.

### T40 OnePlus 8 Pro dual boot: storage driver, userdata shrink, APFS partition — Storage · In Progress · P1
- **Goal (owner, 2026-10-27):** a 64 GB APFS partition for iOS system and data carved off the encrypted `userdata` (Android keeps working with 64 GB less), the loader booting it instead of a RAM image.
- **Blocker, verified:** the 8 Pro's UEFI runs without `UFSDxe` (it asserts Device Error re-initialising the controller ABL used, plan addendum 5), so the loader has no storage. **Measured 2026-10-27 (`ufs_peek=1`, kernel `ufs1`): the controller is alive and warm as ABL left it**: CAP 0x1587031f (32 slots, 64-bit), UFSHCI 3.0, HCE 1, HCS 0x10f (device present, UTRL and UTMRL ready), UTRLBA 0x9a131000 running with an empty doorbell, UTMRLBA 0x9a132000. So the answer is a driver that adopts it without any reset (what `hk_ufshc.c` does on redwood, read only, with the loader's memory for the list), plus a write path with the same whitelist discipline (WRITE(10/16) only into the port's own partition).
- **Plan (no flash write before step 3):** (1) read driver on the warm controller: own list in memory the kernel/loader owns, proof by TEST UNIT READY then READ of the GPT of LUN 0; find out whether the apps SMMU lets the UFS stream DMA to our addresses (stream faults are visible in the global fault registers); (2) the loader serves it (`ufs=2` path, `BLOCK_VERSION` page) so hk_ufs.c reads partitions; (3) SHRINK, in ONE Android session under LineageOS+Magisk root: `tools/android/f2fs-resize` (F2FS_IOC_RESIZE_FS works on the mounted, decrypted view: no need to touch the metadata encryption), then `tools/userdata-carve.py --move` writes the new GPT (userdata ends where the shrunk f2fs ends; new APFS-type partition `darwin` after it); needs a full backup of /data and the owner's explicit OK for each write; (4) boot Android once to prove it works with 64 GB less; (5) writes to the new partition (`ufs_write=1`, whitelist), `tools/darwin-write.py` for the iOS volume, UEFI+loader into the spare slot's `boot_a`, loader menu to choose; every write step needs a fresh OK.
- **2026-10-27 night, measured with `ufs8=1|2|3` (`xnu/files/bsd/dev/instantnoodle_ufs.c`, read only, kernels `ufs2`..`ufs10`):** swapping the controller's transfer list works (readback OK), but **every DMA ends at once in IS bit 17 (SBFES, system bus fatal error), OCS 0xf, the descriptor never fetched**: with a list in kernel memory above 4 GB, in the kernel image below 4 GB (pa f8022000), and with ABL's OWN untouched slot, command descriptor and data buffer (0x9a12f000 / 0x99982018), so it is neither the address nor our list. All UFS clock branches and the power domain are on. The apps SMMU shows 5 valid match registers (streams 0x4a0, 0xc0, 0x0, 0x20, 0x820/0x402): no UFS one, sCR0 has USFCFG set; **writes to sCR0 and to match registers with a mask are ignored (the hypervisor owns the SMMU)**. After the first failure the controller is wedged (a second doorbell clears with IS 0, nothing done) and the phone ends in CrashDump. Reading the registers works (`ufs_peek=1`). Conclusion: the DMA path is broken in the state ABL leaves it in (or by the hypervisor's SMMU/xPU settings for this stream); next: (a) try single-stream bypass match registers for UFS stream candidates the hypervisor may accept (the camera got 0x800/0x840/0xc00/0xc40), (b) a full Linux-style bring-up of our own: HC reset (GCC `gcc_ufs_phy_bcr` 0x77000), device reset, QMP PHY tables, link startup (Lineage `ufs-qcom.c`, `phy-qcom-ufs-qmp-v4-kona.h`), after which the DMA state is the one Android's driver gets.
- **2026-10-27 late: THE UFS READ WORKS on the 8 Pro (`ufs8=9`, kernel `ufs15`, `xnu/files/bsd/dev/instantnoodle_ufs.c`; read only; GPT of LUN 0 read and printed).** The recipe: (1) the DMA failure (IS bit 17) was the apps SMMU: the UFS controller's stream is **0xe0**, no match register had it, and sCR0.USFCFG faults unmatched streams. The hypervisor takes a single-stream match register for 0xe0 (also 0xa0, 0xa1, 0xc0; refuses 0xe1, 0x100.., 0x60.. and every masked entry) and keeps its own fault S2CR when a bypass type is written, but takes **S2CR = a context number (translate)**. So: a context bank of ours (context 6, camera-style 3-level tables, our 72 KB area at IOVA 0x40000000, sctlr 0x127), SMR 0xe0 -> that context, and the controller's list, command descriptors and data buffers are given as IOVAs. (2) The warm controller and ABL's link are fine as they are (no HC reset, no PHY, no link startup needed: modes 4-7 of the same file, which also work as far as the link goes, are kept as tools). (3) The first command returns CHECK CONDITION NOT READY, ASC 04 ASCQ 02 ("initializing command required"): **START STOP UNIT with power condition ACTIVE (0x10) to the device well-known unit 0xd0, then START (0x01) to LUN 0**, after which TEST UNIT READY, READ CAPACITY(10) (last block 61753343, 4 KiB blocks) and READ(10) work. LUN 0's GPT (32 entries, 23 partitions; blocks of 4 KiB): ssd 6, persist 8, misc 8200, keystore, frp, param, mdm_oem_*, modemdump, op2, config, opproduct_a/b, super 310152..3980167, recovery_a/b, vbmeta_system_a/b, metadata 4031400, vm-system_a/b, rawdump, **userdata 4133800..61753338 (219.8 GiB), the last partition**. Nothing was written to the phone. The old `ufs8=1/2/3` failures were this missing SMMU entry, not the controller, the clocks or the address. Next: (a) a kernel-native storage backend for the 8 Pro in hk_ufs.c (partitions from this GPT; more slots and a larger buffer; the loader's `ufs=2` page not needed), (b) the whitelisted write path for the port's own partition only, (c) then step 3 of the plan (shrink in an Android session, with a backup and the owner's OK for every write).
- **2026-10-27 night: the kernel-native read backend is in `hk_ufs.c` (`ufs_8pro=1`, kernel `ufs17`): partitions of LUN 0 from the GPT as `/dev/ufs_*`/`/dev/rufs_*`, 1 MB buffer, ~840 MB/s, liblp magic and AVB0 headers read right (see subsystems/storage.md, BOOT_ARGS.md). Next: the whitelisted write path for the port's own partition, then the shrink (backup + owner's OK per write), then boot from it.**
- **2026-10-27 night: DONE and verified on the phone: userdata erased and shrunk (155.8 GiB; Android 14 boots and shows 166 GB), `darwin` created (sda24, 64 GiB at the end of LUN 0, blocks 44975616..61753338, APFS container type; the old GPT is saved in `op8pro-data/gpt-backup-20261027/`, not in git), the iOS root volume (`3.apfs` out of rootfs.dmg, 5628755968 bytes) at darwin block 0 and the OS and App cryptexes at 1374208 and 2039808 written over adb (push + dd, read back and compared by SHA-1). The first iOS boot with its root on that flash: RAM runs with `userland/flashboot.apfs` (the 17 MB boot volume: stage0, the audio firmware; built by hand from the build.sh recipe, CRYPTEXES=flash) as the resident image, `userland/root-flash1.apfs` (= root-merged1 with rc.jobs.installed) as the payload, boot-args `ufs_8pro=1 root_apfs=ufs_darwin`: root and both cryptexes from `/dev/ufs_darwin`, `/private/var` a RAM filesystem, home screen up (owner-verified). `mediaserverd` crashes in a loop in this mode (not diagnosed: our media library under the whole volume?). Not done: writes (a whitelisted path for darwin: persistent /private/var, hk_data), a standalone boot (the loader cannot read UFS: the UFS backend would have to move into the loader, or the boot image carry kernel + images), `boot_a` (needs the owner's explicit approval).**
- **2026-10-28: the owner flashed the first standalone image (to `boot_a`) and found: nothing persists (/private/var is RAM) and iOS's power-off restarts the phone (the loader's one-shot path ran `reboot()` after the kernel returned with reason 3). Built, to test: `out/instantnoodlep-standalone2.img` = loader with `power_off()` after reason 3 + kernel `ufs19` with the whitelisted write path (window = darwin from block 2042368) + `hk_data=1 var_data=hfs` persistence. Not yet booted.**
- **2026-10-28 (RAM boot of `out/instantnoodlep-standalone5.img` over `fastboot boot`, owner at the phone): the standalone boot works up to the data volume: init formats `/dev/rhk_data` (52 GB HFSX, journaled, 8 MB journal) and mounts it on /private/var, no write errors, no read-back mismatch; the loader's USB link is up during the run (`fastboot oem log` / `get_staged`). Three bugs found with that USB log: the loader's one-shot path ran `reboot()` after iOS shut down (now `power_off()`), darwin's 16777723 blocks failed my `<= 1<<24` size check (no write window, no /dev/rhk_data), and `ufs_buffer_size` was 0 in the synthetic loader page (hk_data_crw never advanced: init hung in newfs_hfs). To check by the owner: persistence across a restart, power-off from iOS's menu. Not yet flashed.**
- **2026-10-28: 120 Hz is the profile's rate (owner: clearly smoother, verified).** Frame drops at 120 Hz: a scripted Settings scroll has 7% (or, on some boots, 6.5%) of its push intervals late; the CPU governor, the GPU clock and a frame-driven CPU boost were measured and are NOT the cause (the runs split by boot, 87.5% or 93% within a refresh: display.md). The GPU stage is 5.75 ms of the 8.33 ms budget: the real limit; next candidates: scaling by the MDP's scaler instead of the GPU blit, Mesa's tile mode, why a boot lands in the slower pacing group. `libhackios-gl surface_changed` (checksum of unchanged IOSurface textures) was 7.7% of samples in a page-swipe workload; libhackios-gl cannot be rebuilt on this Mac (no Mesa tree).
- **2026-10-28: status bar: the whole bar is level with the hole-punch camera** (`HK_SB_ICONS_UP` 4.6 pt in `inject_ui.c`: the icons' box up in DynamicSplit's setup call; measured on a panel screenshot: clock digits centre 59.5-60.5 px, battery body 60.0 px, was 71.0).
- **2026-10-28: standalone boot works from RAM (`fastboot boot`), owner-verified: persistent /private/var on the flash, iOS power-off switches off within seconds (`hk_fast_halt=1`: reboot(2) syncs the data volume and returns to the loader, which powers off; XNU's own unmount sequence stalled on the HFS volume), USB log and no on-screen text. `out/instantnoodlep-standalone10.img` (sha256 cb24cf0e...) was flashed to `boot_a` by the agent at the owner's request. Android's boot_a is gone from that slot: to get it back `fastboot flash boot_a` its backed-up boot image (ios-instantnoodle/backup). Not tested yet: restart from iOS, a power-off with the cable out, the clock (epoch= is the build time).**
- **2026-10-28 (built, NOT tested on the phone; the owner asked for no reboot/reflash meanwhile): boot menu of the standalone image. `out/instantnoodlep-standalone11.img`: hold volume up while the phone starts (a key in the first 1.2 s of the loader) for a menu with iOS, "Android (other slot, once)", Bootloader, Recovery, USB (xl), Power off; the text is the console's, blown up 3-4x in the framebuffer from the top left corner (`om_magnify`, xnuloader/loader.c). "Android" starts the kernel with `hk_swap_slot=1` (patched into the command line in place of `hk_fast_halt=1`): the kernel exchanges boot_a/boot_b's A/B bytes (the loader has no storage) and restarts into the other slot. Returning to iOS after Android needs the HackiOS module in Android (Magisk/KernelSU), `out/hackios-rearm.zip` (SPARE=a, the image's SHA-256 in hackios.conf: made again for every image that is flashed): it runs `bootctl set-active-boot-slot 0` a minute after Android has booted. Without it Android stays the active slot (`fastboot --set-active=a` to undo).**
- **2026-10-28 (built, NOT run on the phone): the volume-up menu is the UEFI's (Mu's boot manager), so it was replaced in the UEFI: `out/instantnoodlep-standalone12.img` = a UEFI built here (`uefi/README.md`: container build) whose `HackiosMenu.c` shows, 3x the font size and centred, iOS (no USB debug, default) / iOS (USB debug) / Reboot to Android (other slot) / Reboot to bootloader / Reboot to recovery and below them the platform's entries; the loader (`out/loader-8pro-uefimenu.efi`) acts on the variable `HackiosBootMode`; default boots have the USB link OFF now; kernel `ufs26` (`hk_swap_slot`); small image `userland/root-sb1.apfs` (status bar and lock screen fixes, T41). The loader's own menu (the 12-second one with magnified console text) was removed. `out/hackios-rearm.zip` is made for this image's SHA-256.**
- **Files:** `hk_ufshc.c`, `hk_ufs.c`, `xnuloader/loader.c`, `tools/userdata-carve.py`, `tools/android/f2fs-resize.c`, `tools/darwin-write.py`; plan addenda 2, 5, 20, 53.

### T49 iOS 27.2 (iPhone19,2/19,3/19,7 IPSW) on the OnePlus 8 Pro — Platform · **Aborted** (2026-10-29)
- Asked: prepare it for a RAM boot beside iOS 16.5. **RAM too small** (8 GB; the OS image alone is 9.5 GB compressed, 12.7 GB with the cryptexes), so aborted as instructed; the phone and the 16.5 installation were not touched. Also in the way: AEA-encrypted images, arm64e userland on a CPU without PAC, an Apple-silicon kernelcache and no matching XNU. Facts and what a different goal would need: [research/ios27-2-ipsw.md](research/ios27-2-ipsw.md).

### T07 OnePlus 8 cameras — Camera · Partial · P1
- **State:** redwood has `hk_camera.c` (four sensors, ISP in userland). The OnePlus 8 Pro has `instantnoodle_camera.c` (boot-arg `cam=1` a look, `cam=2` the power-up as Android does it, then a bus scan): the rails, GPIOs, the camera clock domain and the CCI bus work (the module's actuator and OIS answer), ****the IMX689 answers since 2026-10-27 (chip ID 0x0689; gpio 156 is an eGPIO: `instantnoodle_i2c.c` now sets bit 12 of such pins, plan addendum 47); the module's EEPROM (0x50) still does not**, on the bit-banged I2C. Android's sequences and the sensor-module files are decoded (`tools/camsensor.py`; plan addenda 33, 35, 37, 38).
- **2026-10-27 later (plan addendum 48):** Camera.app now launches (the UI hook library was recursing in it; removed from its injection) with a black viewfinder; the sensor probe kills the kernel in the full config (gpu/wifi/bt...) and not in the minimal one: read the panic with `fastboot get_staged` right after the run returns (`tools/macdev/logpoll.py`). Then the frame path (CSIPHY/CSID/IFE from `techpack/camera`) and `/dev/hk-camera` for iOS.
- **2026-10-27 evening (plan addenda 49, 50): a real IMX689 frame is in DRAM and on the host (`cam=4`).** Remaining: continuous frames + `/dev/hk-camera` for iOS (the 8 Pro has no such device yet), focus, exposure control from iOS commands, Bayer order/tuning, the other sensors (IMX586 needs PM8009 GPIO 1 + smpa5 + ldoc7; the IMX471/S5K3M5 do not answer yet).
- **2026-10-27 night (plan addendum 52): back and front camera stream into iOS, autofocus (AK7377) and the flashlight work; flash for photos and the faster ISP are built, not yet seen.** Open: front colour/orientation, tap-to-focus speed, OIS (LC898128: its firmware download is not attempted), IMX586/S5K3M5, white balance/Bayer order/tuning, grain (binned mode, no NR), video (T25).
- **2026-10-27 night: owner-tested `merged1/2` (plan addendum 52): front and back camera, autofocus, flash and the flashlight work. Tap-to-focus does NOT: the search starts (`AFWindowParams`/`AutoFocusNow` arrive, the sweep runs) but does not land where it was tapped; AF_FAR was raised 768 to 1000 without fixing it. Roadmap: look at the sweep's result and speed per tap (`focus: sweep:` lines, `camera_device.c` `af_*`), the window weighting, and whether the tap's lock (`LockAFNow`) is honoured; then OIS.**
- **Next:** find the line that differs from Android for the sensor and the EEPROM (not yet tried: OIS-first ordering, 400 kHz, MCLK measurement, GPIO levels while Android has the camera open); then CSIPHY/IFE and the ISP in userland; `camera_device.c` formats are iPhone X names. The owner wants this finished.
- **Files:** `instantnoodle_camera.c`, `instantnoodle_i2c.c`, `userland/src/camera_*.c`, `tools/camsensor.py`.
- **Refs:** K03; [input-peripherals.md](subsystems/input-peripherals.md).

### T08 Headphones / USB-C audio, earpiece — Audio · Not Started · P1
- **Facts:** USB-C analog headset: FSA4480 switch (on the speakers' I2C bus, deliberately not addressed), WCD938x RX path, MBHC detection. TFA9874 "receiver" profile exists for the earpiece.
- **Next:** MBHC/FSA4480 detection, RX path in `hk_mic.c`-style DSP bring-up; route change in `HKAudio.driver`.
- **Refs:** [audio.md](subsystems/audio.md); K04.

### T09 Fingerprint / biometric authentication — Security · Not Started · P1
- **Facts:** `biometrickitd` and `pearld` are denied; no driver; `MiniAppleKeyStore` models a device with no passcode. The phone has an under-display
  fingerprint sensor per its product specification; its interface has not been investigated in this repository. Real Touch ID semantics (Secure Enclave, `AppleBiometricSensor`)
  cannot be reproduced; a stand-in would have to satisfy `biometrickitd`/LocalAuthentication without real security.
- **Next:** identify the sensor and its bus from the live device tree; decide whether a *functional* (unlock only) emulation is wanted. Design discussion first.
- **Refs:** K02; research/kernel-standins.md (AppleKeyStore).

### T10 Metal: from experimental to usable — GPU/Graphics · In Progress · P1
- **Works:** see [gpu.md](subsystems/gpu.md) "Metal" (blits, MSAA resolve, fences/events/shared events in-process, heaps, 263/271 system fragment shaders draw, Apple Maps pipelines build).
- **Missing:** whole-system Metal is off by default (chronod widgets: 2 pipelines refused); no third-party Metal app tried; no picture compared for Maps; not implemented:
  mesh/object shaders, tessellation, ICBs, tile shading/imageblocks, function tables, indirect draw/dispatch, multiple viewports, depth-clip clamp, vertex amplification, counters,
  texture views, wide lines, per-binding LOD clamps, wrapping caller memory, heap suballocation, cross-process shared events. 135 `air.visible` modules are library functions, not
  entry points. Siri orb abort history.
- **Files:** `userland/src/hkmetal.m`, `hkdrawable.m`, `tools/mesa/air/*`, `docs/air.md`.
- **Next:** make `metal=2` the default only after: chronod pipelines; a Metal app test (e.g. a triangle app exists: `triangle_app.m`); compare Maps visually. Record numbers in air-report.
- **Refs:** K01.

### T11 Heterogeneous CPU scheduling — CPU · Partial (working, uncommitted) · P1
- **Done (2026-10-26, verified on the phone in RAM runs and in QEMU):** Edge scheduler on three clusters (prime, gold, silver), boot-time choice
  `sched=edge|clutch`, performance controller `bsd/dev/hk_perfctl.c` (group placement, deadline/interactive policy, migration graph, clock
  floors, energy model), BG-stays-on-E guard, cross-core priority inheritance, `sched_check`, `kern.hackios.amp`, `sched_test`, `tools/sched-test/`.
  Details and numbers: [scheduler.md](subsystems/scheduler.md). Exported to `xnu/`; **not committed**.
- **Open:** trace the rare 10-11 ms render frame under 7 busy DF threads; measure app launch time (Clutch vs Edge); try the render-server
  inheritance path (never raised by iOS here); redwood's `board_cpu_dpc*` values and a redwood run (only when the owner asks);
  thermal interplay; validate the energy model against a real power measurement (needs battery-only runs, T13).
- **Refs:** [scheduler.md](subsystems/scheduler.md), [cpu.md](subsystems/cpu.md).

### T12 Sleep power: SoC low-power states — Power · Blocked · P1
- **Blocker (architectural):** UEFI never exits; loader core 0 and resident UEFI drivers (storage, USB) hold votes; vdd-min needs the boot core and core 0 power-collapsed,
  RPMh sleep/wake sets. Boot core power-collapse additionally needs a PSCI resume path.
- **Next options:** (a) give storage/USB to the kernel (large); (b) accept WFI-based sleep; decide with measured drain (T13).
- **Refs:** [power.md](subsystems/power.md).

### T13 Battery drain measurement — Power · Not Started · P1
- **Facts:** no number exists for drain asleep or idle awake; `bq27541` current reads exist; runs 6's log covered only the plugged stretch; the owner said stop drain testing on 2026-10-25.
- **Next:** with the owner's consent: a timed sleep with the cable out. 2026-10-27: the meter (`power: meter:` lines, `kern.hackios.power_meter`) and the bench (`drain_bench=1`) are built (kernel `pwr1`, files `hk_power.c`, `instantnoodle_battery.c`, `GenericARMPE.cpp`), **not yet run**; the owner reported about 8% in an hour asleep. Recipe in [power.md](subsystems/power.md) "Measuring the drain asleep". Needed to judge T12, T20, T21.

## P2

### T14 Location (GPS) — Connectivity · Not Started · P2
GNSS belongs to the modem (T06); location via its QMI service 16. `locationd` denied. **Blocked by T06.** Refs: K12.

### T15 App Store, Apple ID, FairPlay, push — Services · Blocked · P2
- **Facts:** App Store LOADING for ever: AMS builds a bag request but the promise never resolves; an AMS thread waits on `fairplayd.H2`, which exits 0 immediately every 10 s
  without touching IOKit (decision made with inlined syscalls). `apsd` runs but has no push courier; `mobileactivationd`: "Device is not activated". No Secure Enclave.
- **Next:** find what `fairplayd.H2` decides on (Ghidra path in `inject_fairplay.c`; kernel `systrace` extension to print sysctl MIB / mac-syscall policy). Probably unachievable without an SEP stand-in.
- **Refs:** research/ios-userland.md §4; K11.

### T16 Brightness: prove the light changes — Display · Partial · P2
- **Works:** the path from `BKSDisplayBrightnessSet` to DCS `51 hi lo` (20/90/5/100% → 0x52/0x67a/0x6/0x7ff), re-sent after wake, default restored on loader return.
- **Missing:** optical confirmation, byte order (no DSI read), auto-brightness (T04).
- **Next:** owner watches the screen during `tools/t brightness`; or implement a DSI read of the panel's brightness register. Record.
- **Refs:** [display.md](subsystems/display.md); K09.

### T17 CPU after wake: widgets and chronod — Performance · Partial · P2
After each wake Weather/Calendar/Stocks widget extensions and chronod burn ~6 s CPU in 14 s (SwiftUI timeline entries). `vmd` (40% of a core) is fixed. Next: profile (`hkprof=2`, `tools/t wakeprof`), consider
denying chronod's widget extensions at wake or throttling. Refs: K10.

### T18 Wi-Fi gaps — Wi-Fi · Partial · P2
Not done: 802.11r (iOS is told FT is absent), TKIP pairwise, roaming, WPA3 other groups and password identifiers, Bluetooth coexistence. Built, not seen on the phone: private Wi-Fi address,
AKM 6, RSNE/RSNXE downgrade check, WPA3 transition mode. Auto-join across reboots needs a persistent `/private/var` run (`var_store=1 ufs_write=1`, or installed boot) — the
Keychain itself works. Files: `wifi/lx/src/lx_wpa.c`, `lx_sae.c`, `lx_80211.c`, `hk_sta.c`. Refs: [wifi.md](subsystems/wifi.md); K05.

### T19 Fast charging (Warp / USB PD/PPS) — Power · Not Started · P2
OnePlus's fast-charge MCU (I2C `0x26`) protocol and firmware unknown; max here is the normal charger's 2000 mA (when T01 verifies the ICL). Redwood reads the charger but does not drive PD/PPS. Refs: K08.

### T20 CPU idle power collapse — Power · Not Started · P2
Awake idle is WFI (clock gated, power on). `CPU_SUSPEND` power-down (`0x40000004`) needs a resume path that would run thousands of times a second; QEMU cannot test it; risk = random hang. Do only with a measured benefit (T13).

### T21 GPU IFPC and measured GPU power — GPU/Power · Blocked · P2
IFPC needs the GMU power config and an OOB handshake around every host register access; mainline does not do it for this GPU either. hwcg is on but the power saved is unmeasured (blocked by T13).

### T22 CoreAnimation fidelity gaps — Graphics · Partial · P2
- `sb3` (function 40, single-pass blur: shadows under cards of the app switcher) is refused by the OpenGL ES renderer; `glsb3=1` hands a shader but still "refused x1".
- Poster switcher (long press on lock wallpaper) draws black; `poster_switcher_allowed()` hides it; cause unknown.
- Control Center glyph strokes at opacity < 1 come in only at the end (`cc_fade` is a check-only workaround, off).
- Switcher wallpaper wash uses a placeholder image.
- Notification banner material (`MTMaterialLayer`: `luminanceCurveMap`, `colorSaturate 2.4`, `gaussianBlur 29.5`) not compared with a real iOS.
- Mesa shader cache disabled at build.
Files: `userland/src/eagl.c`, `inject.c`, `inject_sb.c`. Refs: research/ios-userland.md §1-2; K09.

### T23 Display late swaps and cold-start jank — Display · Partial · P2
~2% of swaps a refresh late (one push in flight); first gesture set after boot ~7% late (cold); unlock-after-wake blur transition 14 ms GPU/frame; idle home screen still ~30 fps drawn. Files: `MiniIOMobileFramebuffer.cpp`, `eagl.c`. Refs: display.md.

### T24 Speaker protection (TFADSP) and other microphones — Audio · Partial · P2
TFA9874 protection runs in Android's audio DSP (AFE module `0x1000B911` on `TERT_MI2S_RX`, VI on `TERT_MI2S_TX` `0x1000B912` topology `0x1000B909`, container messages via
`AFE_PARAM_ID_TFADSP_RX_CFG`); here a software guard stands in. Only the main mic is set up. Amp health polarity only via MANALARM/MANSTATE. Refs: audio.md; K04.

### T25 Video / Live Photo recording; camera parity — Camera · Not Started · P2
README lists video and Live Photo recording as not working (redwood). No encoder path is built (software HEVC decode only is known: `VCPHEVC.videocodec`). Depends on T07 for OnePlus.
- **2026-10-27 (OnePlus 8 Pro, measured on the phone, `userland/src/vt_test.c`, runs vid1/vid3):** VideoToolbox lists only h263/jpeg/sjpg; `avc1` fails with -12908 (kVTCouldNotFindVideoEncoderErr) at the first frame, `h264.rtvc.sw` and `h264.rtvc` too, `jpeg` cannot create a session (-8888). `hvc1` works only through the software VCPHEVC encoder `inject_media.c` registers in mediaserverd, and is **1.4-2.1 frames/s** (720p/1080p): no real-time recording from Apple's codecs. A recording needs our own H.264 encoder behind VideoToolbox (a VTVideoEncoder plug-in registered with `VTRegisterVideoEncoderWithInfo`, or the VTCompressionSession entry points interposed in mediaserverd); an intra-only baseline CAVLC encoder (I16x16 DC, IDR every frame, slices over 8 cores) is about 600 lines and ~30 Mbit/s at 720p30; nothing of it exists yet. The laggy viewfinder was the ISP (numbers: `camera_isp.c`'s `linecache` comment and the `redwood: camera: timing:` log lines): 31 ms a frame -> about 4.5 ms averaged per sensor frame with the cell-row cache and a 30/60 fps pacing.
- **Flash for photographs (2026-10-27, built, not yet verified by eye):** `camera_device.c` lights the LED (`/dev/hk-torch` at 1000, i.e. 125 mA a channel) for a photograph asked for with FlashMode ForceFlashOn (or Auto in the dark), see the comment above `still_wants_flash()`; the settings dictionary of every photograph is logged (`stream: a photograph's settings:`) because the exact key is from CMCapture's strings, not from a captured request.

### T26 Storage — Storage · Partial · P2
Transactions for a writable APFS (the in-place write and the slot store exist; `tools/apfs/txn-probe.py` measures why transactions are hard); no `fsck_hfs`; HFS write speed limited by the loader's read-back;
`ufs_native=1` driver is read-only and used on redwood only; Block I/O 2 gives no asynchrony. Files: `apfs_write.c`, `hk_ufs.c`, `hk_ufshc.c`. Refs: [storage.md](subsystems/storage.md).

### T27 Boot menu usability — Boot · Partial · P2
5 s countdown too short to choose Developer mode by hand; text console only ("the loader's own graphics are a later step"). Files: `xnuloader/flashboot.c`. Refs: INSTALL.md.

### T28 Security model — Security · Partial · P2
AMFI and Sandbox stand-ins accept everything; `MiniAppleKeyStore` models a never-locked no-passcode device and protects nothing; no Secure Enclave; no passcode support; kernel runs whatever
it is given. Documented honestly as stand-ins ([kernel-standins.md](research/kernel-standins.md)). Not a roadmap item to "finish" unless a security goal is declared.

## P3

### T29 Remove stand-ins and copies — Maintenance · Partial · P3
`hk_wlfw.c` ("A STAND-IN, TO BE THROWN AWAY when ath11k's own qmi.c runs here" — it does now with `wifi_lx`); `hk_adsp.c` is a copy of `hk_wifi.c`'s approach ("a copy, not a shared library, for now");
`hk_haptic_pretend()` test stand-in; `hk_sparse.c`/`apfs_check` bring-up aids; `hk_hfs` fetched at build time.

### T30 Redwood re-verification and parity — Redwood · Not Started · P3
The owner said not to run POCO checks. Redwood lacks `gpu_pt`, DCVS, fence IRQ, system sleep, interrupt-driven touch; dual boot is verified only in QEMU. Needs an explicit request.

### T31 Documentation and repository debts — Docs · Partial · P3
`docs/metal.md` is cited 7x by `air.md` and exists nowhere (write it from the source comments or fix the cites); 43 `kern.hackios.*` sysctls undocumented individually; 49 boot-args only described by
file headers; no CI (removed); `uefi` rebuild-from-scratch unverified; `userland/build.sh:8` and `init.c`/`var_store.c` still say `/redwood`; kernel log lines still prefixed `redwood:` in userland;
`ios-instantnoodle/` copy-pasted READMEs (sweet/guacamole); two-repo index overlap (see architecture.md). Also: `kern.hackios.*` and `rc.jobs.*` indexes.

### T32 Scripted gesture reach and test coverage — Testing · Partial · P3
Scripted touches do not reach iOS system gestures (swipe-up for home/switcher) except from the real bottom edge path; `com.hackios.switcher` returns but changes nothing with 3 apps. `tools/t` covers boot/pods/frames/sleep/powerd/darkwake/brightness/idle;
no automated test for audio, Wi-Fi join, GPU hang recovery (manual `rc.jobs.*`).

### T33 Thermal service never seen throttling — Thermal · Built, Unverified · P3
27 sensors answered at 33-38 C on redwood; the governor has not slowed cores on either phone (needs >95 C load). Next: a controlled heat test with the owner. Refs: thermal.md.

### T34 NFC, UWB, other hardware — Hardware · Not Started · P3
`nfcd` denied; Wallet UI exists without NFC. Nothing investigated.

### T36 iphont agent hardware validation and workflow migration — Tooling · Partial / Built, Unverified · P1
The Python CLI, native loader/kernel mailbox, event logging, userspace bridge, diagnostics, input scheduling and capture interface are implemented and compile. Host/C tests and real XNU QEMU queries pass; physical state/log/framebuffer capture works. The owner explicitly deferred the required RAM loader reload, so USB RPC, daemon/framework/entitlement behavior, touch/keyboard delivery, and full deployment/recovery parity remain unverified. Normal boot profiles are unchanged. Next: use the supervised RAM sequence in [TOOLKIT.md](TOOLKIT.md), then validate one operation at a time. Still unfinished: GPU queue-level introspection, delivered keyboard guarantees, orientation/sensor support, KDP/LLDB integration, high-rate compressed mirroring, userspace function symbols, service-specific regressions and complete legacy replacement. Keep all unique recovery/test/build scripts; [TOOLKIT-MIGRATION.md](TOOLKIT-MIGRATION.md) records the static inventory and deletion gates.

### T37 UI smoothness — Display / performance · Partial (main cause fixed in `perf1`, built; owner: "much snappier") · P1
- **2026-10-27 perf session, cause found by measurement:** the OnePlus 8 Pro's known-good boot-args had no `cpufreq=1` (nor has `instantnoodlep.conf`), and `hk_cpufreq.c` does nothing without it: the gold and prime clusters stayed at EPSS row 0 (710 MHz and 844 MHz; `perfctl:` printed "0 MHz" per cluster), no memory-bandwidth vote, no governor, no perfctl clock floors. Baseline run (kernel `perf0`, `logs/perf/base1.live`): all 7 cores 99-100% busy during the first swipe, `rwtouch idle 60 30` never settled, first gesture 55.8 fps with 32% of frame steps over 16.7 ms (window-server draw 15.4 ms). Kernel `perf1` (`hk_cpufreq_enabled()`: cpufreq on by default for `board=instantnoodle*`, `cpufreq=0` for A/B; files `hk_cpufreq.c`, `hk_perfctl.c`): rows 0 -> 17/19 (2419/2841 MHz), governor on; **owner watched the run: "much snappier"**. Frame-time numbers for `perf1` were NOT captured (run stopped by the owner/app before the gestures); a before/after with `tools/frames.py` is still to do. **Not done:** 120 Hz (`fb_hz=120` is the existing, log-verified mechanism; userland follows it automatically, `ca_vbl`/`ca_fps`; needs a measured run), `ca_vbl=1` at 60 Hz (CoreAnimation uses its own timer at `fb_hz=60`, not the panel TE: untested suspect), idle `top` to see what still keeps cores busy (`userland/root-perf2.apfs` has the job: `power_test top/kprof/cpu` at idle, then scripted gestures). Details: [display.md](subsystems/display.md) "Smoothness".
- **Owner (2026-10-27):** the interface drops a lot of frames and the phone feels slow. It may just be that the build runs from RAM (the system image is a RAM disk with compressed APFS): judge again once the installed (flash) boot works. Measure first (frame times, CPU/GPU load per cluster), then act; related: T11, T23, [display.md](subsystems/display.md).

### T38 Native panel resolution — Display · In Progress · P1
- **State:** iOS runs at the iPhone X's pixel size (the userland is an iPhone10,3's). The owner wants the panel's native resolution (the OnePlus 8 Pro is 1440 x 3168, QHD+). **2026-10-27, owner-verified in a RAM run:** iOS renders at 1320 x 2868 (iPhone 16 Pro Max size; boot-args `fbwidth=1320 fbheight=2868`), GPU-scaled to the panel's FHD+ mode; home screen and status bar (clock, battery) correct; Wi-Fi radio and Bluetooth up beside it. UIKit has no status bar layout for that size and drew the pink "rdar:45025538" Fallback bar: `userland/src/inject_ui.c` (`libhackios-ui`, in SpringBoard, Preferences, MobileSafari, MobileNotes, Maps, MobileSMS, Camera) keeps UIKit's answer and, only for the Fallback, answers the Split1284 notch layout. **QHD+ scoping (from the live DT `op8pro-data/research/instantnoodlep-live.dts`, node `qcom,mdss_dsi_oppo19811_samsung_1440_3168_dsc_cmd`; FHD+ = timing@0/@1, QHD+ = timing@2 120 Hz/@3 60 Hz):** QHD+ is not a timing change. DSI clock 825 MHz (0x3135a800) -> 1.457 GHz (0x568bc300): the DSI PHY PLL and pixel clocks must be reprogrammed; DSC goes from 8 bpc, slice 540 x 72 to **10 bpc**, slice 720 x 96 (two encoders, 8 bpp), so the DSC encoder registers and the panel's PPS change and the MDP must feed 10-bit; VIG/LM/INTF/PINGPONG sizes 540 -> 720 wide and 2376 -> 3168 high; the Iris chip (`pxlw,iris-chip-enable`, `soft-iris-enable`) is listed in this node and its mode setup is unknown (vendor-private). Never written blind. **Read-only dump done (`fb_probe=2`, 2026-10-27, no crash; raw log `logs/res/probe2.log`, untracked).** What the firmware leaves for FHD+ 60 Hz: both DSC encoders identical, picture 1080 x 2376 (SIZE 0x04380948), slice 540 x 72 (0x021c0048), chunk size 0x20e; DSI0 command-mode stream word count 0x439 (= 2 slices x 540 bytes + 1) x 2376 lines (reg +0x58/+0x5c: 04390039 09480168); INTF1 timing registers all zero (command mode); DSI0 PLL/PHY values captured (the 825 MHz setup); DISPCC branch clocks on. For QHD+ the same registers become: stream word count 2 x 720 + 1 = 0x5a1, 3168 lines, picture 1440 x 3168, slice 720 x 96 at 10 bpc (new rate-control table by the DSC spec and a new PPS packet to the panel), PLL 1.457 GHz. **DSC derived and checked (`fb_dsc=2`, 2026-10-27, owner-run):** the VESA DSC 1.1 rate-control rules plus the DT's slice/bpc/bpp numbers reproduce all 148 firmware words of the two FHD+ encoders (layout of the encoder registers in the comment above `dscDerive`, `MiniIOMobileFramebuffer.cpp`); writing the derived set back (73 words per encoder, same values) read back clean and the picture was unaffected, so DSC register writes work. QHD+ sets and PPS are computed and logged (qhd120: 10 bpc, 8 bpp, slice 720x96, initial dec/xmit 616/512, scale 32/10/2739, bpg 259/204; qhd60: 10 bpc, 10 bpp, slice 720x16) but **unverified**: the 10 bpc range tables (VESA) and the 10 bpp offsets (initial_offset 6144 is the 8 bpp value) are not checked against anything, first_line_bpg_offset 12 is the firmware's FHD+ value. The PPS goes to the panel as DSI type 0x0a; the panel's current PPS cannot be read back. Remaining blocks, each a write function behind its own boot-arg: DSI0 PLL 825 MHz -> 1.457 GHz, panel timing-switch DCS (timing@2/@3), DSC/PPS for QHD+, VIG/LM/PINGPONG/DSI-stream sizes (720 wide, 3168 high; stream word count 0x5a1), the Iris chip's mode (unknown), larger framebuffer pitch/buffers. The ordering is the risk: the panel must be switched first or last in step with the pipeline, and there is no way back except a reboot. **First QHD+ attempt (`fb_qhd=1`, 2026-10-27, owner-run) FAILED and left the panel black until a force-reboot:** the panel took timing@3's switch (DCS 0x60=00, 0xba=03, 0xf7, 0x2a/0x2b to 1439/3167, compression 07 01) and a DSC PPS (type 0x0a), the pipeline writes (both DSC encoders, DSI0 stream +0x58/+0x5c = 0x07090039 / 0x0c600258, VIG/LM 720 x 3168, INTF1 tear lines 3168) were accepted, but the CTL flush stayed pending (0x20003) and INTF1's frame counter never moved for 20 s: no TE from the panel (or a pipeline that waits for it), so no frame was sent. Sending the FHD+ switch and PPS again and writing all saved registers back did NOT bring TE back; the panel stayed black although the phone and iOS ran on. Not known: whether the panel wants the Iris chip put in step (the DT names it for this mode), a display-off/on or reset cycle, the PPS before the compression command, or DSC values it rejects (10 bpc tables are the VESA's, unverified). Lessons: a QHD+ attempt must keep a way to bring the panel back without a reboot (untried: DCS 0x28/0x29 cycle, a TE-less recovery); do not re-send the FHD+ switch alone. Next (low risk): confirm the 10 bpc tables from the Samsung panel's own behaviour is impossible offline, so the first QHD+ attempt must be a throwaway run where a broken picture costs only a reboot **2026-10-27 later (see INSTANTNOODLEP-PLAN.md addendum 46 notes):** `fb_qhd=2` ladder: TE stays 60 Hz after the panel switch and after the pipeline resize; with `fb_qhd_flush=1` (mixer, DSC and INTF flush bits too) the first push completes but shows snow (DSC sets differ); the panel's own DSC sets for timing@2/@3 are in the device tree's on-command (`kQhd60Pps`, `kQhd120Pps`) and are used by `fb_qhd_pps=2`; then 120 Hz needs the PLL (825 MHz -> 1.457 GHz). **Not done:** the panel itself still runs FHD+ (1080 x 2376); QHD+ needs the panel's timing@2/@3 DCS sequence plus DSC, layer mixer, INTF and framebuffer-pitch changes (DT in the plan, addendum 45); other apps need the library injected (kernel `exec_inject` is exact-name, 512 bytes).

### T41 Status bar and lock screen fit the 8 Pro's camera hole — Display / UI · Owner-verified (RAM boot), not flashed · P2
- **State (2026-10-28):** `inject_ui.c` moves the clock clear of and level with the hole and the icons nearer the screen's edge (DynamicSplit hooks, `HK_SB_CUTOUT_RIGHT`, `HK_SB_CLOCK_UP`), `inject_sb.c` hides the lock screen's Control Center coaching grabber (the cyan bar under the battery; cause inferred). Image `userland/root-sb2.apfs` (small image + the two libraries; sb1 only had the FixedSplit hooks and changed nothing; boot image `out/instantnoodlep-standalone12-sb2.img` = kernel ufs25 + loader-8pro-quiet, booted with `fastboot boot`, NOT flashed). Details: `docs/subsystems/userland.md`.
- **Check on the phone:** boot log lines `libhackios-ui: status bar geometry ON ...`, `leadingCenteringEdgeInset a -> b`, `lock screen Control Center coaching: hidden ok, made hidden ok`; home screen: clock fully visible, vertically on the hole's centre; battery near the right edge; lock screen: no bar. Tune: `HK_SB_CUTOUT_RIGHT`, `HK_SB_CLOCK_UP` (points).
- **Open:** applications without `libhackios-ui` (Camera, Photos, App Store...) still draw the clock under the hole (the library is injected in the INJECT list's programs only; Camera crashed with it: build.sh note); a lone battery stays mid-ear (UIKit centres the items; the group's right edge is right with Wi-Fi + cellular); the sa1 image's `ext/HKMetal.bundle` and `hal/HKAudio.driver` symlinks point at nonsense paths (earlier repacks with 7z prefixed their targets; kept as they were in root-sb1).

## Complete — do not reopen (older documents made these look unfinished)
- Speakers on the OnePlus 8 (heard by the owner 2026-10-24); the alert slider; the main microphone (owner's claps); App Library grey pods (fixed 2026-10-25); iOS shadows "too large" (owner: resolved);
  `ca_ambient` idle cost; `vmd` spin; shared-interrupt-after-wake bug; GPU block collapse in sleep (verified 2026-10-26); per-program GPU page tables; hang recovery; cold `gpu=1`; installed boot of the OnePlus 8 (verified 2026-10-24);
  Wi-Fi join incl. WPA3 and TKIP-mixed on the OnePlus 8 (verified in RAM runs); HFS+ `/private/var` (verified); Metal fences/events/heaps (metal_sync_test 10/10).
- The README's old "alert slider: no driver" and "microphones: not yet" were stale; "Wi-Fi: a network cannot be joined from Settings yet" in `userland/build.sh`'s WIFID comment is stale too.

## Source-comment markers that are real work (inventory)
`hk_wlfw.c` stand-in; `hk_adsp.c` copy; `apfs_write.c` ("no caller in the kernel yet"); `instantnoodle_slider.c` interrupts not used (polled); `instantnoodle_speaker.c`/`hk_speaker.c` "what is not done compared with Android"
(DSP algorithm, boost current limit by battery); `mc_aes.c` no XTS/GCM; `MiniIOSurface` child surfaces/wrapping/purgeability/compression/notifications; `hkmetal.m` ~40 features; `GenericARMPE.cpp` no RTC driver (time from the loader / `xl settime`). The code has essentially no `TODO`/`FIXME` markers: unfinished work is written as prose in headers.


### T47 Wi-Fi auto-join, sleep, AirDrop, sensors, charging — OnePlus 8 Pro · **Open** (2026-10-29)
- **Wi-Fi auto-join (K44):** wifid hears the saved network and never sends ASSOCIATE; every scan/SCAN_DONE variant ends in `AUTO-JOIN: Scan FAILED`. Next: find what corewifi's auto-join scan waits for (decompile `WiFiDeviceManagerStartAutoJoinAndReply`/CoreWiFi scan with ghidra, or compare with an open Apple80211 driver's request set: itlwm/AirportItlwm), the meaning of requests 419/365/423/115/97/96.
- **Sleep (K45):** make `syssleep=1` safe: Wi-Fi radio full re-bring-up after a wake (RDDM crash today, and Bluetooth re-downloads at the same time), the second sleep, the modem (its service restarts it).
- **AirDrop:** needs AWDL (`awdl0`, wifid's p2p, Apple80211 types for it): not started. **Sensors:** accelerometer, gyro, compass, light, proximity, barometer and GPS are behind the sensor DSP (SLPI: firmware through TrustZone like the ADSP, then its sensor QMI) and the modem's LOC service for GNSS: not started.
- **Charging:** works as reported by the gauge; wall-charger input limit (DCP 2000 mA, CDP 1500 mA) never run on a wall charger; design capacity and cycle count read 0; Warp/PD not implemented (T19).
- **Status icons (K46):** half-alpha rendering and a pulsing searching animation on the signal icon.
