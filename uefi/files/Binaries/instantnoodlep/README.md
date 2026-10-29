## Firmware Infos

**Only this README is tracked.** The driver set is byte-for-byte the OnePlus 8's (`../instantnoodle`); `uefi/apply.sh` copies it to
`Mu-Silicium/Binaries/instantnoodlep` and puts this file over it. Nothing of the phone's firmware is in the repository.

- **Device:** OnePlus 8 Pro (instantnoodlep, IN2023), SM8250
- **Source:** the OnePlus 8 set, unchanged: the drivers of [Project-Silicium/Device-Binaries](https://github.com/Project-Silicium/Device-Binaries)
  `kebab` (OnePlus 8T, the same SoC, with Silicium's patches below) and, for the USB drivers (`UsbPwrCtrlDxe`, `UsbfnDwc3Dxe`,
  `UsbDeviceDxe`, `UsbConfigDxe`, `UsbInitDxe`), the OnePlus 8's own `BOOT.XF.3.2-00295-SM8250-1`.
- **Tried and failed (2026-10-26):** this phone's own USB drivers from `BOOT.XF.3.2-00358-SM8250-1` (extracted from its `xbl_b`
  with UEFIReader) on top of that baseline: `fastboot boot` was accepted, then the screen went black and the phone
  entered Qualcomm CrashDump (USB 05c6:900e) with no loader screen. Cause not proven; mixing builds is the suspect.
- `UsbConfigDxe` here is unpatched (kebab's removes the USB de-init at ExitBootServices).

## Patches/Fixes (kebab's set, from Device-Binaries)

### ButtonsDxe:

- Reason: Helps navigating Menus (e.g. UEFI Menu).
- Patch: Key code was patched for the power button to be mapped as ENTER instead of SUSPEND.
- Patch Creator: [Gustave Monce](https://github.com/gus33000)

### FeatureEnablerDxe:

- Reason: The TZ applet it already brought up.
- Patch: Both DXEs were patched to not start again the TZ applet.
- Patch Creator: [Gustave Monce](https://github.com/gus33000)

### UsbConfigDxe:

- Reason: Is Important to get USB working in Windows / Linux and UEFI.
- Patch Nr. 1: Exit BootServices routine was patched to not deinit USB after exit boot services.
- Patch Nr. 2: USB Mode has been Changed from Device Mode to Host Mode.
- Patch Creator: [Gustave Monce](https://github.com/gus33000) & [CloudSweets](https://github.com/cloudsweets)
