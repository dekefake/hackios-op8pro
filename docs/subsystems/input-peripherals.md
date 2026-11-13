# Input and peripherals: touch, keys, slider, haptics, flashlight, battery, camera

Everything that is a small device on the phone. None of Apple's drivers exist here, so each is a kernel driver that
feeds a character device (or an IOKit stand-in) and a library of ours inside a stock iOS daemon that turns it into
what iOS expects.

## Touch and buttons: `/dev/hk-touch`

iOS expects touches from Apple's multitouch driver through IOHIDFamily; neither exists. A producer (a kernel driver,
`virtio-input` under QEMU, or a script via `rwtouch`) posts `struct hk_touch` records (`xnu/files/bsd/sys/hk_input.h`:
`HK_TOUCH_DOWN/MOVE/UP`, `HK_BUTTON_DOWN/UP`, `HK_SLIDER`; coordinates 0..65535) to `/dev/hk-touch`
(`hk_input.c`, `hk_touch_post()`). `userland/src/inject.c` inside **backboardd** reads them and synthesises the
events: a "virtual" HID service (built in, one usage) turns samples into the events Apple's driver would send.
Findings behind it:

- backboardd takes for its touch screen a service that exists when its touch processing starts; one that turns up
  later is not noticed. CoreBrightness's five-second wait for an ambient light sensor
  (`-[BLControl waitForALSArrival]`) used to hide this race by accident; the wait is kept (a "brightness_fixes()"
  attempt to remove it made backboardd stop adopting the service: "still waiting for a main screen digitizer"). The
  service is therefore created repeatedly until the server has opened it (`make_service()`).
- UIKit's screen-edge recognizers (swipe up to leave an app, swipe back, pull down) accept only touches that Apple's
  controller marked as coming in over an edge, from the shape of the contact. All we have is where the finger landed,
  so a finger landing within 16/375 of the width or 16/812 of the height of an edge is marked as coming in over it.
- `TouchDetectionMode` from the window server (low byte): 1 report touches, `0xfe` wake mode (display off, a tap is to
  wake it), `0xff` off.
- Scripted touches (`rwtouch`) do not reach iOS's *system* gestures (swipe up for the app switcher, going home).
  Use `rwtouch swipe` from the bottom edge for the real gesture path and `rwtouch notify com.hackios.switcher` for
  the switcher. As of the last notes the switcher notification returns but does not change the screen when three apps run.
- Latency: `touch_lat=1` prints a line per sample (`tl: i TYPE posted US read US sent US`) and per SpringBoard
  run-loop turn; analyse with `tools/latency.py`, `tools/frames.py`, `tools/pipeline-latency.py`.

### Drivers

| Device | Chip | File | Notes |
|---|---|---|---|
| redwood | Goodix GT9916 ("Berlin-D") or FocalTech FT3680, SPI of serial engine 13 | `bsd/dev/hk_goodix.c` | `touchscreen=1`. GPIO96=1 and GPIO68=0 identify the FocalTech variant. No SPI framework: the driver does power (PM8350C LDO2 through RPMh), clocks, pins and the GENI engine in FIFO mode itself, or bit-bangs the four pins. **No interrupt**: a thread looks at the line 125 times a second (`touch_poll_us`). FocalTech firmware upload is `hk_focaltech_fw.h` (generated from `fw-touch/focaltech_ts_fw_m20.bin`, ~114 KB over SPI). |
| instantnoodle | Samsung S6SY761 ("SY771"), I2C `0x48` | `bsd/dev/instantnoodle_touch.c` | `board=instantnoodle`, `touchscreen=0` off. Pins gpio36/37 driven by hand in `instantnoodle_i2c.c` (open drain: low = output 0, high = input), because the engine's FIFO path is disabled on these phones and would need GPI DMA. Reset gpio38, report line gpio39 (active low, TLMM summary interrupt 208+32). Supplies PM8150A LDO1 (1.8 V) and PM8150 LDO13 (3 V) through RPMh. Panel coordinates 1080 x 2400 scaled to 0..65535. Configuration commands are not acknowledged on this phone yet the chip reports touches after its boot-complete event. Interrupt-driven (`touch_irq=0`: 10 ms polling); worker at priority 92. Held in reset with the screen off. |
| QEMU | virtio-input | `hk_virtio_input.c` | `virtio_input=1`; mouse with the left button is a finger |

**Never pass `touchscreen=1` on the OnePlus 8.** That argument selects redwood's Goodix driver; it touched GPIO68 and sent
the OnePlus to a NoC CrashDump. The OnePlus path is guarded now, but the profile does not pass it.

### OnePlus 8 Pro touch: rate and game mode (2026-11-07, RAM image 260, the owner's fingers)

- `instantnoodle_touch.c` sends the chip OnePlus's game mode after its start and every screen wake: `0x3F 00 00 00 12`
  (`SEC_CMD_SENSETIVE_CTRL`; 0x18 is the default; from OnePlus's own `sec_drivers_s6sy761.c`, which sets it only when a game
  asks). The chip takes it (`tl: g ... set`). `touch_game=0` leaves the default. The owner: "a bit faster, hard to tell".
