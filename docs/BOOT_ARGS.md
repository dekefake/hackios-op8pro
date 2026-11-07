# Boot arguments, sysctls and related knobs

Boot-args are given on the kernel command line: `XL_CMDLINE="..." ./xl run`, the profile's `BOOTARGS`/`FLASH_BOOTARGS`
(`common/devices/*.conf`), `CMDLINE=` for QEMU, or `--cmdline` of `xnuloader/mkpayload.py`. The kernel reads them with
`PE_parse_boot_argn()`; user space reads `sysctl kern.bootargs`. This page is **generated from the source** (202 kernel
names via `PE_parse_boot_argn`/`PE_parse_boot_arg_str`, plus the userland ones) and annotated by hand; names marked
"see the header comment" are diagnostic or tuning knobs whose only documentation is the comment at the top of the named file.
**When you add a boot-arg, add it here** (see [../AGENTS.md](../AGENTS.md)). Regenerate the name list with:

```sh
grep -rhoE 'PE_parse_boot_arg(n|_str)\( *"[A-Za-z_0-9]+"' xnu/files | sort -u
```

Rules of thumb: a bare `./xl enter` or a bad arg can hang a phone; **never pass `touchscreen=1` on the OnePlus 8**; `wifi_again=1` /
`adsp_again=1` have reset the phone; `sound_amps=1` makes the speakers audible. Per-device defaults live in the profile.

## Parsed by the loader (not the kernel)

| Arg | Meaning |
|---|---|
| `cpus=N`, `cpumask=M` | also read by the loader to know the kernel will start more cores (`cpumask` picks cores by position, bit 0 first) |
| `gamut=p3` | test: leave the iPhone's "P3" display gamut in the device tree (default sRGB, see display.md) |
| `ufs_probe=1` | loader probes Block I/O 2 behaviour before the kernel starts |
| `board=NAME` | the loader adds `board=` for the profile's board; `board_<block>=0x...` overrides come from `BOARD_ARGS` (`bsd/sys/hk_board.h`: `epss`, `cmd_db`, `rsc`, `tsens0/1`, `gcc`, `dispcc`, `intf`, `ufshc`, `spmi_*`, `thermal_gpuss_0/1`, `cpu_domains`, `small_cores`) |

## Kernel

### `bsd/dev/hk_adreno.c`

| Boot-arg | Meaning |
|---|---|
| `adreno` | Run bring-up steps 1..N and stop. |
| `adreno_blind` | Step 20 with microcode nowhere (diagnostic). |
| `adreno_dirty` | Leave the GPU as a panic leaves it. |
| `adreno_fine` | Wait before every single access. |
| `adreno_keep` | With adreno=21: keep the GPU for the system (HKGPU). |
| `adreno_only` | Run step K alone. |
| `adreno_pause` | ms the newest step waits after announcing (default 2200). |
| `adreno_return` | Return to the loader when steps are over. |
| `adreno_skip` | Bitmask of steps to skip. |
| `adreno_speed` | Speed level asked of the GMU in step 17. |
| `adreno_stream1` | Second stream handling in step 18 (diagnostic). |
| `adreno_watch` | Seconds of "still alive" lines after the last step. |
| `display_probe` | Read what the firmware left the display controller doing. |
| `gpu` | 1: start the Adreno for the window server (needs fw-gpu and a GL=1 image). |
| `gpu_cycle` | Boot test: N GPU stop/start cycles before anything uses it. |
| `gpu_dcvs` | GPU speed follows load (default on kona; 0 = 525 MHz). |
| `gpu_exact` | *diagnostic/tuning knob: see the header comment of this file* |
| `gpu_hot` | DCVS holds 305 MHz from this temperature (default 85.0 C). |
| `gpu_hwcg` | 0 turns hardware clock gating off. |
| `gpu_irq` | GPU fences raise an interrupt (GIC 332; default on kona). |
| `gpu_log` | *diagnostic/tuning knob: see the header comment of this file* |
| `gpu_pt` | Per-program GPU page tables (default on kona). |
| `gpu_recover` | Reset and restart a hung GPU (default on kona). |
| `gpu_va` | *diagnostic/tuning knob: see the header comment of this file* |

### `bsd/dev/hk_adsp.c`

| Boot-arg | Meaning |
|---|---|
| `adsp` | 1: start the audio DSP and its services. |
| `adsp_apr_wait` | APR wait. |
| `adsp_listen` | Seconds step 10 listens for QRTR services. |
| `adsp_new` | Steps from K on are unproven; pause after each. |
| `adsp_pause` | ms pause after announcements. |
| `adsp_step` | Run DSP steps 1..N and stop. |
| `adsp_wait` | Seconds step 7 watches for "up". |
| `battmgr` | 0: no DSP battery manager. |
| `battmgr_notify` | DSP battery manager notifications. |
| `battmgr_xm` | DSP battery manager extra mode. |

### `bsd/dev/hk_apfs_check.c`

| Boot-arg | Meaning |
|---|---|
| `apfs_check` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_chunk` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_contents` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_depth` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_dev` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_hash` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_max` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_mnt` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_skip` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_store_len` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_store_note` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_store_seed` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_subtree` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_threads` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_volume` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_volume_at` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_walkers` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_write_at` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_write_byte` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_write_len` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_write_path` | *diagnostic/tuning knob: see the header comment of this file* |

### `bsd/dev/hk_audio.c`

| Boot-arg | Meaning |
|---|---|
| `rec_sound` | Record the first N seconds of played sound (xl sound). |
| `rec_sound_fed` | Record what the guard passed. |
| `rec_sound_mic` | Record what the main mic hears. |

### `bsd/dev/hk_battery.c`

| Boot-arg | Meaning |
|---|---|
| `battery` | redwood: 1 Qualcomm UEFI charger state shared by the loader; instantnoodle: 0 turns the gauge off. |
| `battery_shared_pa` | Physical address of the loader's charger page. |

### `bsd/dev/hk_buttons.c`

