##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = instantnoodlep
  PLATFORM_GUID                  = 696E415D-F1B7-55CF-A608-30B0CA2D4757
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/instantnoodlepPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = instantnoodlepPkg/instantnoodlep.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM8250
  # 1 = SM8250-AB
  # 2 = SM8250-AC
  #
  SOC_TYPE                       = 0

!include KonaPkg/KonaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"OnePlus"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"8 Pro"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"instantnoodlep"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"8Pro_instantnoodlep"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"8 Pro"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2376
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|instantnoodlepPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|instantnoodlepPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  #
  # ACPI Tables
  #
  kebab/AcpiTables.inf