- Measured with real fingers (`tl: g` every 8 strokes, ported from upstream hakios a3975b6): samples 8.25-8.34 ms apart
  (121 a second; the longest gap 10-15 ms), the first move 6-8 ms after the press, the lift 8 ms after the last move --
  one report period each, no missed reports. The reports follow the panel's 120 Hz; OnePlus's S6SY761 driver has no
  report-rate switch (no `tp_refresh_switch`/`set_high_frame_rate`), so its "240 Hz" is not a rate Android sets either.
- Upstream's touch changes of 3-5 November (4 ms polling, reading while a finger is down) fix a FocalTech that pulses its
  line; this chip's line is level-triggered and the driver already reads while it is low, and backboardd's bridge reads the
  device blocking, so neither applies here. The press line on the console is kept to the first eight presses (a console
  write from the touch thread holds the next sample up).

### Buttons (`hk_buttons.c`)

None of the three keys is a SoC pin: they are inputs of the PMICs on the SPMI bus, reached through the SoC's **PMIC
arbiter** (version 5), which gives each system on the chip a set of registers per PMIC peripheral ("channel").
Redwood: power = KPDPWR_N and volume down = RESIN_N of the PMK8350's PON block, volume up = GPIO6 of the PM7325
(pulled up). OnePlus 8: PM8150 PON and GPIO6/7. The driver **reads only** (the one thing written is the arbiter's
command word); the firmware reads the same keys itself (fastboot/recovery choice) and has left them configured; the
driver verifies that before it believes them and a key that reads as held before it was ever seen released is not
believed. On the OnePlus 8 the power key and volume keys have interrupts (arbiter, PDC, SPI 481; see
[power.md](power.md)). Bring-up steps `buttons_steps=N`. The phone has what the iPhone X (whose userland runs here)
has: side button (`HK_BUTTON_LOCK`) and two volume keys, no home button. `hk_spmi_lock()` serialises all PMIC
transfers (two at once and the arbiter drops one).

### Alert slider (OnePlus 8, `instantnoodle_slider.c`)

A magnet in the slider and two iSentek IST8801 hall sensors on serial engine 5 (GPIO 4/5), "up" `0x19` and "down"
`0x18`, powered by PM8150A LDO10 (3.0 V). The factory calibration (`/mnt/vendor/persist/engineermode/tri_state_hall_data`,
three pairs `down,up`; boot-arg `slider_cal=D,U,D,U,D,U`, in the profile) decides the position (nearest of three within
22); without it OxygenOS's rule (above 30 up, below -30 down). A position counts after two equal reads. Interrupts
not used: both sensors are read every 100 ms (300 ms while dark). Reported as `HK_SLIDER` records and
`kern.hackios.slider`. iOS side: backboardd keeps ring/silent (middle = silent, down and up = ring) and SpringBoard shows
its ringer HUD; "up" drives **Do Not Disturb** through an assertion of SpringBoard's HID client
(`com.apple.springboard.donotdisturb.hid`, named in its entitlements), dressing the ringer pill (`SBRingerPillView`)
with a moon and "Do Not Disturb". The first DND call is deferred 20 s or SpringBoard SIGSEGVs. Verified on the phone by
the owner.

### Haptics (`hk_haptic.c`, `haptic=N`)

