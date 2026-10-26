APRIORI DXE {
  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  INF Binaries/instantnoodlep/QcomPkg/Drivers/EnvDxe/EnvDxe.inf

  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  INF Binaries/instantnoodlep/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/PdcDxe/PdcDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/VcsDxe/VcsDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/SPMIDxe/SPMIDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/TzDxe/ScmDxeLA.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/TLMMDxe/TLMMDxe.inf

  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf

  INF Binaries/instantnoodlep/QcomPkg/Drivers/I2CDxe/I2CDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/PmicDxe/PmicDxeLa.inf

  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  INF Binaries/instantnoodlep/QcomPkg/Drivers/SdccDxe/SdccDxe.inf
  # UFSDxe left out on the OnePlus 8 Pro: both the baseline and this phone's own build-00358 UFSDxe assert
  # "Device Error" (UFS.c) when they re-initialise the UFS the stock bootloader already brought up; the kernel has its own UFS driver

  INF FatPkg/EnhancedFatDxe/Fat.inf

  INF Binaries/instantnoodlep/QcomPkg/Drivers/TzDxe/TzDxeLA.inf

  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf

  INF Binaries/instantnoodlep/QcomPkg/Drivers/FeatureEnablerDxe/FeatureEnablerDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/instantnoodlep/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf
!else
  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif 

  INF Binaries/instantnoodlep/QcomPkg/Drivers/PILProxyDxe/PILProxyDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/PILDxe/PILDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/SPSSDxe/SPSSDxe.inf
!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/instantnoodlep/QcomPkg/Drivers/CPRDxe/CPRDxe.inf
!endif

  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

  INF Binaries/instantnoodlep/QcomPkg/Drivers/SPIDxe/SPIDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/AdcDxe/AdcDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/UsbPwrCtrlDxe/UsbPwrCtrlDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/ChargerExDxe/ChargerExDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/XhciDxe/XhciDxe.inf

  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

  INF Binaries/instantnoodlep/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/TsensDxe/TsensDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/LimitsDxe/LimitsDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/GpiDxe/GpiDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/HashDxe/HashDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/CipherDxe/CipherDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/MacDxe/MacDxe.inf
  INF Binaries/instantnoodlep/QcomPkg/Drivers/RNGDxe/RngDxe.inf

  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
}