| Boot-arg | Meaning |
|---|---|
| `board` | Device board. `instantnoodle` enables the OnePlus 8 drivers (touch, battery, slider, speakers, Wi-Fi PCIe, ...); the loader passes it. |
| `buttons` | redwood: 1 power/volume key driver. |
| `buttons_steps` | Stop key bring-up after N steps. |
| `buttons_wake` | Power-key wake mode. |
| `touchscreen` | redwood: 1 starts the Goodix/FocalTech driver. NEVER on the OnePlus 8. 0 turns off the OnePlus driver. |

### `bsd/dev/hk_camera.c`

| Boot-arg | Meaning |
|---|---|
| `cam` | 1..5 camera bring-up levels (5 = for iOS, redwood). |
| `cam_after` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_fake` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_fake_fps` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_frames` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_focus` | OnePlus 8 Pro: the main camera's lens to iOS position N (0 furthest .. 1000) once the camera is up (`instantnoodle_lens.c`). |
| `cam_lensscan` | OnePlus 8 Pro: `1` = after 90 frames the lens sweeps its codes 0..1000 in steps of 32 and logs `lens scan: code C sharpness S` (the frames' own sharpness), then keeps the sharpest; iOS' focus commands are ignored meanwhile. |
| `cam_lensmax` | OnePlus 8 Pro: the lens' soft limit (largest code, 1..1014; default 1000). |
| `cam_gain` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_keep_mb` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_mode` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_pattern` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_sensor` | wide selects the wide sensor in early modes. |
| `cam_settle` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_shutter` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_vblank` | *diagnostic/tuning knob: see the header comment of this file* |

### `bsd/dev/hk_cpufreq.c`

| Boot-arg | Meaning |
|---|---|
| `cpufreq` | 1: raise every used EPSS group to its top row and enable the governor/thermal service. **Default on for `board=instantnoodle*`** (kernel `perf1`, 2026-10-27; without it the OnePlus 8 Pro's big cores stayed at 710-844 MHz); `cpufreq=0` turns it off. Other boards: off unless given. |
| `cpufreq_gov` | Governor on (default with cpufreq=1); 0 = groups stay at their cap. |
| `cpuload` | *diagnostic/tuning knob: see the header comment of this file* |
| `ddr` | *diagnostic/tuning knob: see the header comment of this file* |
| `ddr_dark` | Memory vote with the screen off (1024). |
| `ddr_idle` | Memory vote after a second idle (1024). |
| `ddr_rest` | Memory-vote rest value (see hk_cpufreq.c). |
| `ddr_sleep` | Memory vote in system sleep (0 = none, floor). |
| `ddr_vote` | RPMh memory vote while busy with the screen lit (default 16383). |
| `gov_start` | Seconds after boot the governor takes over (default 45; tests use 30). |
| `membench` | Expose kern.hackios.membench (all cores writing, ~1 s). |

### `bsd/dev/hk_perfctl.c`, `hk_sched_check.c`, and the scheduler (`osfmk/kern/sched_*.c`, `osfmk/arm64/machine_routines.c`) — see [subsystems/scheduler.md](subsystems/scheduler.md)

| Boot-arg | Meaning |
|---|---|
| `sched` | `edge` (default where the cores fall in more than one frequency domain: both phones) or `clutch` (the previous scheduler, one processor set: the regression baseline). `edge` on a one-domain board (QEMU) runs Edge on one cluster. |
| `board_cpu_domains` | Frequency domain (= cluster) of each core number, 4 bits a core, lowest first. Default `0x21110000` when the loader names small cores (both phones), else one domain. Also used by `hk_cpufreq.c`. |
| `board_small_cores` | Mask of core numbers that are E cores, beside the loader's `small-core` (QEMU tests: `0x0f`). |
| `board_cpu_dpc<D>` | Energy model coefficient of domain D, µW/MHz/V² (default SM8250's DT: 100, 533, 642). |
| `perfctl` | 0: no performance controller (Edge with its static graph; every group prefers the boot cluster). |
| `perfctl_log` | 1: log every recommendation change (`perfctl: group ID NAME: class LEVEL -> LEVEL ...`). |
| `sched_check` | 1: the kernel's scheduler checks a few seconds after every core is up (`sched_check: PASS|FAIL ...`). |
| `sched_edge_bg_up` | 1: background-QoS threads may migrate/be stolen from E to P (Apple's code); default 0 keeps them on E. |
| `sched_edge_promote` | 0: no cross-core priority inheritance (a turnstile-promoted thread keeps its group's E preference). |
| `small_cores` | Under `sched=clutch` only: 0 all cores alike, 1 Apple's SMT-style pairing, 2 (default) slower cores as secondaries of their own. |
| `small_cores_after` | Seconds before the slower (E) cores start (default 28; 0 at once). |

### `bsd/dev/hk_display.c`

| Boot-arg | Meaning |
|---|---|
| `fb_scan` | redwood: tear-free copy by panel scan, 120 Hz. |

### `bsd/dev/hk_goodix.c`

| Boot-arg | Meaning |
|---|---|
| `touch_flip` | redwood touch axis flip. |
| `touch_lat` | 1: log latency lines for every touch sample. |
| `touch_poll_us` | redwood: interrupt-line poll period. |
| `touch_rate` | redwood touch rate. |
| `touch_spi_hz` | redwood: touch SPI clock. |

### `bsd/dev/hk_haptic.c`

| Boot-arg | Meaning |
|---|---|
| `haptic` | 1..4 probe/play, 5 = for iOS. |
| `haptic_effect` | Effect number for haptic=4. |

### `bsd/dev/hk_mic.c`

| Boot-arg | Meaning |
|---|---|
| `mic` | 1 look, 2 codec on SoundWire, 3 listen, 4 feed iOS. |
| `mic_adc` | Analog ADC selection. |
| `mic_input` | Mic input selection. |
| `mic_main` | Main mic selection. |
| `mic_secs` | Seconds for mic=3. |
| `mic_tap` | Test: mic hears the haptic motor. |
| `mic_test` | Mic test mode. |

### `bsd/dev/hk_net.c`

| Boot-arg | Meaning |
|---|---|
| `net_names` | en0 naming. |
| `net_trace` | Trace en0 frames. |

### `bsd/dev/hk_power.c`

| Boot-arg | Meaning |
|---|---|
| `drain_bench` | 1 (OnePlus 8): the drain bench thread (below; built 2026-10-27, not yet run on the phone). Reads the battery gauge around every sleep (`power: meter:` lines, `kern.hackios.power_meter`). |
| `drain_after` | Seconds after the gauge answers before the bench starts (default 90). |
| `drain_cable` | Seconds the bench waits for the cable to come out (default 600); with it in, the figures are not a drain. |
| `drain_awake` | Seconds of awake drain looked at before the first sleep (default 30). |
| `drain_force` | 1 (default): ask for the system's sleep again when it has been awake `drain_gap` s (default 60) with the panel dark. 0: only iOS decides. |
| `drain_count` | Sleeps after which the bench says "bench: DONE" (default 3); `drain_stop=1` then ends the bench. |
| `gpu_sleep` | 0 keeps the GPU powered through system sleep. |
| `sleep_max` | Cap every system sleep to S seconds (timed runs need it). |
| `syssleep` | 0 refuses system sleep (default on with board=instantnoodle, off elsewhere). |

### `bsd/dev/hk_slpi.c` (OnePlus 8 Pro: the sensor DSP) and `bsd/dev/hk_fastrpc.c` (its file service)

| Boot-arg | Meaning |
|---|---|
| `slpi` | 1 (default on the OnePlus 8 Pro since loader 15, 2026-11-07): start the sensor DSP (SLPI), its SEE client and FastRPC listener; 0 opts out. |
| `slpi_delay` | Seconds after the root is mounted before starting (default 30). |
| `slpi_step` / `slpi_upto` | Run steps 1..N and stop / pause after step N (like `adsp_step`). |
| `slpi_settle_ms` | ms after the DSP says up before the next step (default 300). |
| `slpi_loud` | 1: every step's lines to the loader's log too. |
| `slpi_oplus` | 0: do not fill OnePlus SMEM item 130 (sensor list from `oplus,sensor-devinfo`; without it SEE has no IMU). |
| `slpi_sns` | 0: no SEE client (QMI service 400). |
| `slpi_watch` | Seconds the SEE client streams events after start (default 90; then on-change only). |
| `slpi_hz` | Sample rate asked of streaming sensors (default 10). |
| `slpi_frpc` | 0: no FastRPC listener (SEE then finds no sensors: it cannot read its registry). |
| `slpi_frpc_pd` | Bitmask of DSP processes attached: 1 root, 2 sensors (default 3). |
| `slpi_frpc_trace` | 1: log every FastRPC call served. |
| `slpi_dsplog` | 1: ask the DSP for its own log lines (adspmsgd_adsp1 init2, as Android's adspmsgd); they are logged as `frpc: dsp:`. |

| `als` | 0: no ambient light sensor service in backboardd (default: one, with slpi=1: Auto-Brightness, True Tone; userland `inject.c`). |
| `nightshift` | 0: CoreAnimation's colour matrices (Night Shift, True Tone) are not passed to the display (userland `inject.c`). |
| `cbtrace` | 1: log what CoreBrightness's colour module does with its HID services (userland `inject.c`, a probe). |
| `prox` | 1: a proximity service in backboardd (userland `inject.c`; "Placement" 1 makes backboardd's sensor controller take it, RAM image 188). Off by default until a near/far is seen: SEE has sent no proximity data event yet. |
| `orient` | 0: backboardd's device orientation stays CoreMotion's (userland `inject.c`; default: from the accelerometer when there is one). |

Sysctls: `kern.hackios.sensors` (the sensors SEE reported and their last values), `kern.hackios.sensor_values` (the same as a struct, for `inject.c`), `kern.hackios.fastrpc` (listener state, calls served), `kern.hackios.slpi_charger`.

### `bsd/dev/hk_sound.c`

| Boot-arg | Meaning |
|---|---|
| `sound` | 1 port, 2 tone, 3 audible tone with amplifiers, 4 what iOS plays. |
| `sound_amps` | 0: amplifiers set up but never started (OnePlus default 0; profile passes 1). |
| `sound_below` | Amplifier level in eighths of a dB under Android max (default 240; profile 96). |
| `sound_gain` | Gain before the guard (default/max 96). |
| `sound_guard_as` | Test: guard reckons amps N under. |
| `sound_park` | 0: do not park the audio path when idle. |
| `sound_pause` | Bring-up step pause (ms). |
| `sound_secs` | Seconds for sound=1..3. |

### `bsd/dev/hk_sparse.c`

| Boot-arg | Meaning |
|---|---|
| `apfs_sparse` | Offset of a sparse image. |

### `bsd/dev/hk_speaker.c`

| Boot-arg | Meaning |
|---|---|
| `speaker_probe` | 1 pins, 2 amplifier IDs, 3 amplifiers set up alone. |

### `bsd/dev/hk_thermal.c`

| Boot-arg | Meaning |
|---|---|
| `thermal` | 0 off; 2 watch and report without cpufreq. Default on with cpufreq=1. |

### `bsd/dev/hk_torch.c`

| Boot-arg | Meaning |
|---|---|
| `torch` | 1 look, 2 light briefly, 3 for iOS. |
| `torch_max_s` | Longest continuous light (default 30, max 120). |
| `torch_secs` | Seconds for torch=2. |

### `bsd/dev/hk_ufs.c`

| Boot-arg | Meaning |
|---|---|
| `darwin_blocks` | Size of darwin in blocks. |
| `darwin_file` | darwin as a file of userdata (extent map). |
| `darwin_lba` | First LBA of darwin. |
| `darwin_part` | Name of the port partition. |
| `hk_data` | 1: /dev/hk_data over userdata blocks 3145728..end; 2: RAM disk. |
| `hk_data_mb` | RAM-disk size for hk_data=2. |
| `ufs_bench` | Storage benchmark. |
| `ufs_check` | Hash partitions with reference images. |
| `ufs_native` | 1: kernel's own read-only UFS command list (hk_ufshc.c). |
| `ufs_native_bench` | Native benchmark. |
| `ufs_peek` | 1: dumps the UFS host controller's first 0x100 bytes of registers at boot (`ufshc_peek()`, read only; for the OnePlus 8 Pro, whose UEFI has no UFS driver). |
| `ufs_8pro` | 1: the OnePlus 8 Pro's own UFS backend (`instantnoodle_ufs.c`, read only): the controller adopted as ABL left it, SMMU stream 0xe0 through a context of ours, START STOP UNIT, a 1 MB data buffer; the partitions of LUN 0 from its GPT become `/dev/ufs_NAME` and `/dev/rufs_NAME` (no loader page needed). Verified on the phone 2026-10-27 (256 MB in 305 ms). |
| `hk_fast_halt` | 1 (a standalone boot): iOS's shutdown/restart (`reboot(2)`) syncs the var store and the data volume and goes straight back to the loader (reason 3: the loader switches off; 4: it resets), instead of XNU's own unmount sequence, which stalled on the HFS data volume. |
| **Defaults (OnePlus 8 Pro, standalone images, since 2026-11-02)** | Everything checked to work is on without being asked: `default_args` of `platform_instantnoodlep` in `xnuloader/loader.c`. An image's own line names only what is its own (`epoch=`, `wifi_mac=`, the device agent's `iphont=1 iphont_control=1 exctrace=1`). **Opt out by naming it:** `wifi=0`, `bt=0`, `sound=0`, `modem=0`, ... (the default of that name is then left out). `hk_defaults=0` leaves out all of them (the profile's BOOTARGS for runs over `xl`). The line may be 1024 bytes (`BOOT_LINE_LENGTH`, kernel and loader together: a loader and a kernel of different sides of this change do not fit); what does not fit is said in the log |
| `hk_slot_ok` | `=1`: at start, the running slot marked successful in LUN 4's GPT (once; nothing written after that), or ABL's retry count runs out and it starts the other slot |
| `kern.hackios.ufs_peek` (sysctl), `hk_lun` | read one block of LUN 0, 4 or 5, for GPT backups; root only |
| `kern.hackios.modem_restart` (sysctl) | write 1: a test: the modem started again from scratch as if its link had gone |
| `hk_swap_slot` | **Since 2026-11-02:** `=1` the menu's Android: only the active slot marked unbootable, ABL makes the other active itself; `=2` only says what it would do. Back: `fastboot --set-active=a`. **Was, disabled 2026-10-28** (the boot menu no longer has "Android (other slot)"; `instantnoodle_ufs_ab_swap()` refuses with EPERM): exchanging only the A/B attribute bytes of boot_a/boot_b left them disagreeing with the partition type GUIDs that ABL's `fastboot --set-active` moves with them, and the phone would not boot until its LUN 4 table was rebuilt over EDL. Change slots with `fastboot --set-active=a|b`. |
| `ufs_8pro_test` | 1 (with `ufs_8pro=1`): at boot, reads the first bytes of a few partitions, checks `super`'s liblp magic and an unaligned read, times 256 MB; into the loader log (`ufs: 8pro test: ...`). |
| `ufs8` | 1..9: experiments with the 8 Pro's controller (`instantnoodle_ufs.c`): 4 register dump, 5/6/7 controller/PHY/device re-initialisation, 8/9 SMMU stream entries (9 is the working recipe, read only). `ufs8pa=ADDR` places the area of mode 1. |
| `ufs_native_check` | Native driver self-check. |
| `ufs_shared_pa` | Physical address of the loader's storage page (loader passes it). |
| `ufs_write` | 1: allow writes to /dev/rufs_darwin past BLOCK_WRITE_FIRST. |
| `var_save_ms` | Kernel-side wait for the var saver on return (ms). |

### `bsd/dev/hk_ufshc.c`

| Boot-arg | Meaning |
|---|---|
| `ufs_native_buffer` | Native data buffer size. |
| `ufs_native_hang` | Native hang test. |
| `ufs_native_leave` | Leave native list in (diagnostic). |
| `ufs_native_slots` | Native slots used. |
| `ufs_native_spin` | Native spin test. |

### `bsd/dev/hk_virtio_blk.c`

| Boot-arg | Meaning |
|---|---|
| `virtio_blk` | QEMU virtio block. |

### `bsd/dev/hk_virtio_input.c`

| Boot-arg | Meaning |
|---|---|
| `virtio_input` | QEMU virtio pointer/keyboard. |

### `bsd/dev/hk_virtio_net.c`

| Boot-arg | Meaning |
|---|---|
| `virtio_net` | QEMU virtio network (en0). |

### `bsd/dev/hk_virtio_sound.c`

| Boot-arg | Meaning |
|---|---|
| `virtio_sound` | QEMU virtio sound. |

### `bsd/dev/hk_wifi.c`

| Boot-arg | Meaning |
|---|---|
| `bt` | OnePlus 8 Pro Bluetooth (QCA6390 over its UART), `instantnoodle_bt.c`: 1 a look, 2 power on, 3 HCI Reset and the ROME version over the UART, 4 + the firmware download (needs the build's `instantnoodle_btfw.h`, from `op8pro-data/bt-fw`), 5 + the packet device `/dev/hk-bt`, 6 + the kernel control `com.apple.uart.bluetooth` for iOS's bluetoothd (userland `BLUETOOTH=1`). 2-4 refuse `wifi=1`; 5-6 run beside it. |
| `bt_after` | Seconds after the kernel starts before the `bt` thread begins (default 20; with `wifi=1` use about 45 so Wi-Fi is up first). |
| `bt_nvm` | 1: set byte 2 of the NVM's tag 17 to 0 (115200) as btqca.c would (experiments; default leaves the file's value). |
| `bt_skip` | Bit 0: do not download the patch; bit 1: do not download the NVM (experiments). |
| `bt_cts` | 1: the UART engine honours the controller's RTS (default on at `bt=5` and up, off below). |
| `wifi` | 1: bring up the Wi-Fi radio and station (stands for wifi_irq, wifi_ba, wifi_wide). |
| `wifi_again` | DANGEROUS: reset the phone twice out of two. |
| `wifi_ask` | Ask the service its capabilities. |
| `wifi_dirty` | Leave the DSP running at the end. |
| `wifi_file` | 0: do not join fw/network at start. |
| `wifi_fine` | Pause before every access. |
| `wifi_listen` | Seconds step 14 waits for the firmware service. |
| `wifi_loud` | Say the whole bring-up. |
| `wifi_lx` | 1/2: ath11k itself instead of the stand-in. |
| `wifi_mac` | The radio address for the driver (normally from fw/mac). |
| `wifi_new` | Unproven steps from K. |
| `wifi_pause` | ms pause. |
| `wifi_radio_read` | Step 16 reads radio registers. |
| `wifi_return` | Return to loader after steps. |
| `wifi_step` | Run Wi-Fi steps 1..N and stop. |
| `wifi_wait` | Wait. |
| `wifi_watch` | Seconds of alive lines. |

### `bsd/dev/hk_wlfw.c`

| Boot-arg | Meaning |
|---|---|
| `wifi_cal` | Step 15 cold-boot calibration. |
| `wifi_mission` | Step 15 goes on to mode mission. |
| `wifi_ready` | Seconds to wait for "firmware ready". |

### `bsd/dev/instantnoodle_battery.c`

| Boot-arg | Meaning |
|---|---|
| `battery_empty_mv` | Empty-battery threshold in mV (default 3300). |
| `battery_shutdown` | 0: only log an empty battery. |
| `charge_icl` | 0: leave the charger input limit to the firmware. |

### `bsd/dev/instantnoodle_slider.c`

| Boot-arg | Meaning |
|---|---|
| `slider` | 0: do not read the alert slider. |
| `slider_cal` | Factory calibration D,U,D,U,D,U of the alert slider. |

### `bsd/dev/instantnoodle_touch.c`

| Boot-arg | Meaning |
|---|---|
| `touch_irq` | 0: poll the OnePlus touch chip every 10 ms. |

### `bsd/hk_hfs/hk_hfs_iokit.c`

| Boot-arg | Meaning |
|---|---|
| `hfs_corruption_panics` | Panic on HFS corruption. |

### `bsd/miscfs/apfs/apfs_vfsops.c`

| Boot-arg | Meaning |
|---|---|
| `apfs_cheap` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_meta_cluster` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_par` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_root_c` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_root_v` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_round` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_ubc_read` | *diagnostic/tuning knob: see the header comment of this file* |

### `iokit/Kernel/GenericARMPE.cpp`

| Boot-arg | Meaning |
|---|---|
| `epoch` | Override the time-of-day epoch. |
| `machine_name` | Override the machine name iOS sees. |
| `sleep_test` | QEMU test: sleep S seconds repeatedly in GenericARMPE::start (tools/sleep-test). |
| `sleep_test_count` | Number of test sleeps. |
| `small_cores_after` | Seconds before the slow cores start (default 28; 0 = at once). |

### `iokit/Kernel/HKGPU.cpp`

| Boot-arg | Meaning |
|---|---|
| `gpu_boost` | ms of top GPU level after a touch (default 300; 0 off). |
| `gpu_cached` | 1: GPU buffers are ordinary cached memory. |

### `iokit/Kernel/MiniIOMobileFramebuffer.cpp`

| Boot-arg | Meaning |
|---|---|
| `gpu_stats` | 1: once a second `tl: u busy PERMILLE of 1 s in N submissions`, the 3D core's own busy time (`hk_adreno.c`). `glmirror=0` (`eagl.c`): see display.md. |
| `fb_boost` | 0 (default): an experiment that did not pay: 1 = while frames are pushed at 50 a second or more every push holds every CPU group at its top row for 250 ms (`frameBoost()`, `hk_cpufreq_frames()`); no measurable effect on late frames (display.md). `fb_kicklog=1`: log only the `tl: k` line of each push (for measuring). |
| `fb_hz` | 60: switch the OnePlus panel back from 90 Hz. **OnePlus 8 Pro (instantnoodlep): 60 or 120** (needs `fb_dcs=1` and the push path; its panel's own timing@1 commands); the profile uses **120** since 2026-10-28 (owner: clearly smoother). Give it once: with two `fb_hz=` the kernel and userland read different ones. The panel keeps its rate across RAM runs; ABL's reset brings it back to 60. |
| `fb_probe=2` | OnePlus 8/8 Pro: besides `fb_probe=1`'s dump, a read-only raw dump of DSI0's controller (0xae94000), its D-PHY and PLL (+0x400, +0x900), both DSC encoders, INTF1's timing registers and the display clock controller, to the loader log (`display: DSI0+0x..:` lines). Owner-run 2026-10-27, no crash. |
| `fb_dsc=N` | OnePlus 8 Pro: derives the DSC encoder set and the PPS of the FHD+ (checked against both encoders: all 148 words equal) and the two QHD+ modes from the device tree's `qcom,mdss-dsc-*` numbers, logged as `display: dsc ...`. `fb_dsc=2` also rewrites the derived FHD+ values into both encoders (only if every word matched) and reads them back. Owner-run 2026-10-27: 0 of 148 differ, 0 differ on read-back, screen unaffected. |
| `fb_qhd=1`, `fb_qhd_secs=N` | **Experimental, left the panel black (2026-10-27): do not use without a force-reboot at hand.** OnePlus 8 Pro, needs `fb_dcs=1`: a throwaway attempt at the panel's 60 Hz QHD+ mode before iOS has the screen (panel switch + DSC packet, pipeline resized, test pattern for N s (20), then FHD+ put back). See docs/TODO.md T38. |
| `modem` | OnePlus 8 Pro only (`bsd/dev/instantnoodle_modem.c`, `hk_mhi.c`). The X55 modem on PCIe 2, in steps, on a thread beside the system's start: `1` reads the control lines and PCIe 2's clocks; `2` PCIe 2 up and the modem's MHI registers read, then down; `3` as 2 plus the SMMU stream and `sbl1.mbn` through BHI; `4` MHI through the boot ROM and SBL and Sahara served from the phone's modem partition (all 15 images); `5` as 4, then the mission's channels started and QRTR listened to. Research use: changes hardware state; RAM runs only so far. | off |
| `modem_keep` | `1`: PCIe 2 left up when the steps end. | 0 |
| `modem_fw` | `a`: the firmware from `modem_a` instead of `modem_b` (LUN 4, FAT16). | b |
| `modem_ids` | Sahara image id overrides/additions, `ID=FILE,ID=FILE` (files of `image/sdx55m/`). | built-in table |
| `modem_secs`, `modem_watch`, `modem_qrtr` | seconds: Sahara's limit (60), watching the modem after the images (15), listening to QRTR in the mission (20). | |
| `modem_quiet`, `modem_cycle`, `modem_lanes` | experiments on the mission: `modem_quiet=1` start the channels and say nothing (2: answer hellos only); `modem_cycle=1` reset the link with a full controller power cycle instead of PERST alone; `modem_lanes=2` tell the controller it has two lanes. | 0 |
| `modem_chans` | bit mask of the MHI channel pairs started (LOOPBACK, DIAG, QDSS, EFS, QMI0, QMI1, IP_CTRL, IPCR, TF, DCI, DUN, IP_HW0: bits 0..11); the working set is `139` (LOOPBACK, DIAG, EFS, IPCR) with `modem_quiet=3` (plain first-link mission). | 0x881 |
| `modem_bringup`, `modem_tftp`, `modem_efs`, `modem_sms` | `modem_bringup=1`: the QMI bring-up (DMS online, UIM PIN1 0000, NAS registration); `modem_tftp=1`: serve the modem's TFTP (QRTR service 4096); `modem_efs=1`: Sahara ids 16/17/20 from the phone's `mdm1m9kefs1/2/3` (LUN 5, read only) instead of the firmware's empty stubs (without it the modem spends minutes loading every carrier config); `modem_sms=1`: one test SMS to the owner's number once registered. | 0 |
| `modem_service` | 1: after the registration the modem thread stays: it keeps `kern.hackios.cellular_*` / `modem_*` true (bars, state, operator), sends SMS written to `modem_sms_send`, decodes SMS received, and boots the modem again when its link goes away (a system sleep). Needs `modem_qrtr=` large (seconds the thread runs). | 0 |
| `modem_data` | OnePlus 8 Pro (`instantnoodle_modem.c`): **mobile data**. 1: after the registration the WDA data format (raw IP in QMAP, MHI endpoint type 3 interface 4), WDS bind mux 1, IP family 4, start network interface (default profile; retried while the packet domain attaches) and get current settings; then `en1` (`hk_net` unit 1) is made, the kernel answers its ARP and DHCP with the session's address and `net_test cell` (job com.hackios.wifi) has configd's IPConfiguration run DHCP on it. 2: also a DNS query for example.com sent through the session as a self-test (logged `DNS ANSWER`). Needs `modem_chans` with bit 11 (IP_HW0, channels 100/101): `modem_chans=2187` (LOOPBACK, DIAG, EFS, IPCR, IP_HW0). Verified on the phone 2026-10-29 (DNS, TCP, HTTPS through en1). | 0 |
| `modem_data6` | 2 (default): the IPv6 call first, no IPv4 call when it is up (iOS makes IPv4 by CLAT46); 1: IPv6 as a second call after the IPv4 one (fails on the 'orange' profile); 0: no IPv6. | 2 |
| `kern.hackios.voice_speaker` (sysctl) | a call on the speakers (1) or at the earpiece (0, where every call starts); `libhackios-call` writes it from iOS's route picker |
| `kern.hackios.voice_mute` (sysctl) | this call's microphone muted; `hk_commcenter` writes it from CallKit's mute action |
| `kern.hackios.voice_earpiece_gain` (sysctl) | 0..15, TDMSPKG of the earpiece amplifier during a call, a test for judging the level (the container's value is 13) |
| `im_probe`, `im_sms` | `libhackios-im` inside imagent (`/etc/inject.conf`): `im_probe=1` logs every call of the telephony and SMS classes to the console (debug); `im_sms=0` turns the native SMS send off (Messages' sends then fail as before and `hk_smsd` resends failed rows). | 0, on |
| `modem_data_ep`, `modem_data_qmap`, `modem_apn`, `modem_en1` | `modem_data_ep=HEX`: endpoint type (high half) and interface id (low half) of the MHI data port, default `0x30004` (the modem accepted it first try; other ids are tried if it refuses); `modem_data_qmap=0`: raw IP with no QMAP header; `modem_apn=NAME`: the access point (else the SIM's default profile); `modem_en1=0`: no interface for iOS (session and DNS test only). | 0x30004, 1, profile, 1 |
| `modem_sms_self`, `modem_sms_num` | `modem_sms_self=1`: after the registration a test SMS to the SIM's own number (DMS Get MSISDN, then EF MSISDN; this SIM has none), or to `modem_sms_num=DIGITS` (international, no plus; given by the owner for the test: keep it out of committed files). The received messages are the sysctl `kern.hackios.modem_sms_ring` (the last eight, `N<TAB>NUMBER: TEXT`), which `hk_smsd` puts into Messages' database. | 0 |
| `sbsigmode` | SpringBoard hook (userland): the signal mode forced on the cellular icon while registered (1 is "active"; 0 shows dots, 2 dim). | 1 |
| `modem_blk`, `modem_resume` | largest TFTP block offered (1024); `modem_resume=1` RESUME_TX after every datagram (experiment). | |
| `modem_probe`, `modem_nc` | `modem_probe=1`: step 3 first hands the boot ROM an unmapped address (proof of the SMMU path); `modem_nc=1`: buffers mapped non-cacheable (as the failed first tries). | 0 |
| `fbwidth`, `fbheight` | The size iOS renders at (default 1125 x 2436, the iPhone X), GPU-scaled to the panel with `fb_push=2`. OnePlus 8 Pro: `fbwidth=1290 fbheight=2796` (iPhone 14/15 Pro Max size, 3x, since 2026-10-28: at the 1320 x 2868 used before, iOS 16.5's keyboard has no digits plane, K43; about 1.25% stretch to QHD+ 1440 x 3168, 2% to the FHD+ panel mode now driven; status bar needs `libhackios-ui`). |
| `fb_subtype=N` | Overrides the DT `/product artwork-device-subtype` (iPhone X: 2436). It changes UIKit's whole layout class; not needed with `libhackios-ui`. |
| `fb_probe` | 1: dump the OnePlus 8 display pipeline; 2: the mode registers of a QHD+ switch too; 3: also the VIG pipes' scalers (SSPP + 0xa00: version, state, coefficient tables), read only. |
| `metal` | 1 Metal driver for test programs; 2 whole system (with metal_profile=1). **Default `metal=2 metal_profile=1` since 2026-11-07** (loader defaults); `metal=0` opts out. |

### `iokit/Kernel/MiniIOSurface.cpp`

| Boot-arg | Meaning |
|---|---|
| `surftrace_of` | Trace a surface. |

### `osfmk/arm64/loader_return.c`

| Boot-arg | Meaning |
|---|---|
| `bootcpu` | Run the kernel on this core (profiles pass 7) instead of the firmware default; the loader watches from the first core. |
| `hv_wdt_pet` | Pet the Qualcomm hypervisor watchdog from the return channel. |
| `loader_return_after` | Return to the loader after S seconds of wall time. |
| `return_cpu` | Core the return runs on. |
| `return_panic` | Return to the loader on panic (see loader_return.c). |

### `osfmk/arm64/user_exception_trace.c`

| Boot-arg | Meaning |
|---|---|
| `ustacks` | Dump user thread stacks (qemu/touch.py stacks NAME). |
| `ustacks_at` | *diagnostic/tuning knob: see the header comment of this file* |
| `ustacks_count` | *diagnostic/tuning knob: see the header comment of this file* |
| `ustacks_ms` | *diagnostic/tuning knob: see the header comment of this file* |
| `ustacks_threads` | *diagnostic/tuning knob: see the header comment of this file* |
| `ustacks_tname` | *diagnostic/tuning knob: see the header comment of this file* |

## Userland (read through `sysctl kern.bootargs`, userland repo)

| Boot-arg | File | Meaning |
|---|---|---|
| `gl=0/1` | `inject.c`, `inject_sb.c` | 0: never load the OpenGL ES library; 1: also without a GPU (Mesa software) |
| `glyes=N,..`, `glno=N,..` | `inject.c` | tell CoreAnimation's renderer it has/has not those shader functions. **`glyes=40` crashes the window server** (see research/ios-userland.md) |
| `ca_fps=N`, `ca_vbl=1`, `fb_hz` | `inject.c`, `inject_sb.c` | frame-rate range / VSync-driven frame clock (automatic where the panel is faster than 60) |
| `ca_ambient=N` | `inject_sb.c` | frame rate of slow ambient display links (default 30; 0 off) |
| `ca_rates=1`, `ca_links=1` | `inject.c`, `inject_sb.c` | diagnostics: list rate methods / display links |
| `cc_fade=MS` | `inject_sb.c` | check-only workaround: eased Control Center fade |
| `sb_clock=N` | `inject_sb.c` | status-bar clock centred N points further right (OnePlus camera hole) |
| `sb_hud_dy=N` | `inject_sb.c` | volume HUD shifted N points down (default 160.4: level with the OnePlus 8 Pro's volume rocker; 0 = iOS's place). Placement unverified; the HUD's views are logged as `volume HUD:` |
| `sb_delay=S` | `inject_sb.c` | SpringBoard start delay so apps register in a stable order (default 8 under `xl run`) |
| `autounlock=0` | `inject_sb.c` | keep the lock screen (default: dismiss after 10 s) |
| `siri_orb=0` | `inject_sb.c` | skip Siri orb shader prewarm (diagnostic) |
| `touch_lat=1` | `inject.c`, `inject_sb.c` | latency lines |
| `hkprof=1/2` | `hkprof.h` | userland sampling profiler (2: from process start) |
| `gltrace*`, `glcensus`, `glshaders`, `glshow*`, `glskip`, `glplain`, `glwait`, `glcopy`, `glfetch`, `glgroup`, `vblur`, `glenv`, `glslow` | `eagl.c` | OpenGL ES debugging; see the header of `eagl.c` |
| `metal_profile=1` | `hkmetal.m` | with `metal=2`: whole system on Metal; `metal_sync=1` fence per draw |
| `metal_dump=NAME`, `metal_skip=NAME`, `metal_texlog=1` | `hkmetal.m` | the AIR of every pipeline function whose name has NAME, into `/private/var/tmp/hk_air/` (for `build/mesa-host/.../air2nir`); leave out the draws of pipelines whose "vertex / fragment" names contain NAME; each different fragment texture a draw samples, once, with texels of its first and last level (with `applog=1`) |
| `ca_detach=1` | `inject.c` | lets CoreAnimation detach a layer to a display plane again (default: `CA_DISABLE_DETACHED=1` in backboardd: the framebuffer shows one layer only) |
| `metal_probe=1` | `hkmetal.m` | every 4th drawable a process draws: how much of it is black, in nine columns (needs `applog=1` to be seen); every 30th command buffer: its passes and the last target's middle column |
| `softpipe`, `llvmpipe` | `gl_osmesa.c` | Mesa software driver choice |
| `root_apfs`, `var_store`, `var_data`, `var_data_format`, `var_save_every` | `init.c`, `var_store.c` | root and persistence (see storage.md) |
| `exctrace=1 applog=1 applog_name=APP`, `systrace=1/2 systrace_name=NAME` | kernel `user_exception_trace.c` (`applog` is only mentioned in its comment) | trace failing syscalls / crashes (`usertrap:`, `userabort:` in the log) |
| `log_launchd=0` | used by `tools/t`, README | keeps launchd's lines out of the log that comes back (the parser was not found by grep in this checkout: unverified where it is read) |

## Kernel sysctls (`kern.hackios.*`)

56 exist in the source (`bsd/dev/*.c`, `iokit/Kernel/*`); most are probes. Used by tests/docs: `thermal`, `thermal_pressure`,
`thermal_hottest`, `thermal_throttles`, `sleeps`, `sleep_seconds`, `sleep_last`, `sleep_timer`, `sleep_timer_key`, `display_on`,
`backlight`, `backlight_sent`, `top`, `cpuspeed`, `memlat`, `membench`, `frames`, `kprof_ctl`, `kprof`, `slider`, `charger`,
`battery_soc`, `battery_charging`, `var_save`, `var_busy`, `adsp_services`. List them on a running system with `sysctl kern.hackios`.
The others are not yet documented individually (TODO). Scheduler: `amp` (text: clusters, graph, Edge's and the controller's counts,
latency histograms, energy model, busiest groups; `sched_test amp LABEL`), `sched_check` (write 1: run the kernel's scheduler checks now;
read: passed << 16 | failed) — [subsystems/scheduler.md](subsystems/scheduler.md).

## iphont development agent (built; physical transport validation pending)

`iphont=1` enables kernel diagnostics and creates root-only `/dev/iphont`; default is 0. `iphont_control=1` additionally authorizes input and userspace requests for that development boot; default is 0. `iphont_selftest=1` runs native read-only kernel probes and an authorization-denial check at startup; it still needs `iphont=1`. Runtime `iphont trace LEVEL` selects event severity 0..4. See [TOOLKIT.md](TOOLKIT.md) for protocol, validation evidence, logging overhead, and the owner's deferred RAM reload. No flag is added to the normal installed profile.

Sysctls added 2026-10-29: `kern.hackios.cellular_data` (bit 0 the modem's IPv4 session, bit 1 the IPv6 one), `kern.hackios.modem_qmi` (write `SERVICE MSGID PORT HEX...`, read the answer; `hk_cell qmi`), `kern.hackios.modem_sms_send` (any user may write `NUMBER TEXT`: GSM 7-bit or UCS-2, concatenated parts).

Added 2026-11-01/02 (OnePlus 8 Pro cellular): `modem_diag=0` (the modem's DIAG channel is opened but the modem is not asked for its debug messages; default 1 asks for all of them, a stream that never stops: use 0 on a daily image); `im_sms_in=1` (libhackios-im in imagent tries to hand received SMS to imagent's own session, an experiment that does not work yet; default off). The daily images no longer pass `applog=1` (every program's standard output and error on the console). hk_commcenter's answers file (`/hackios/etc/hk_commcenter.json`, or a newer `hk_cc*.json` put on the phone with `iphont put` during this boot): `verbose`, `capture_plain`, `snapshot`, `dump`, `dump_strings`, `describe`, `call`, `mg`, `unlink`, `set_settings`, `setters`, `legacy`, `legacy_extra`, `notify_state`, `serve_commcenter_xpc` (research/modem-sdx55.md).

Added 2026-11-02: `surf_wrap=1` (surfaces around a client's pictures, upstream hakios 759b620; default 0: no difference measured on the 8 Pro), `fb_pri=N` (priority of the panel's threads, upstream cd8fdbb; default 0, the same), `glstill=1` (still surfaces looked at every 8th frame, 759b620; default off, the same), `glstats=1` (the OpenGL statistics every 20 and 200 frames; off by default: they were 5000 of 12000 log lines in 9 minutes), `sbsigtry=1` (the cellular views given modes 0, 2 and 1 in turn, for measuring). `mic=4` is now in the daily set. Sysctl `kern.hackios.fb_frames`: frames put on the panel since boot. Sysctl `kern.hackios.fb_layers` (2026-11-06): seven words, the last swap's layer mask and its four surfaces, how many swaps had more than one layer, and the last such mask. `/private/var/tmp/hk_sb_cell.log`: what the status bar was told, as it changes (`inject_sb.c`).

Also 2026-11-02, a call's sound (none yet, research/modem-sdx55.md): `voice=N` (`hk_mic.c`: 1 the DSP's voice version, 2 and the mailbox, 3 and the sessions while `kern.hackios.voice_call` is set), `voice_bridge=1` (QRTR forwarded between the modem and the DSP; a call then drops after 5 s). The answers file's `"voice": 0`, `"voice_dry_run": 1`, `"voice_session": N`, `"voice_fake_incoming": "NUMBER"` and `"rat"` are read by `hk_commcenter`.

`voice_sat=1` (2026-11-02): the DSP's own MHI channels to the modem (`mhi_sat`); with `voice=3`, the sound of a call. In the daily set of `out/tree/flash.sh`. `voice_bridge=1` is a dead end: it crashes the modem on a call.

Added 2026-11-07 (Metal and display performance, [subsystems/gpu.md](subsystems/gpu.md) "Performance: Maps"): `metal_stats=1` (the Metal driver's five-second line per process to the console, the library's per-draw stages, backboardd's "pace" line), `metal_cost=1` (with `metal_stats=1` and `metal_sync_in=`: GPU time by pipeline), `metal_sync_in=NAME` (wait for every draw in that process only; `metal_sync=1` with Metal everywhere hung the phone once), `metal_fbkeep=0` (surfaces made per draw and the framebuffer unbound after it, as before), `metal_tiler=0` (no discards for DontCare actions, resolve as a blit after the pass), `metal_msaa=N` (most samples a render target or pipeline gets, 1/2/4, default 2; 1 draws Maps black), `metal_strict=1` (indices read to bound vertex fetches), `fb_same=1` (a swap of the frame already on the panel is copied and pushed again). `gpu_log=1` lines now also say each submission's process (`by NAME`) and each fence wait (`tl: w`); `gpu_stats=1` adds the clock. Sysctl `kern.hackios.fb_same`: swaps not pushed because nothing changed. `metal_native=0` (2026-11-07): applications' Metal layers keep contentsScale 3 instead of the panel's density (2.512 on the OnePlus 8 Pro). Sysctl `kern.hackios.fb_panel`: four words, iOS's display size and the panel's, in pixels.