Redwood: Awinic AW86927 (ID 0x9270, Xiaomi's `aw8692x` 1.7.0), I2C of serial engine 9 (GPIO 36/37), address `0x5a`,
reset GPIO 35; the motor's f0 is 175.5 Hz. OnePlus 8: Awinic AW8697 with the waveforms of the phone's
`/vendor/firmware/aw8697_haptic.bin` (`aw8697_haptic_170.bin` is used; 11 waveforms: 1..10 short, 11 looped). `haptic=1..4`
progress from reading to playing an effect; **`haptic=5` is the mode for iOS**: what iOS plays on its "Actuator" device
(`audio_plugin.c`) is written to `/dev/hk-haptic` as one-channel samples and each waveform becomes a tap (waveform 1) or,
if longer than a tap, the looped waveform until it ends (at most `HAPTIC_MAX_MS`, then rest for as long). Gain follows the
loudness (Android's own 0x80 from a third of full scale; five eighths for the softest; never above). Limits: Android's
own values only; any fault flag, failed read-back or unacknowledged byte puts the chip in reset for the run. Verified
on the OnePlus 8 (system feedback, CoreHaptics).

### Flashlight (`hk_torch.c`, `torch=N`)

Redwood: the PM8350C flash LED module (SPMI slave 2, `0xee00`), channel 0 at Android's torch current 125 mA
(12.5 mA resolution). OnePlus 8: PM8150L, both LEDs. `torch=1` looks, `2` lights for `torch_secs`, **`3` is the mode for
iOS**: `/dev/hk-torch` takes a level 0..1000 (thousandths of Android's current, never above it). Limits: the light on at
most `torch_max_s` (default 30, max 120) at a time; the module's fault and over-temperature flags, the PMIC's
temperature alarm and the battery's temperature (45 C) are checked ten times a second and any trouble switches it off for
the run. Every write goes through one function that takes only this module's registers, with read-back. iOS side:
the loader tells MobileGestalt the camera has a flash (`/product/camera "flash"`) and `libhackios-media.dylib` in
mediaserverd gives `FigFlashlight` (which finds no camera) a device of ours whose torch level is written to
`/dev/hk-torch` (Control Center's button and AVFlashlight). Verified on both.

### Battery and charger (`hk_battery.c`, `instantnoodle_battery.c`, `HKBattery.cpp`)

iOS finds every `IOPMPowerSource` (the first is "InternalBattery-0"). `HKBattery.cpp` is that service; values reach it
through `hk_battery_report()` with a `struct hk_battery_state` (percent in hundredths, voltage, current, temperature,
present, external, status, charges in uAh, cycles, USB type). `CurrentCapacity` is the percentage and `MaxCapacity`
100 (newer-battery convention); mAh values are `AppleRawCurrentCapacity` etc. Everything else (status bar, "Charging"
on the lock screen, Batteries widget, Settings > Battery) follows from powerd. The status-bar percentage switch is
turned on by the image (`SBShowBatteryPercentage`).

- **Redwood:** values from the audio DSP's battery manager (`hk_adsp.c`, `adsp=1`, `battmgr=0` off). Charger limits and
  JEITA are firmware-owned. Without `adsp=1` the loader owns the firmware charger context on core 0 (`battery=1`).
  Verified charging with `adsp=1` (+0.4 A from a computer's port); `sysctl kern.hackios.charger` has health and type.
- **OnePlus 8:** a TI **bq27541** fuel gauge (bq27411 variant if the type register says so) at I2C `0x55` on serial
  engine 16 (gpio48/49, pull-up and clock stretching needed), OnePlus's driver `bq27541_fuelgauger.c` as the register map.
  Charger presence from the PM8150B (SPMI slave 2, USBIN `0x1310` bit 4, state `0x1006` [2:0]). Reads every 5 s. The Warp
  fast-charge MCU (`0x26`) is left alone. Details of charge limits, off-mode charging and the empty-battery shutdown:
  [power.md](power.md).
- A UIDevice fallback in SpringBoard (`inject_sb.c`) still reads `kern.hackios.battery_soc/charging` when no power source
  exists; once `HKBattery` is there the hooks hand everything to iOS's own code.

### Cameras (redwood only: `hk_camera.c`, `cam=N`)

Redwood has four sensors: macro OmniVision OV02B10 (CCI 1 master 0, `0x3c`), wide Samsung S5KHM2SD 108 MP (CCI 0
master 1, `0x20`, chip ID `0xfad2`), front OmniVision OV16A1Q 16 MP (CCI 0 master 0, `0x10`, ID `0x1641`), ultra-wide
Samsung S5K4H7 8 MP (CCI 1 master 1, `0x2d`, ID `0x487b`), with the PM8008i supplying the LDOs. The driver was brought
up step by step: `cam=1` look (reads only), `2` power the sensor as Android does and read its ID, `3` the CSID test
pattern through RDI 0 / IFE-lite 0 into memory (`xl camera` fetches it), `4` the sensor's own frames through its CSIPHY,
**`5` the wide camera for iOS** (`/dev/hk-camera`; `cam_sensor=wide` selects it in the early modes). Limits: only Android's
voltages; every write read back; anything wrong switches all off.

iOS side (userland repo): `inject_media.c` gives mediaserverd's capture-device vendor the create function of our
capture device (`camera_device.c`), a `FigCaptureDevice` with two `FigCaptureStream`s (wide as the back camera, front
as the front), laid out like H10ISP's in iOS 16.5, formats named as the iPhone X's in CMCapture's
`D22/AVCaptureSession.plist`. Frames: RAW10 read from `/dev/hk-camera`, made into pictures by the CPU
(`camera_isp.c`: black level, lens shading, grey-world white balance and exposure gains, the camera's own colour matrix,
sRGB gamma, zoom; no noise reduction). Camera.app shows preview, focus (by contrast, continuous, and on a tap), and
takes HEIC photos (README claim). The portrait-mode Core Image prewarm that crashed mediaserverd is skipped.
**OnePlus 8 Pro lens motor (autofocus), 2026-10-27, `bsd/dev/instantnoodle_lens.c`.** The main module's actuator is an AKM AK7377 at I2C 0x0c on CCI master 0
[source: the module file's actuatorDriver "imx689_ak7377"; Linux ak7375 driver, same family]: reg 0x02 mode (0x20 sleep after power-up, 0x40 standby, 0x00 active; Android's init is
0x40 then 0x00), position = one 16-bit word at reg 0x00, the 10-bit code << 6 (module file: 2 data bytes, register 0, shift 6; region 0..1014). The OIS (LC898128 at 0x24) is not touched:
Android's driver reprograms its flash. The driver reads before writing, reads back the mode steps and the first position, ramps 24 codes / 2 ms, and switches itself off on any surprise.
**Observed (lens1/lens2 on the phone):** the chip answers, comes up active, the first write reads back; the userland's contrast autofocus then finds sharp places (sharpness 25 at
iOS position 268 for a close scene, 22 at the far end in run 1); the owner: autofocus works but is slow, tap-to-focus does not. Direction: code 0 (no current, rest) is the CLOSEST
focus, a higher code is further, so iOS position = 1000 - code. Not measured: where infinity lies, the userland's AF range (AF_NEAR 16..AF_FAR 768, redwood's) against the code range
0..1000, why the climb is slow (a step of 48 and frames of waiting each; the driver's own ramp is under 5 ms a step), why tap-to-focus does not converge. OIS: not done.

**OnePlus 8 Pro, 2026-11-07 (verified on the phone, RAM images 181-184, unless marked):**
- *mediaserverd died whenever Camera.app opened* (CMCapture `-[BWMultiFilterThumbnailNode prepareForCurrentConfigurationToBecomeLive]`, a nil
  filter descriptor: the filters' lookup tables want the Metal device mediaserverd is kept from): `inject_media.c` leaves that node unprepared.
  The filter strip has no thumbnails; the camera opens, switches and closes.
- *The kernel panicked after a few camera starts with slpi=1*: every start mapped its SMMU context page with `ml_io_map()` (permanent) and
  leaked its page tables until io_map's range ran out ("kmem_alloc ... failed with 3" under fp_smmu_up()); now mapped once, tables freed.
- *Camera.app ended the kernel's run* (RAM image 174, `slpi=1`): the IFE's eight streams take match registers 9.., of which the hypervisor refuses four
  (their SMRs read 0 after the write); a refused one still reads free, so the sensor DSP's FastRPC (`hk_fastrpc.c`, started after the camera's first
  stream) took register 10, and the camera's stop then "put back as found" all eight, writing 0 over it. `instantnoodle_camera.c` now gives a refused
  register back at once and puts back only registers that still hold what it wrote (`fp_put_back_matches()`); `hk_fastrpc.c` checks its own before
  each call and rewrites it if lost (`mmu_keep()`).
- *Focus by the frame's own lens position (built; not yet seen: the back camera lay against the table):* the kernel stamps each frame with where the lens was while it was exposed (`HK_CAMERA_FOCUS_STAMPED`,
  `HK_CAMERA_FOCUS_MOVED` in `hk_camera.h`, both copies); the userland measures sharpness only on frames exposed where it sent the lens. Before, the
  frames still in the ring from before a move were measured as the new place's, and a tap's sweep settled where the picture was blurred
  (logs/lens/scan1.live: best 1.52 at 48, settled at 36 with 0.026). Redwood's frames have no STAMPED bit and keep the old frame counting.
- *Colour tuning (front camera verified: a white ceiling is neutral grey, evenly lit):* the image had no `.isp` files for the 8 Pro (identity colour matrix, no lens shading). `tools/camtuning.py` now reads Qualcomm's
  V2.0.0 tuning files: `camera-imx689.isp` and `camera-imx471.isp` (from the phone's `com.qti.tuned.semco_imx689.bin` / `truly_imx471.bin`,
  vendor data, not in git) in `out/tree/r42/fw/`; `camera_device.c` picks them by sensor on `board=instantnoodle`. The front camera's own file has
  an identity colour matrix (its tuning does colour elsewhere), so only its lens shading and white balance lights change.

**Video recording, OnePlus 8 Pro, 2026-11-12 (RAM image 319, verified on the phone):** VideoToolbox here has no H.264 encoder (the
iPhone's are its AVE hardware) and only Apple's software HEVC, at 1.4-2.1 frames a second. `userland/src/h264enc.c` is an H.264 encoder of
ours (Constrained Baseline, CAVLC; I16x16 and P16x16 with integer-pel motion search and P_Skip; one slice per band of macroblock rows, the
bands on threads; one QP a frame from a bit budget; no deblocking), and `userland/src/h264_vt.c` puts it behind VideoToolbox as an encoder
for `'avc1'` (`VTRegisterVideoEncoderWithInfo`, a CoreMedia derived object of `VTVideoEncoderGetClassID()`; the encoder class's layout is in
the file's header), registered in mediaserverd by `inject_media.c` (`h264enc=0`: not registered). On the Mac it decodes without an error in
ffmpeg and is bit-identical to its own reconstruction (`userland/src/h264enc_test.c`; 1080p at 16 Mbit/s: 37.8 dB, 16.8 ms a frame on 8
threads). On the phone the capture graph opens it for a 1920x1080 30 fps movie at 15.3 Mbit/s (the bitrate the graph asks for), and Camera
writes a finished movie (the mdat's size filled in, a moov after it).
- Camera must ask for H.264: *Settings > Camera > Formats > Most Compatible*, i.e. `CAMUserPreferenceCaptureEncodingBehavior = 1` in
  `com.apple.camera` (mobile). A new data volume gets it from `userland/var/mobile/Library/Preferences/com.apple.camera.plist`; on a phone
  set up before: `hk_prefs set com.apple.camera mobile CAMUserPreferenceCaptureEncodingBehavior int 1`. With *High Efficiency* (iOS's default) a
  movie goes to the software HEVC, the graph cannot keep up and does not stop in time, and mediaserverd is killed ("Timed out waiting for the
  capture graph to stop"); the movie is left without a moov. Photos under *Most Compatible* are JPEG.
- On the phone (image 321) the encoder takes 17.4 ms a 1080p frame (30 fps needs 33) and runs about 12% over the bitrate asked
  (17.1 Mbit/s for 15.3). A movie of image 319 checked on the Mac: H.264 Constrained Baseline 1920x1080 with AAC, decoded by ffmpeg
  without an error, but 315 frames in 13.4 s (23.6 a second) and a few repeated time stamps: frames are lost or doubled on the way.
- Image 319's first Camera session after boot showed a black viewfinder (it asked for the back, telephoto and front streams and never
  started the graph; closing and reopening Camera cured it). Images 320 and 321 opened live the first time.
- *Orientation (fixed 2026-11-12, image 321, seen on the phone standing upright):* the viewfinder of both cameras was turned a quarter
  turn counter-clockwise. The sensors' pictures were right (landscape, as the iPhone's: the vendor's tables flip the IMX689 and IMX471
  with register 0x0101 = 3, as Android does); the capture graph stands them up for the portrait screen with the M2 scaler's
  `Transform` option (4: a quarter turn clockwise), which `m2scaler.c` ignored. It now honours it (bits: 1 mirror left-right, 2
  top-bottom, 4 a quarter turn clockwise after them, as IOSurfaceAccelerator's own check reads them).
- The telephoto (S5K3M5, sensor 3) is offered as a third stream, "Back Telephoto Camera" (the iPhone X's `:2` device); AVFoundation lists it
  as `AVCaptureDeviceTypeBuiltInTelephotoCamera` (camera_avtest, image 321), so applications can use it. The iPhone X's 2x
  button belongs to its *Back Dual Camera*, which needs a `FigCaptureSynchronizedStreamsGroup` from the device; the device offers none yet.

**Photographs through iOS's own processing, OnePlus 8 Pro, 2026-11-12 (kernel data96/97, RAM images 322-361, verified on the phone):**
- *Real pixels.* The IMX689 ran its res4 (2000x1128, 4x4 binned, 16:9): a photograph was a 1504x1128 crop scaled up to 4032x3024. The
  kernel now runs res1 (4000x3000, 30 fps, the 48 MP array binned 2x2) for Photo and res4 for video (the stream's format 1): the stream
  asks with `HK_CAMERA_SENSOR` b = 1 or 4 and the kernel restarts the sensor (`cam_main=4`: res4 always, as before).
- *iOS's noise reduction (GNR).* The capture graph's `BWGNRNode` (GNR.videoprocessor, Metal) now processes every photograph. What it
  needed: the stream's `ModuleInfo` (`sensorID` 0x0343/0x0354/0x0353, so CMCapture finds the iPhone X camera's tuning in
  `CMCapture.framework/D22/CameraSetup.plist`, and lens/actuator/integrator IDs for its `FocalLengthCharacterization`); frames with
  `SensorID`, still frames with `BracketedCaptureSequenceNumber`; the white balance gains, `TotalSensorCropRect`, `ExposureBias` and an
  SNR from the signal; the Metal device in mediaserverd (now the default, `mediamtl=0` hides it); and from the Metal driver: textures
  of other texel formats over the same IOSurface or YCbCr plane (`-[FigMetalContext rebindTex:...]`), `-iosurface` on a texture, a
  texture "over a buffer" (memory of its own), integer formats. `photo_gnr=0` goes past GNR as before; `photo_gnr_trace=1` traces it.
- *Stopping.* With Metal in mediaserverd the scene classifier's sink (`BWSceneClassifierSinkNode`, an Espresso network whose MPS
  pooling kernel this driver does not dispatch) never went idle and mediaserverd was killed 10 s after Camera.app closed. It is
  now taken for idle at stop (`scene_classifier=1` leaves it). `media_stopwatch=1` lists a slow stop's sinks and the end of data.
- *Exposure.* The longest frame is the graph's `MinimumFrameRate`'s: in a dim room a recording ran at 15 fps; now 30 with more gain.
- *Photos' thumbnails were cyan* (then magenta): the grid's shader samples a two-plane YCbCr texture (Metal format 500) as RGB, which
  the GPU's YCbCr conversion gives. The driver's copy for it is now converted with BT.601 (`hkmetal.m`). Verified in the grid.
- Not yet: SIS (multi-frame low light, `photo_sis=1` asks for it; the stream gives one frame per request), the scene classifier,
  Live Photo, the Dual Camera (2x).

No Live Photo recording. See [../KNOWN_ISSUES.md](../KNOWN_ISSUES.md).

### Sensors, ambient light, location

**OnePlus 8 Pro (2026-11-07, verified on the phone, RAM image 174):** the sensors hang off the sensor DSP (SLPI), not the audio DSP.
`hk_slpi.c` (`slpi=1`) starts it the way `hk_adsp.c` starts the ADSP: firmware (`slpi.mdt`/`.bNN`) read at boot from the modem partition's
FAT (`image/`, `hk_fw_partition_file()` in `instantnoodle_modem.c`; nothing bundled), PAS 12 through TrustZone at 0x90600000, SMP2P
(ours 430, theirs 481), GLINK over IPCC client 4 with two channels: "IPCRTR" (QRTR, node 9) and "fastrpcglink-apps-dsp".

- **SEE client:** QMI service 400, `SNS_CLIENT_REQ` 0x20 carrying protobuf `sns_client_request_msg`; the SUID sensor (SUID `0xabab...`) is
  asked for each data type (`sns_suid_req`, msg 512, answer event 768), then each found sensor gets `std_sensor_config` (513) or on-change
  (514) and sends sensor events (1025) as indications 0x21/0x22. Values are in `kern.hackios.sensors`.
  Reported: accel (about (-0.5, -0.9, 10.0) m/s^2 flat on a table), gyro, ambient_light, rgb, proximity, gravity, game_rv, device_orient,
  pedometer, sig_motion, amd, motion_detect, gyro_cal. **Not found:** mag (AKM09918/MMC5603 on the DSP's I2C 1: not yet known why), pressure
  (probably not fitted).
- **FastRPC listener (`hk_fastrpc.c`):** SEE reads its registry and configs through the application processor's file service (Android's
  `adsprpcd`/`sscrpcd`). Without it SEE starts and finds nothing. The listener maps a 2 MB pool through the SMMU (stream 0x541, IOVA 0x80000000),
  attaches to the root process (pd 0) and the sensors process (pd 2), then serves `adsp_listener` `next2` calls: `apps_remotectl`, `apps_std`
  (fopen/fread/fwrite/fseek/stat/opendir/readdir/mkdir/frename...), paths mapped from `/mnt/vendor/persist/`, `/vendor/`, `/odm/`,
  `/data/vendor/sensors/` onto `/hackios/sensors/{persist,vendor,odm}` (read-only, in the image; from the phone's own partitions, not in git)
  and `/private/var/hackios/sensors` (writable; made by the kernel). Context ids carry the process in their low bits and are unique across both
  sessions (a collision hung the DSP after ~257 calls).
- **OnePlus SMEM item 130** (`struct sensor_info`, Android's `oplus_sensor_devinfo.c`): which parts are fitted, filled from the dt node
  `oplus,sensor-devinfo` before the DSP starts. Without it SEE registered no IMU. The bootloader allocates it (4768 bytes here, the 2-source layout).
- **Orientation (2026-11-07, built, not yet seen):** backboardd turns the screen by CoreMotion's `CMDeviceOrientationManager`; `inject.c`
  `orientation_start()` answers its three methods from `kern.hackios.sensor_values` (a struct per kind: events, values, timestamp) when the
  kernel has an accelerometer, else leaves CoreMotion's (`orient=0`: not at all). How backboardd uses it: research/ios-userland.md 1.5a.
- **Orientation verified (2026-11-07, RAM images 177-189):** face up (5) on a table, and portrait (1, y = +8.1 m/s^2) when the phone was picked up
  upright, back to 5 when laid down; the landscape signs (3/4) not yet seen.
  `slpi=1` is in loader 15's default arguments.
- **Proximity (2026-11-07; `prox=1` only: it makes backboardd abort, see BOOT_ARGS):** `inject.c` `proximity_start()` makes backboardd a service of DeviceUsagePage 0xff00 /
  DeviceUsage 8 once the DSP has a proximity sensor (waits up to 3 minutes for it), and sends kIOHIDEventTypeProximity with mask 0x40 (near the
  receiver) or 0 on each change of SEE's near/far, read 5 times a second (`prox=0`: none). The kernel keeps SEE's proximity events (message
  773/774, inferred from sns_proximity.proto: field 1 near 1 / far 0, field 2 raw ADC) in `kern.hackios.sensor_values`; until now only its
  configuration event (769) had been seen, so whether 773 is what arrives is not known yet.
- **Ambient light (2026-11-07; `als=1` only: it makes backboardd crash, see BOOT_ARGS; SEE's data[0] is the lux, 19.7 by day, verified):** a service of (0xff00, 4) in backboardd and kIOHIDEventTypeAmbientLightSensor
  events, SEE's lux (data[0], inferred) as the level and as field 0xc000b (the illuminance: what CoreBrightness itself writes there when it
  rewrites an event, 20F66 0x1af009b14). Off by default: with an ALS iOS turns auto-brightness on, and a wrong reading would hold the screen
  at its dimmest.
- **Compass / magnetometer (2026-11-13, kernel data106-108, RAM images 380-382; not working).** The Compass app opens and
  shows the dial fixed at 0: iOS has no heading because SEE registers no magnetometer (`mag`, `mag_cal`, `geomag_rv`, `rotv`
  all absent in `kern.hackios.sensors`). What is known:
  - The DSP does load the magnetometer registry: `kern.hackios.fastrpc_opens` (every name it opened, in order) lists
    `ak0991x_0*` and `mmc5603x_0_platform*` (config, orient, placement, fac_cal), `default_sensors.mag`, `sns_mag_cal_config`,
    `devinfo.mag`. The firmware has both drivers (`sns_ak0991x`, `sns_mmc56x3x`, strings in `slpi.b16`/`b21`).
  - Both parts sit on the DSP's I2C bus 1 (`bus_type` 0, `bus_instance` 1; MMC5603 at 0x30, AK09918 at 0x0c, IRQ 113),
    the only sensors on that bus; the ones that work are on SPI 3 (LSM6DSM) and I2C 5 (TCS3707). So the bus or the parts'
    supply (only `sensor_vddio` is listed for them) is the likely fault; nothing in the device tree shows an AP-side supply.
  - The DSP's own log would say. `slpi_dsplog=1` now also sets its FARF mask (`adsp_current_process`
    `set_logging_params(0x1f)`, accepted in both processes) and keeps what comes in `kern.hackios.dsplog`
    (lines about a magnetometer or a failure) and `kern.hackios.dsplog_last`. Nothing comes: the root process takes
    `adspmsgd_adsp1` init2 (static handle 5), but SEE runs in the sensors process, which loads the
    interface as a module from the AP (`libadspmsgd_adsp1_skel.so`, Android's `/vendor/lib/rfsa/adsp`), and that file is
    not in any local copy of the phone's vendor files (only part of `/vendor/lib/rfsa/adsp` and the `dsp` partition
    were pulled). Also asked for and not served: `oemconfig.so` (not on the phone either), `sns_bring_to_ear.so` and
    `wigig_sensing.so` (in the `dsp` partition's `sdsp/`), and an AP-side `sns_registry` module.
  - The phone's own `vendor_a` (EROFS in `super`, read block by block with `hk_lun`; `vendor_b`'s first extent is no
    file system start, probably a Virtual A/B snapshot) has the same 37 modules in `lib/rfsa/adsp` as the earlier pull and
    no `libadspmsgd_adsp1_skel.so`: Android does not load it either. Its way is `adspmsgd_adsp1` init3 (quic/fastrpc
    adspmsgd.c: heap 0, filter 0x1f001f, a 256 KB buffer, then `wait()`), for which the DSP asks the AP for the buffer
    through `apps_mem` (`request_map64`) and the AP maps it into the DSP (Linux fastrpc's INIT_MMAP on the process
    handle). Neither is served here yet.
  - (2026-11-13, kernel data109-113, images 385-390) Tried and ruled out:
    - `apps_mem` (request_map64 with a remote map, INIT_MMAP on handle 1) is served now, but `adspmsgd_adsp1` init3 in
      the sensors process fails (-1) without ever asking for memory, as init2 does: handle 5 is not the log there.
    - Diag over GLINK: `slpi_diag=1` opens `DIAG_CTRL`, `DIAG_DATA`, `DIAG_CMD` from our side. The DSP acknowledges and
      never opens its end (`kern.hackios.slpi_diag`). The only channels it opens unasked are `mhi_sat` and
      `LOOPBACK_CTL_DSPS`. Its QRTR services: 15 (inst. 48, 49), 24, 43, 51 (inst. 8, 12), 66, 400. No diag.
    - `sns_default=0` (every sensor of a type) and `sns_more=1` (more types): `kern.hackios.sns_suids` lists every SUID
      answer, and there is no `mag`, `mag_fusion`, `mag_cal` or `geomag_rv` at all.
    - SMEM 130 matches OnePlus's `oplus_sensor_devinfo.c` (LineageOS lineage-20) field for field. The device tree has no
      `ldo_enable` and no `mag_fusion` entry. SMEM 608 (sensor feedback) is 32 bytes and says nothing
      (`kern.hackios.smem_item` / `smem_dump`).
    - Booting without the audio DSP and sound (`adsp=0 sound=0 mic=0 voice=0`) changes nothing.
    - SEE rewrites `devinfo.<sensor>` for each sensor it brings up: gsensor, gyro, als, als_rear, ps, rgb, rgb_rear. It
      does not rewrite `devinfo.mag`.
  - Android did run a magnetometer here: `sns_mag_fusion.bias` holds a real, updated calibration (version 1, accuracy 2),
    and Android's `sensors_list.txt` has `geomag_rv`.
  - How the MEMSIC driver probes (slpi.b16, disassembled with LLVM's `llvm-objdump --triple=hexagon`; SEE's printf
    descriptors are {line|level, 8, format, 0} right before each string):
    `process_timer_events` turns the rail on, then "memsic sbefore read id", "Read chip id", and WHO-AM-I compared
    with 0x10; otherwise "Read WHO-AM-I error" / "memsic Unsupported Sensor", then "Find Sensor. hw_is_present".
    Both parts on bus 1 failing that read points at the bus or the parts' supply, which the DSP owns. Its messages go
    out only through Qualcomm diag, which this phone's sensor DSP offers neither over GLINK nor QRTR here.
  - (2026-11-13, local registry/JSON re-read, no phone) Power config confirmed: both mag parts use `num_rail=1`,
    `rail_on_state=1` and `vddio_rail=/pmic/client/sensor_vddio` **only** — no `vdd_rail` (`kona_qrd_mmc5603x_0.json`,
    `kona_qrd_ak991x_0.json`, and the parsed `persist/.../registry/*_platform.config`). The working ALS/SAR on I2C bus 5
    (`alsps`, `tcs*`, `sx*`) share that same `sensor_vddio`, so VDDIO is up and the mag's power config matches Android's.
    That rules power out and narrows the fault to **SSC I2C bus-instance 1 itself** (its LPI pins / pull-ups): bus_type 0
    (I2C) instance 1 for both mag parts, vs the working ALS on I2C instance 5 and accel on SPI instance 3 — so the I2C
    framework works, bus-instance 1 specifically does not. Bus 1 is LPI/DSP-owned and absent from the Linux dts (`op8p.dts`
    has no mag/mmc/akm node, as expected). The SSC `power_manager` registry defines `power.gpio.gpio_17/18/19`
    (`sleep_pull` set) but nothing maps them to bus 1.
  - (2026-11-13, SLPI firmware `slpi.b04`/`b16`, no phone) The SSC I2C/SPI buses are **QUPV3_2 SE0-5**
    (`QUPV3_2_SE0..SE5_GENI4_CFG/IMAGE/DATA` in `slpi.b04`), so bus_instance 1 = QUPV3_2 **SE1**. The firmware
    drives its own pins (`SSC_TLMM_CSR`, `LPASS_LPASS_TLMM_CSR`, `tlmm_core_init`) from per-board DAL configs
    `/tlmm/sm8250{mtp,qrd,cdp,qrd-iot}[0-3]` -- Qualcomm reference boards, none named for the OnePlus board, though
    this is OnePlus's own pulled firmware so one of them is presumably selected by soc_id. The working accel (SE3)
    and ALS (SE5) are on the **same** QUPV3_2, so the QUP is clocked and powered and **SE1 is the only failing
    serial engine** -- which rules out a QUP-wide fault and isolates it to SE1's own pins/pull-ups or the parts.
    The mag drivers' probe strings are in `slpi.b16`: `sns_ak0991x_sensor.c:sns_scp_register_com_port fail rc:%u`,
    `dsns_mmc5603x_sensor.c:get client req`, and an I3C path (`dsns_mmc56x3x_hal_island.c:assign i3c address
    failed; rv=%d`, `sns_com_port_i2c.c:CCC:SETDASA hndl:%p addr:0x%x new_addr:0x%x`) -- but the registry has the
    mag as `bus_type` 0 (plain I2C, static 0x30 / 0x0c), so the I3C SETDASA path should not run; which line
    actually fires is a DSP-side log that leaves only over the closed diag/QDSS. **Conclusion:** the fault is
    isolated to QUPV3_2 SE1 (the magnetometer's own bus), inside the DSP's domain. Confirming the exact cause
    needs the DSP diag (this SLPI opens none over GLINK or QRTR) or the board schematic for SE1's SCL/SDA pins and
    pull-up rail. A fix from our side (drive the mag over SE1 from the AP kernel, as `instantnoodle_i2c.c` drives
    the touch/slider buses) is possible in principle but needs those physical pins **and** would contend with the
    DSP for SE1, with CrashDump risk -- not safe to attempt blind.
  - Next, if picked up again: the SSC's own bus-1 pins and pull-up supply (from a schematic or the firmware's GPIO and
    rail tables), or a way to the DSP's diag. With a magnetometer: the heading for iOS (CoreLocation's `CLHeading`).
- **Not done:** CoreMotion for apps (locationd would want the iPhone X's motion coprocessor protocol). `slpi=1` is not in the default arguments yet. See [../TODO.md](../TODO.md) T47.

GPS belongs to the modem (QMI LOC, `userland/src/hk_location.m`, research/modem-sdx55.md). Redwood: not present.

## Files

`xnu/files/bsd/dev/hk_input.c`, `hk_goodix.c`, `hk_virtio_input.c`, `instantnoodle_touch.c`, `instantnoodle_i2c.c`,
`hk_buttons.c`, `instantnoodle_slider.c`, `hk_haptic.c`, `hk_torch.c`, `hk_battery.c`, `instantnoodle_battery.c`,
`hk_camera.c`; `xnu/files/iokit/Kernel/HKBattery.cpp`; `xnu/files/bsd/sys/hk_input.h`, `instantnoodle.h`. Userland repo:
`userland/src/inject.c`, `inject_sb.c`, `inject_media.c`, `camera_device.c`, `camera_isp.c`, `rwtouch.c`, `battery_test.c`,
`torch_test.c`.
