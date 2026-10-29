# Camera frame path on the OnePlus 8 Pro (SM8250 "kona", Titan 480): sensor to DRAM

Register-level specification for a bare-metal driver (no Linux framework; the sensor's I2C is bit-banged and works) that gets one
RAW10 frame of the main sensor (IMX689, slot 0, CCI0 master 0, CSIPHY 0) into DRAM. Research only: nothing here was run on
the phone. Written 2026-10-27 from Qualcomm's driver for this phone and the working POCO X5 Pro (redwood) code.

Source tags: **[A: file:function]** Android driver, under `op8pro-data/lineage-kernel/techpack/camera/drivers/` (shortened:
`csid` = `cam_isp/isp_hw_mgr/isp_hw/ife_csid_hw/`, `vfe` = `cam_isp/isp_hw_mgr/isp_hw/vfe_hw/`, `phy` = `cam_sensor_module/cam_csiphy/`);
**[DT:n]** line n of `op8pro-data/research/instantnoodlep-live.dts`; **[CC]** `op8pro-data/lineage-kernel/drivers/clk/qcom/camcc-kona.c` / `gcc-kona.c`;
**[R:n]** `hackios-op8pro/xnu/files/bsd/dev/hk_camera.c` line n (redwood); **[I]** `instantnoodle_camera.c` (this phone, works);
**[mod]** my decode of `com.qti.sensormodule.semco_imx689.bin` (tools/camsensor.py does not decode modes for the V2 files, see 6);
**[inf]** inference, not seen in a source.

## 0. Four findings that change the brief

1. **The IMX689's module file is 3-trio C-PHY, not 4-lane D-PHY.** `laneAssign` = 0x0210 (three nibbles), `isComboMode` = 0, all nine
   resolution elements say 3 lanes, the init table writes 0x0111=3 and every mode table 0x0114=2 [mod]; on the IMX586 family 0x0111=3 selects C-PHY
   and 0x0114=2 three trios [inf]. So: CSIPHY C-PHY registers (`csiphy_3ph_v1_2_1_reg`), CSID `lane_type` = 1, 3 lanes. The D-PHY table is in 3.7 as a fallback only.
2. **Redwood's IFE-lite is a different block.** Redwood: "vfe-lite165", bus ver2 (reset 0x18, WM0 at 0x2200, REG_UPDATE 0x4ac) [R:2345-2384]. Kona: "vfe-lite480",
   **bus ver3** (reset 0x0c, REG_UPDATE 0x20, WM0 at 0x1c00) [A: vfe/vfe17x/cam_vfe_lite48x.h]. The CSID-lite and CSIPHY layouts are (nearly) the same.
3. **No 2x2-binned ~2000x1500 main stream exists.** Binned 4000x3000 modes carry a second PDAF stream (VC1, DT 0x30). The smallest single-stream mode is
   **mode 4: 2000x1128, 4x4 binned, 240 fps nominal, one VC0/DT 0x2b stream** (6).
4. `instantnoodle_camera.c` labels GCC 0xb02c "gcc_camera_xo_clk": in `gcc-kona.c` 0xb02c is `gcc_camera_hf_axi_clk`; the XO clock is 0xb040 (`GCC_CAMERA_XO_CBCR`
   define, [I] line 126). Cosmetic, but the cam=1 print is wrong.

## 1. Recommended block chain

`IMX689 --(3 trios)--> CSIPHY 0 --> CSID-lite 0 (RX, RDI0) --> IFE-lite 0 (camif-lite RDI0 -> write master 0) --> camnoc --> DRAM`

| Block | Physical base | Size | SPI | Source |
|---|---|---|---|---|
| CSIPHY 0 (`qcom,csiphy-v1.2.1`) | 0x0ac6a000 | 0x2000 | 0x1dd | [DT:28469] (CSIPHY n: 0xac6a000 + 0x2000*n, n<=5) |
| CSID-lite 0 (`qcom,csid-lite480`, hw idx 2) | 0x0acd9200 | 0x1000 | 0x1d4 | [DT:30066] |
| IFE-lite 0 (`qcom,vfe-lite480`, hw idx 2) | 0x0acd9000 | 0x2200 | 0x1d5 | [DT:30086] (its range contains the CSID-lite's; the IFE-lite's registers used are 0x00-0x4c and 0x1200-0x1fff only: map both) |
| CPAS top (`qcom,cam-cpas`) | 0x0ac40000 | 0x1000 | camnoc 0x1cb | [DT:29430] |
| CAMNOC | 0x0ac42000 | 0x8000 | | [DT:29430] |
| camcc | 0x0ad00000 | 0x10000 (the driver maps 0x20000) | | [DT:4061] |
| GCC | 0x00100000 | | | [I] |
| refgen (CSIPHY reference) | 0x088e7000 | 0x84 | | [DT:8924] |
| apps SMMU (qsmmu-v500) | 0x15000000 | 0x100000 | | [DT:12413] |

(Other instances, not used: CSID0 0xacb5200 / IFE0 0xacb4000 [DT:29970]; CSID1 0xacc4200 / IFE1 0xacc3000; CSID-lite1 0xacdb400 / IFE-lite1 0xacdb200.)

**Why CSID-lite 0 + IFE-lite 0, RDI0, not the full IFE0.** (a) Power: both lite blocks have the single supply `camss` = `titan_top_gdsc` [DT:30066-30110]; the full IFE
also needs `ife_0_gdsc` (camcc 0xa004, [DT:4225]) and its own six clocks. (b) The RDI path is the raw pass-through: CSID RDI0 -> camif-lite RDI0 -> WM0, no pixel pipeline, no
UBWC. (c) Same choice as redwood, so the code shape carries over. (d) Android has an RDI-only context mode (`is_rdi_only_context`, [A: cam_ife_hw_mgr.c] lines 2780, 3023); which CSID/IFE it picks for it I did not trace.
Full IFE would need: GDSC 0xa004, `ife_0_{ahb,areg,axi,clk,cphy_rx,csid}` (camcc 0xa088/0xa030/0xa084/0xa028/0xa080/0xa058), top reset at 0x1c, REG_UPDATE 0x34, bus at 0xaa00,
RDI WMs 23-25 at 0xb... [A: vfe/vfe17x/cam_vfe480.h]; not worth it. CSID-lite's RX `phy_sel` takes 0-5 (`csi2_rx_phy_num_mask` 0x7) [A: csid/cam_ife_csid_lite480.h] and Android sets
`phy_sel = res_type - PHY_0` [A: cam_ife_csid_core.c:cam_ife_csid_cid_reserve], so CSIPHY 0 -> `phy_sel` 0. Whether lite 0 can really take PHY 0 is **[inf]** (nothing in the
driver restricts it; the first hardware test of 4.A does not need a PHY at all).

## 2. Prerequisites: rails, power domain, clocks

### 2.1 Already on after `instantnoodle_camera.c` `power_up()` [I:437-554]
Sensor rails and GPIOs (BOB vote, GPIO 156/26, ldof1, smpa5, ldof7, ldof5+GPIO 93), RPMh `mmcx.lvl` at 64 (LOW_SVS), GCC `gcc_camera_ahb_clk` 0xb008, `titan_top` GDSC
(camcc 0xc144, bit0 = collapse, bit31 = on), MCLK0 (RCG 0x5000/cfg 0x5004 source 0 = XO 19.2 MHz, branch 0x5018, GPIO 94 function 1), reset GPIO 145 high, chip ID read. The new frame
path goes between `read_id()` and `power_down()`. Note `power_down()` leaves the GCC AHB clock and the MMCX vote on (a crash after giving them back, [I:673-678]): keep that.

### 2.2 To add (votes and rails)
| What | How | Source |
|---|---|---|
| MX rail at SVS_L1 (192) for camcc PLL0 at 1200 MHz (the PLL's `vdd_mx` class: 1066 MHz at SVS, 1600 at SVS_L1) | `instantnoodle_rail_level("mx.lvl", 192)` | [CC: cam_cc_pll0 `.rate_max`], [R:1753-1754] |
| MMCX at >= 64 for all 400 MHz clocks below (`VDD_LOWER` = LOW_SVS 64 carries cphy_rx / csid-lite / ife-lite at 400 MHz, camnoc_axi at 300 MHz) | already voted (64) | [CC: rate_max], vdd-level.h |
| CSIPHY's 1.2 V supply: `mipi-csi-vdd` = `pm8150_l9` (RPMh `ldoa9`, 1.2 V, `csi-vdd-voltage` 0x124f80) | `instantnoodle_supply("ldoa9", 1200, true)` (the Wi-Fi driver already votes the same resource, [instantnoodle_wifi.c:283]); Android's logs show L9 enabled with 1-4 users in every state (`android-logs/regs-*.txt` line 11: display, Wi-Fi and others hold it) | [DT:28469, 6766] |
| refgen (PHY reference): `qcom,refgen-kona-regulator`, enable = bit 0 of the register at +0x80; **Android's idle log has it `disabled`, users 0** (`android-logs/regs-idle.txt` line 60): do not expect it on | read 0x088e7080 first; if bit0 = 0 write 1 (read-modify-write); undo at the end | [CC-less: drivers/regulator/refgen.c:refgen_kona_enable] |
| bus bandwidth (Android votes 10.6 GB/s on the camnoc/mnoc paths) | not done in redwood and it worked; **risk R6** | [A: vfe/cam_vfe_soc.c:cam_vfe_enable_soc_resources] |

### 2.3 Clocks (all camcc unless GCC; "idx" = parent select in CFG_RCGR[10:8], div field = 2*div-1 in [4:0], M/N mode bits [13:12] = 0)
PLL0 parents of `cam_cc_parent_map_0`: XO 0, PLL0 main 1 (1200 MHz), PLL0 even 2 (600), PLL0 odd 3 (400), PLL2 5 [CC: cam_cc_parent_map_0].

| Clock | RCG cmd / cfg | Branch (CBCR) | Rate and source (Android's ask) | Source |
|---|---|---|---|---|
| gcc_camera_ahb_clk | | GCC 0xb008 | on already | [CC gcc-kona.c] |
| gcc_camera_hf_axi_clk / sf_axi | | GCC 0xb02c / 0xb030 | bus clocks, enable only | [CC gcc-kona.c], cpas node [DT:29430] |
| slow_ahb_clk_src | 0x7058 / 0x705c | | 80 MHz = even/7.5: idx 2, div field 14 (`mnd_width` 8, no M/N) | [CC cam_cc_slow_ahb_clk_src] |
| cpas_ahb (`cpas_ahb_clk`) | | 0xc0f4 | slow_ahb | [CC] |
| core_ahb (`cpas_core_ahb_clk`) | | 0xc150 (BRANCH_HALT_DELAY: no CLK_OFF poll) | slow_ahb | [CC] |
| camnoc_axi_clk_src | 0xc0f8 / 0xc0fc | | 300 MHz = even/2: idx 2, field 3 (Android: 300 at lowsvs, 400 at svs; 19.2 XO at minsvs) | [CC], cpas rates [DT:29430] |
| camnoc_axi_clk | | 0xc114 | camnoc_axi_clk_src | [CC] |
| cphy_rx_clk_src (shared by CSIPHY and CSID) | 0xa068 / 0xa06c | | 400 MHz = odd/1: idx 3, field 1 | [CC], csiphy0 [DT:28469] |
| csiphy0_clk | | 0x601c | cphy_rx_clk_src (DT rate 0) | [CC] |
| csi0phytimer_clk_src | 0x6000 / 0x6004 | | 300 MHz = even/2: idx 2, field 3 | [CC], [DT:28469] |
| csi0phytimer_clk | | 0x6018 | | [CC] |
| ife_lite_csid_clk_src (`ife_csid_clk_src`) | 0xc01c / 0xc020 | | 400 MHz: idx 3, field 1 | [CC], csid-lite0 [DT:30066] |
| ife_lite_csid_clk | | 0xc034 | | [CC] |
| ife_lite_cphy_rx_clk (`ife_cphy_rx_clk`) | | 0xc03c | cphy_rx_clk_src | [CC] |
| ife_lite_clk_src (`ife_clk_src`) | 0xc000 / 0xc004 | | 400 MHz at lowsvs = odd/1: idx 3, field 1 (480 MHz at svs+ = PLL2/1.5 on kona-v2; not needed) | [CC: ftbl_cam_cc_ife_lite_clk_src(_kona_v2)], [DT:30066] |
| ife_lite_ahb | | 0xc040 | slow_ahb | [CC] |
| ife_lite_axi (`ife_lite_axi`) | | 0xc044 | camnoc_axi_clk_src | [CC], ife-lite0 [DT:30086] |
| ife_lite_clk (`ife_clk`) | | 0xc018 | ife_lite_clk_src | [CC] |
| cam_cc_gdsc_clk (XO, keeps the GDSC logic running; `CLK_IS_CRITICAL`) | | 0xc16c | read it: must be on | [CC cam_cc_gdsc_clk] |

Android enables every clock of the IFE-lite at TURBO (`cam_vfe_enable_soc_resources`) and the CSID at the level of its configured rate; 400 MHz everywhere is the lowest level at which the DT gives csid/ife-lite a rate
([DT:30066]: csid src 400 MHz at every level; ife src 400 at lowsvs). Redwood used PLL0 + the same RCG scheme ([R:1929-1999]); the camcc offsets all differ (list in 3.2).

**camcc PLL0 (Lucid, at camcc+0x0; the register layout is the same as redwood's [R:1839-1849])**: values differ from redwood's (yupik) in two registers [CC: cam_cc_pll0_config]:
L 0x3e, CAL_L 0x44, ALPHA 0x8000, CONFIG_CTL 0x20485699, CONFIG_CTL_U 0x00002261, **CONFIG_CTL_U1 0x329a699c** (redwood 0x329a299c), **USER_CTL 0x00003100** (redwood 0x3101),
USER_CTL_U 0x00000805, USER_CTL_U1 0 -> 1190.4 + 9.6 = 1200 MHz; even post-div field (bits 11:8) = 1 (/2 = 600), odd (15:12) = 3 (/3 = 400). Enable flow as redwood `pll0_up()` [R:1866-1905]
(that is `clk_lucid_pll_configure` + `alpha_pll_lucid_enable`): update-bypass, outputs off, standby (OPMODE 0x38 = 0), out of reset (MODE bit2), OPMODE = 1, wait MODE bit31 (lock), USER_CTL |= 7, MODE |= 1.
Read MODE first: if bit0 or bit31 is already set somebody else runs it (redwood gives up in that case).

## 3. Bring-up sequence

Conventions: `C` = CSID-lite 0 base 0xacd9200, `I` = IFE-lite 0 base 0xacd9000, `P` = CSIPHY 0 base 0xac6a000. Every write is read back where the register keeps its value (redwood's `blk_put`).
**A read of a block whose clock is off is a bus error and a CrashDump** (docs/research/hardware-qualcomm.md section 1): do the reads in the order below, never earlier.

### 3.1 Steps
**S0 (reads only, already possible):** GCC 0xb008, 0xb02c, 0xb030, 0xb040; camcc 0xc144 (GDSC), 0xc16c, PLL0 MODE 0x0, the RCG/CBCR offsets of 2.3; refgen +0x80; SMMU sCR0 / IDR / SMR / S2CR (global registers only; context banks are hands-off, the display's are CB4/5 and crash on read).

**S1 votes:** `mx.lvl` 192, `ldoa9` 1200 mV, refgen bit (2.2). [R:1819-1836 `rails()`] adapted: kona needs MX and MMCX, not CX.

**S2 GDSC:** titan_top already on [I]; if not: clear bit0 of camcc 0xc144, wait bit31 (<=100 us) [R:903-922 with 0xc144 instead of 0xc194].

**S3 GCC AXI branches:** set bit0 of GCC 0xb02c and 0xb030, wait CLK_OFF (bit31) clear [R:1944 gcc 0x26010/0x2601c -> kona 0xb02c/0xb030].

**S4 PLL0** as in 2.3.

**S5 RCGs** (write CFG_RCGR = (old & ~0x371f) | idx<<8 | field, then CMD_RCGR |= 1, wait bit0 clear): slow_ahb 0x7058 (idx 2, 14), camnoc_axi 0xc0f8 (2, 3),
cphy_rx 0xa068 (3, 1), csi0phytimer 0x6000 (2, 3), ife_lite_csid 0xc01c (3, 1), ife_lite 0xc000 (3, 1) [R:1929-1972 `clocks_up()`, offsets and PLL0 outputs per 2.3].

**S6 branches** (set bit0, wait bit31 = 0; skip the poll for 0xc150): 0xc0f4 cpas_ahb, 0xc150 core_ahb, 0xc114 camnoc_axi, 0x601c csiphy0, 0x6018 csi0phytimer,
0xc034 ife_lite_csid, 0xc03c ife_lite_cphy_rx, 0xc040 ife_lite_ahb, 0xc044 ife_lite_axi, 0xc018 ife_lite [R:1943-1995].
*First read after this:* CPAS top 0xac40000 + 0x0 (camera version; Android requires major 4, minor 8, incr 0 = 0x00040800 [A: cam_cpas/cpas_top/cam_cpastop_hw.c:cam_cpastop_get_hw_info]); +0x4 cpas version;
this proves AHB + GDSC + clocks. Then IFE-lite 0xacd9000 + 0x0 / +0x4 / +0x8 (HW_VERSION, TITAN_VERSION, HW_CAPABILITY), CSID-lite +0x0 (HW_VERSION), CSIPHY (needs only its own clocks) +0x0.. (values unknown: log them).

**S7 memory and SMMU** (see 5.3): allocate one contiguous region, hand it to the loader as redwood does [R:2082-2117 `frames_alloc()`]; make the IFE-lite's address valid (S7a bypass or S7b context).

**S8 CSID-lite reset** [A: csid/cam_ife_csid_core.c:cam_ife_csid_global_reset + cam_ife_csid_reset_regs]; identical to redwood `csid_reset()` [R:2417-2456]: C+0x24 = 0 (RX irq mask), C+0x34/0x44/0x54/0x64 = 0 (RDI irq masks),
C+0x78 = 1 (top clear), C+0x28 = 0xfffffff, C+0x38/48/58/68 = 0x7fff, C+0x80 = 1 (irq cmd), C+0x104 = 0x80, C+0x200/300/400/500 = 2 (RDI CFG0), then twice (hw reset first: strobe 0x1e, then registers: strobe 0x1):
C+0x78 = 1, C+0x80 = 1, [hw pass: C+0x74 = 1, C+0x80 = 1], C+0x10 = 0x1e (hw) or 0x1 (regs), [regs pass: C+0x74 = 1, C+0x80 = 1], wait C+0x70 bit0, then C+0x78 = 1, C+0x80 = 1. SAME as redwood.

**S9 IFE-lite reset and init** [A: vfe/cam_vfe_core.c:cam_vfe_reset, vfe/vfe_top/cam_vfe_top_ver3.c:cam_vfe_top_ver3_reset / cam_vfe_top_ver3_init_hw, vfe/vfe_bus/cam_vfe_bus_ver3.c:cam_vfe_bus_ver3_init_hw] **DIFFERS from redwood**:
1. I+0x28 = 0x00020000 (top IRQ mask 0: only "reset done", `CAM_VFE_LITE_48X_TOP_RESET_MASK`).
2. I+0x0c = 0x00000002 (`CAM_VFE_LITE_HW_RESET_AND_REG_VAL`, GLOBAL_RESET_CMD). Wait I+0x40 (IRQ_STATUS0) bit17.
3. I+0x34 = 0xffffffff, I+0x38, I+0x3c likewise (IRQ_CLEAR0-2), I+0x24 = 1 (IRQ_GLOBAL_CLEAR). Mask everything again: I+0x28 = I+0x2c = I+0x30 = 0.
4. Clock-gating overrides: I+0x10 = 0xffffffff (CORE_CGC_OVD_0), I+0x14 = 1 (AHB_CGC_OVD), I+0x18 = 1 (NOC_CGC_OVD) [A: cam_vfe_top_ver3_init_hw]. (Android also writes 0xff to the `core_cgc_ovd_1` slot, which the lite header leaves at offset 0 = the read-only version register: **skip it**.)
5. Bus: I+0x1a08 = 0x03ffffff (BUS_CGC_OVD), I+0x1adc = 0 (TEST_BUS_CTRL) [cam_vfe_bus_ver3_init_hw].

**S10 write master 0 (RDI0), frame mode** [A: cam_vfe_bus_ver3.c:cam_vfe_bus_ver3_acquire_wm (lite RDI branch), _start_wm, _update_wm; cam_vfe_bus_ver3_update_hfr]:
| Register | Value | Meaning / source |
|---|---|---|
| I+0x1c64 BURST_LIMIT | 0xf | start_wm; as redwood [R:2526] |
| I+0x1c0c IMAGE_CFG_0 | 0x0000ffff (height 0 << 16 \| width 0xffff) | RDI: `CAM_VFE_RDI_BUS_DEFAULT_WIDTH`, height 0 (ver2 used 0xff01) |
| I+0x1c18 PACKER_CFG | 0 (PLAIN_128: bytes pass untouched) | `is_lite` forces `pack_fmt` 0 |
| I+0x1c14 IMAGE_CFG_2 (stride) | **ALIGN16(width*10/8)** (mode 4: 2512) | Android writes `plane_stride` (CamX's, 16-aligned: `ALIGNUP(.,16)`); value for RAW10 **[inf]** |
| I+0x1c30/0x1c34 IRQ_SUBSAMPLE period/pattern | 0 / 1 | HFR blob defaults [inf]; redwood writes the same on ver2 [R:2529-2530] |
| I+0x1c38/0x1c3c FRAMEDROP period/pattern | 0 / 1 | same [inf]; ver3 code only writes them from the HFR blob (`cam_vfe_bus_ver3_update_hfr`) |
| I+0x1c04 IMAGE_ADDR | IOVA (or PA) of the frame buffer, 32 bits | update_wm: `image_buf + wm_data->offset` |
| I+0x1c08 FRAME_INCR | stride*height (mode 4: 2833536 = 0x2b3c80) | `plane_stride * slice_height` |
| I+0x1c00 CFG | **0x00010001** (bit0 enable, bits [17:16] = 1 = frame based mode) | `en_cfg = (0x1<<16)\|0x1`; uapi: wm_mode 0 line, 1 frame, 2 index [A: include/uapi/media/cam_isp.h, cam_vfe_bus_ver3_update_wm_config] |
| I+0x1a18 BUS_IRQ_MASK0 | (1<<4) \| (1<<0) \| 0xc0000000 | comp-group-0 done (comp_grp 0 + `comp_done_shift` 4), RUP RDI0, the two error bits (`bus_error_irq_mask`); we poll, GIC stays masked |
| I+0x20 REG_UPDATE_CMD | 0x1 (RDI0) | `reg_update_cmd_data` [vfe48x_camif_rdi_reg_data[0]]; commits the WM state at the next SOF |
| I+0x74 TOP_DEBUG_CFG | 1 | optional; camif-lite start writes it |
The camif-lite RDI0 (0x1200..) needs no programming: `cam_vfe_camif_lite_resource_start` only writes REG_UPDATE and TOP_DEBUG_CFG when `is_ife_lite` [A: vfe/vfe_top/cam_vfe_camif_lite_ver3.c:236-285].
Redwood equivalent [R:2511-2538 `ife_config`]: same idea, other offsets, constant stride 0xff01, CFG 3.

**S11 CSID-lite RX and RDI0** [A: cam_ife_csid_core.c:cam_ife_csid_enable_csi2 (1494), cam_ife_csid_init_config_rdi_path (2126), cam_ife_csid_get_format_rdi (76), cam_ife_csid_enable_rdi_path (2567)]; redwood [R:2460-2496 `csid_config`]:
| Register | Value for the sensor (VC 0, DT 0x2b, 3 trios C-PHY) |
|---|---|
| C+0x100 RX_CFG0 | (lanes-1)=2 \| lane_cfg<<4 (0x210<<4 = 0x2100) \| phy_sel<<20 (0) \| lane_type<<24 (1, C-PHY) = **0x01002102** |
| C+0x104 RX_CFG1 | (1<<6) \| 1 = 0x41 (+ (1<<2) when VC > 3: needed for the TPG's VC 0xa; redwood omitted it, [inf] harmless) |
| C+0x200 RDI0_CFG0 (first) | vc<<22 \| dt<<16 \| cid<<27 \| 0xf<<12 (RAW10 in, RAW10 out: decode 0xf, plain 0) \| 0<<10 \| 1<<2 \| 3; for VC 0, DT 0x2b, CID 0: **0x002bf007** (TPG, VC 0xa: 0x02abf007) |
| C+0x204 RDI0_CFG1 | 2 (`CSID_TIMESTAMP_STB_POST_IRQ`) |
| C+0x2b0 ERR_RECOVERY_CFG0 | **0x9** (overflow_ctrl_en 1 \| 8: detect overflow). NEW on 480; redwood does not write it |
| C+0x20c/0x210 FRM_DROP pattern/period | 0 / 1 |
| C+0x214/0x218 IRQ_SUBSAMPLE pattern/period | 0 / 1 |
| C+0x224/0x228, 0x22c/0x230 pix/line drop pattern/period | 0 / 1 each |
| C+0x208 RDI0_CTRL | 0 (halt mode) then later 1 (`CAM_CSID_RESUME_AT_FRAME_BOUNDARY`) |
| C+0x200 RDI0_CFG0 (second) | first value \| 1<<31 (path enable) |
| C+0x24 RX_IRQ_MASK | RST_DONE bit27, TG_FIFO_OVERFLOW 26, lane FIFO overflows 15-18, CPHY EOT 11 / SOT 12 / PH_CRC 13, CRC 19, ECC 20, MMAPPED_VC_DT 21, STREAM_UNDERFLOW 23, UNBOUNDED_FRAME 24 (as `cam_ife_csid_enable_csi2`); polling only |
| C+0x34 RDI0_IRQ_MASK | RST_DONE bit1, FIFO_OVERFLOW 2, CCIF_VIOLATION 15, OVERFLOW_RECOVERY 17, plus SOF bit12 and EOF bit9 for polling |
Differences to redwood's CSID code: `phy_sel` mask 0x7 here (redwood's lite-17x mask is 0x3 and it writes `phy & 3`), **C-PHY bit 24 set**, ERR_RECOVERY_CFG0 write, a 480-only packing_fmt field (bit 30) and drop-enable bits 3/4 of CFG0 that are 0 here.
Everything else (offsets, reset, TPG) is identical: the CSID-lite 17x and 480 headers differ only in those fields [diff of `cam_ife_csid_lite17x.h` / `cam_ife_csid_lite480.h`].

**S12 (rung B only) CSIPHY** see 3.5. **S13 (rung B only) sensor stream on** see 6.

**S14 wait for the frame** see 5. **S15 teardown (reverse)**: sensor off (0x0100 = 0, then wait ~30 ms), C+0x34 = 0, C+0x208 = (ctrl & ~3) | 2 (halt immediately) [A: cam_ife_csid_disable_rdi_path],
C+0x200 &= ~(1<<31), C+0x100 = 0, I+0x1c00 = 0, I+0x20 = 1, 1 ms; CSIPHY reset (3.5, step 1); branches off in reverse (S6), RCGs back to found values, PLL0 off [R:1908-1920], SMMU back [R:2265-2289], votes back (not MMCX/GCC AHB: see 2.1).

### 3.2 What differs from redwood (summary)
| Item | Redwood (SM6375) | Kona (SM8250) |
|---|---|---|
| CSID-lite | 0xacc8000 | 0xacd9200 (registers SAME offsets, + 0x2b0 etc.) |
| IFE-lite | 0xacc4000, bus ver2: reset 0x18, REG_UPDATE 0x4ac, bus 0x2000, WM0 0x2200.. | 0xacd9000, **bus ver3**: reset 0x0c, REG_UPDATE 0x20, bus 0x1a00, WM0 0x1c00.. |
| CSIPHY | 0xace0000 + 0x2000*n, D-PHY write set with m+4 = 0x07 | 0xac6a000 + 0x2000*n; C-PHY table in 3.6 (per-trio differences, other data-rate rows); D-PHY m+4 = 0x03 |
| camcc RCG/CBCR | slow_ahb 0x7058, camnoc 0xc124, cphy_rx 0xa064, csid 0xc020, ife 0xc004; branches 0xc11c/0xc140/0xc038/0xc040/0xc01c; GCC 0x26010/0x2601c | slow_ahb 0x7058 (same), camnoc 0xc0f8, cphy_rx 0xa068, csid 0xc01c, ife 0xc000; branches 0xc0f4/0xc114/0xc034/0xc03c/0xc018 + 0xc040, 0xc044, 0xc150; GCC 0xb02c/0xb030 |
| titan_top GDSC | camcc 0xc194 | camcc 0xc144 |
| PLL0 | yupik config | kona config (CONFIG_CTL_U1, USER_CTL differ) |
| rails | CX 128 + MX 192 | MX 192 + MMCX >= 64 (+ ldoa9, refgen) |
| SMMU stream | 0x800 mask 0x4e0 | ids 0x800 0x801 0x840 0x841 0xc00 0xc01 0xc40 0xc41 = **0x800 mask 0x441** [DT:29287] |
| frame done | BUS_IRQ_STATUS1 bit0 | BUS_IRQ_STATUS0 (I+0x1a28) bit 4 |
| TPG, RX, RDI, reset | | same, plus the CSID differences above |

### 3.3 Optional: CAMNOC (only if the WM stalls, see R5)
Android writes at power-on, to camnoc (0xac42000 + offset), for the IFE RDI write port [A: cam_cpas/cpas_top/cpastop_v480_100.h, `CAM_CAMNOC_IFE_RDI_WR`, applied by cam_cpastop_poweron]:
PRIORITYLUT_LOW 0x1430 = 0x66665433, PRIORITYLUT_HIGH 0x1434 = 0x66666666, URGENCY_LOW 0x1438 = 0x1030, DANGERLUT_LOW 0x1440 = 0xffffff00, SAFELUT_LOW 0x1448 = 0x000f.
Read those five first and compare with the values above; write only if they differ and a frame does not arrive. No TCSR/SCM errata write exists for Titan 480 (`tcsr_camera_hf_sf_ares_glitch` disabled) and the `SMMU_SE_IFE` safe-LUT SCM call is Titan 170/175 only [A: cam_ife_hw_mgr.c:cam_ife_notify_safe_lut_scm].

### 3.4 CSIPHY clocks and supplies
Regulators of the PHY node: `gdscr` = titan_top, `refgen` = 0x88e7000, `mipi-csi-vdd` = pm8150_l9 1.2 V [DT:28469]; clocks `cphy_rx_clk_src` 400 MHz, `csiphy0_clk`, `csi0phytimer_clk_src` 300 MHz, `csi0phytimer_clk`
(Android enables at SVS and sets the source rate: `cam_csiphy_enable_hw`) [A: phy/cam_csiphy_soc.c:cam_csiphy_enable_hw].

### 3.5 CSIPHY programming (C-PHY, 3 trios) [A: phy/cam_csiphy_core.c:cam_csiphy_reset, cam_csiphy_config_dev, cam_csiphy_cphy_data_rate_config, cam_csiphy_cphy_irq_config; phy/include/cam_csiphy_1_2_1_hwreg.h]
Redwood runs the same sequence for its C-PHY [R:2721-2861 `csiphy_common`, `csiphy_cphy`, `csiphy_up`]. All writes are to `P`.
1. **Reset** (`csiphy_reset_reg_1_2_1`): 0x814 = 0, **wait 5 ms** (Android's delay field; redwood spins 5 us), 0x818 = 0, 0x81c = 0, 0x800 = 1, 0x800 = 0.
2. **Common**: 0x814 = lane_enable = bits (2n+1) per trio = **0x2a** for trios 0,1,2 (D-PHY would be 0xd5); 0x818 = 0x01; 0x81c = 0x52 (3-phase); 0x800 = 0x0e.
3. **Per trio t (0,1,2)**, `b` = 0x100 + 0x200*t, `m` = 0x980 + 0x100*t, 34 writes in this order (values for trio 0; differences for trio 1 / trio 2 given):
   m+0x10 = 0, m+0x14 = 0, m+0x18 = 0, m+0x10 = 0x08, m+0x14 = 0x08 (trio 1: 0x00), m+0x18 = 0x1a (trio 1: 0x1f), m+0x0c = 0xaf (trio 1: 0xbf), b+0x68 = 0xac, b+0x04 = 0x06,
   **b+0x0c = settle & 0xff**, **b+0x08 = settle >> 8**, b+0x14 = 0x20, b+0x50 = 0, b+0x88 = 0, b+0x8c = 0x7f, b+0x90 = 0x7f, b+0x18 = 0x3e, b+0x1c = 0x41, b+0x20 = 0x41, b+0x24 = 0x7f,
   b+0x28 = 0, b+0x2c = 0, b+0x60 = 0x02, b+0xcc = 0x41, b+0x64 = 0x33, b+0xdc = 0x50, m+0x40 = 0x80, m+0x44 = 0x7d, m+0x48 = 0x7f, m+0x04 = 0x20, m+0x08 = 0x05, m+0x00 = 0x60, m+0x30 = 0x01, 0x800 = 0x0e.
   (Absolute: trio 0 b = 0x100/m = 0x980, trio 1 0x300/0xa80, trio 2 0x500/0xb80. Lane k of the table is addressed by lane_mask bit k, so with lane_mask 0x7 all three are written.)
4. **Data-rate row** (`data_rate_delta_table_1_2_1`, first row whose `bandwidth` >= the trio's rate in bit/s; rows 5.7e9, 7.98e9, 10.26e9): for mode 4 (about 2.5 Gbit/s per trio; any plausible definition stays below 5.7e9) **row 0**, 12 writes:
   0x15c = 0x66, 0x35c = 0x66, 0x55c = 0x66, 0x9b4 = 0x03, 0xab4 = 0x03, 0xbb4 = 0x03, 0x144 = 0x22, 0x344 = 0x22, 0x544 = 0x22, 0x16c = 0xad, 0x36c = 0xad, 0x56c = 0xad
   (redwood's rows differ: its row 0 is 0x32/0x0b/0x25 [R:2775]).
5. **IRQ masks** (`csiphy_irq_reg_1_2_1`): 0x82c.. step 4 = ff ff fb ff 7f ff ff ef ff ff ff (11 registers). Status for logging: 0x8b0 + 4i (i < 11) [R:2864-2872 `show_csiphy`]; clear 0x858 + 4i, global IRQ command 0x828 (1 then 0).
6. **Settle count**: Android computes `settle_cnt = settle_time / 200000000` (CamX's value) and writes it as above. **It is not in the V2 module file** [mod]: unknown, risk R2. Start: 0x22 (34, the value of redwood's C-PHY sensor from its module file); table default is 7. Sweep 0x07..0x30 by boot-arg, read CSID RX CRC/ECC/PH_CRC counters after each.

### 3.6 D-PHY fallback (not what this sensor does)
`csiphy_2ph_v1_2_1_reg` [A: phy/include/cam_csiphy_1_2_1_hwreg.h]: lane blocks (b, m): data lane 0 (0x000, 0x900), clock lane (0x700, 0xc80), data lane 1 (0x200, 0xa00), 2 (0x400, 0xb00), 3 (0x600, 0xc00); per lane 20 writes:
b+0x30 = 0, m+0 = 5, m+8 = 0x10, m+4 = 0, m+4 = 3, b+4 = 0xc, b+0x2c = 1, b+0x34 = 7, b+0x10 = 2, b+0x1c = 8, b+0x3c = 0xb8, b+8 = settle (default 0x10), b+0 = 0x8d (clock lane 0x80),
b+0xc = 0 (clock lane 0xa5), b+0x38 = 0xfe (clock lane 0x1f), b+0x14 = 0x60, b+0x28 = 0 (clock lane 4), b+0x24 = 0 (not clock), 0x800 = 2, 0x884 = 1. Common: 0x814 = 0xd5-style enable, 0x818 = 1, 0x81c = 2, 0x800 = 2.

## 4. The two rungs

### 4.A Rung A: CSID test pattern through RDI0 / IFE-lite 0 into memory (no sensor, no CSIPHY, no I2C)
Run S0-S11 with the TPG instead of a PHY, then start the generator. Redwood path: `capture_tpg()` [R:2613-2651].
- RX_CFG0 with `phy_sel` 0 and 4 lanes as redwood (`lane_cfg` 0x3210, D-PHY bit clear: 3 \| 0x3210<<4 = **0x00032103**) [R:2462-2468]; the TPG is independent of the PHY.
- TPG setup [A: cam_ife_csid_config_tpg:1440; same as redwood R:2469-2481]: C+0x604 = (0<<16)|(1<<10)|0xa (VC 0xa), C+0x608 = (0x3ff<<12)|0x740, C+0x60c = 0x12345678, C+0x610 = width<<16|height, C+0x614 = 0x2b,
  C+0x618 = (in_format - 1)<<16 | 0x8 with in_format 3 (RAW10, as redwood `(3u-1)<<16 | 0x8`) = 0x00020008, C+0x640 = 1<<5 (colour bars), C+0x648 = 0.
- RDI0_CFG0 with **vc = 0xa, dt = 0x2b** (`cam_ife_csid_path_reserve` forces DT 0x2b / VC 0xa for the TPG, line 1106).
- Start: C+0x600 = (4<<20) | (0x80<<8) | ((lanes-1)&3)<<4 | 7 = 0x00408037 for 4 lanes [A: cam_ife_csid_tpg_start; R:2502].
- Suggested size 1600x1200 as redwood (RAW10: 2000 B per line, stride 2000 -> IFE stride ALIGN16 = 2000, which is a multiple of 16: choose a TPG width whose line is *not* a multiple of 16 B on a second run, e.g. 1604, to see the line pitch the WM really uses).
- Expected at the end: CSID RDI0 byte counters C+0x2e0/0x2e4 = width*10/8*height; WM0 wrote `frame_incr` bytes from IMAGE_ADDR; colour-bar pixels in memory (unpack as 5.2).
This rung proves S1-S11, the SMMU/IOVA path, the WM stride/frame-mode guess and the completion detection without the PHY or the sensor.

### 4.B Rung B: the sensor's own frames
Redwood path: `capture_sensor()` [R:3118-3232]. Order (same as Android): power_up (exists), chip ID (exists), sensor `init` table, sensor mode table (`res4`), titan_up (S1-S6), SMMU (S7), CSID reset (S8), IFE-lite reset/WM (S9-S10),
CSID config (S11, `lane_cfg` 0x210, C-PHY, VC 0), **CSIPHY up (3.5)**, then sensor `on`, poll (5), sensor `off`, teardown.
Optional intermediate: the sensor's own colour bars (`tpgon`: 0x0600 = 0, 0x0601 = 1; `tpgoff`: 0x0600 = 0) through the whole chain, to separate "PHY/CSID wrong" from "sensor tables wrong".

## 5. Frame completion, memory, unpacking

### 5.1 Detection (polling; no GIC interrupt)
Android's SPIs (csid-lite0 0x1d4, ife-lite0 0x1d5, csiphy0 0x1dd) stay masked. Poll, in this order, and log all of them (redwood `show_paths` [R:2541-2549] is the template):
- CSID C+0x20 RX_IRQ_STATUS (errors, see S11), C+0x160 packets received (counts), C+0x164 ECC, C+0x168 CRC, C+0x30 RDI0_IRQ_STATUS (bit12 SOF, bit9 EOF, bit2 FIFO overflow, bit15 CCIF violation, bit13/14 pixel/line count error), C+0x250 RDI0_STATUS, C+0x2e0/0x2e4 byte counters.
- IFE-lite I+0x1204 camif-lite RDI0 HW_STATUS, I+0x44 IRQ_STATUS1 (RDI0 SOF bit0, EOF bit1 [vfe48x_camif_rdi_reg_data[0]]), I+0x40 IRQ_STATUS0 (bit0 RDI0 error, bit4 bus, bit17 reset done), I+0x48 IRQ_STATUS2 (bit8 RDI0 error),
  **I+0x1a28 BUS_IRQ_STATUS0: bit4 = RDI0 buffer done (composite group 0)**, bit0 = register-update done (RUP), bits 30/31 bus errors; I+0x1a64 CCIF_VIOLATION_STATUS, I+0x1a68 OVERFLOW_STATUS, I+0x1a70 IMAGE_SIZE_VIOLATION;
  WM0 I+0x1c68..0x1c74 ADDR_STATUS_0..3, I+0x1c7c/0x1c80 DEBUG_STATUS_0/1.
- Frame done = I+0x1a28 bit 4. Then clear: I+0x1a20 = status, I+0x1a30 = 1 (BUS_IRQ_GLOBAL_CLEAR); top I+0x34 = status0, I+0x24 = 1. For a second frame write a new IMAGE_ADDR and I+0x20 = 1 before the next SOF [R:3199-3208].
- Redwood waits 100 ms steps for the first frame (tpg) and 2 ms steps up to 5 s (sensor) [R:2629-2636, 3193-3213]. Same here.

### 5.2 Where the frame is and its layout
The WM writes from IMAGE_ADDR on, contiguous in **frame mode**, `frame_incr` apart for successive frames. The CSID RDI path passes RAW10 as received (MIPI packed: **5 bytes per 4 pixels**), the packer is PLAIN_128 (16-byte bus words),
so a line is `width*10/8` bytes; **the line pitch to expect is ALIGN16(width*10/8)** (redwood measured 5008 for a 4000-pixel line on its ver2 bus [R:2339-2343]; for ver3 the stride register decides, **[inf]**: R3).
Mode 4: line 2500 B, pitch 2512, frame 2833536 B (692 pages); payload `width*10/8*height` = 2820000 B = the CSID's byte counter.
The buffer must be flushed/invalidated by the CPU before reading (`dc civac`, `dsb sy`, as [R:2641-2650]).

### 5.3 Buffer address
`IMAGE_ADDR` (I+0x1c04) is **32 bits**. Two options, decide by the S0 SMMU reads:
- **S7a bypass** if sCR0 bit 10 (USFCFG) is 0 (an unmatched stream bypasses; redwood prints this: `show_smmu` [R:1658-1689], scr0 bit 10) and no SMR matches the IFE streams: IMAGE_ADDR = physical address, buffer below 4 GB (DRAM starts at 0x80000000 here) [inf].
- **S7b context** as redwood `smmu_up()` [R:2175-2243]: free SMR/S2CR match register + free context bank (never touch a context that a valid SMR uses: CB4/5 are the display's), 3-level 39-bit tables of ours (TCR 0x1b | ...), then
  S2CR(n) = ctx, SMR(n) = 1<<31 | **0x441<<16 | 0x800** (kona: eight ids, [DT:29287]; redwood 0x4e0) and IMAGE_ADDR = an IOVA inside the IFE pool (0x7400000..0xdc000000, [DT:29287]; redwood uses 0x40000000). Global SMMU writes go through the hypervisor and were accepted for audio/GPU/Wi-Fi on this phone (hardware-qualcomm.md section 3).
After the run read the context's FSR/FAR and the global SGFSR (redwood `smmu_faults()`): "the SMMU has refused the IFE nothing" is the log line to look for.
Which of the eight stream ids the IFE-lite WM uses is not in the DT [inf]: the mask covers all.

### 5.4 Unpacking MIPI RAW10
For each group of 5 bytes b0..b4: p0 = b0<<2 | (b4 & 3), p1 = b1<<2 | (b4>>2 & 3), p2 = b2<<2 | (b4>>4 & 3), p3 = b3<<2 | (b4>>6 & 3). Bayer order: the module's word 0 of the resolution element is 0 for every mode [mod];
tools/camsensor.py's key says 0 = BGGR (CamX order) [inf for IMX689]; confirm from the first picture. tools/camraw.py reads the redwood header `RWCAM001` (`struct cam_header` [R:2057-2068]); reuse it with `bayer`, `stride` 2512, `format` 1.

## 6. IMX689 mode and sensor writes [mod]
Module file `op8pro-data/cam-extract/o/lib64/camera/com.qti.sensormodule.semco_imx689.bin` (slave 0x34 8-bit = **0x1a** 7-bit, 16-bit register address, 8-bit data, id 0x0689 at 0x0016; no delays, no polls in any table;
power sequence VANA, CUSTOM_REG1, VDIG, VIO, MCLK 19.2 MHz, RESET, each followed by 1 ms [tools/camsensor.py output]). Decoded resolution elements (408 bytes; fields from my script: output pixel clock = word 3, line length word 4, frame length word 5,
bin factor word 9, fps = double at word 11, "output rate" word 8, stream blocks via word 2):

| mode | main stream (VC0 DT 0x2b) | second stream | bin | fps | line x frame | vt clock | rate word 8 | writes |
|---|---|---|---|---|---|---|---|---|
| 0 | 8000x6000 | VC1 DT 0x30 4000x3000 | 1 | 30 | 11664 x 6090 | 2131.2 MHz | 2192.09 M | 106 |
| 1, 6 | 4000x3000 | VC1 DT 0x30 2000x1500 | 2x2 | 30 | 11856 x 3094 / 3104 | 1100.8 MHz | 579.29 M | 106 |
| 2 | 4000x3000 | + 20032x1 DT 0x31, VC1 4000x750 | 2x2 | 30 | 11664 x 6235 | 2182.4 MHz | 1112.5 M | 322 |
| 3 | 4000x3000 | VC1 2000x1500 | 2x2 | 60 | 11856 x 3066 | 2182.4 MHz | 1112.5 M | 106 |
| **4** | **2000x1128** (crop y 186, **4x4 bin**) | **none** | 4x4 | **240.11** | **3872 x 1184** | 1100.8 MHz | 757.03 M | 106 |
| 5 | 1296x736 (crop 352,382) | none | 4x4 | 481 | 3872 x 780 | 1452.8 MHz | 735.63 M | 106 |
| 7 | 4608x3456 | VC1 2304x1728 | 1x1 | 29.8 | 11664 x 3571 | 1241.6 MHz | 717.53 M | 106 |
| 8 | 4000x3000 | VC1 2000x1500 | 2x2 | 30 | 14096 x 4116 | 1740.8 MHz | 1184.91 M | 106 |

**Recommended: mode 4**, table `res4`: 2000x1128, RAW10, CSI data type 0x2b, one stream, 3 trios, C-PHY. Registers visible in its table: 0x0112/0x0113 = 0x0a0a (RAW10), 0x0114 = 0x02 (3 trios), 0x0340/41 = 0x04a0 (frame length 1184),
0x0342/43 = 0x0f20 (line length 3872), 0x034c/4d = 0x07d0 (2000), 0x034e/4f = 0x0468 (1128), 0x0202/03 = 0x0460 (coarse integration 1120 lines), 0x0204/05 = 0 (gain 1x), window 0x344..0x34b = x 0..0x1f3f, y 0x2e0..0x147f (the stream block says offset 0,186).
Mipi rate: word 8 x 10 / 3 = 2.52 Gbit/s per trio (CamX-style estimate [inf]; about 1.1 Gsym/s at 2.28 bit/sym). 240 fps is demanding (about 680 MB/s into DRAM): if the chain misbehaves, raise the frame length
(0x0340/41) to slow it, or use **mode 1 (4000x3000, 30 fps)** as the conservative fallback: pitch 5008, frame 15024000 B (3668 pages), PDAF stream on VC1 DT 0x30 is simply not mapped by CSID RDI0 (it only raises no "unmapped" interrupt: bit 22 is not in Android's mask).

**Sequence written to the sensor (all `addr16, data8`, 7-bit address 0x1a, in this order):** `init` (3202 writes, first 0x0136=0x13, 0x0137=0x33 (19.2 MHz), 0x33f0 = 6, 0x33f1 = 4, 0x0111 = 3 ...) -> `res4` (106 writes) -> (optional exposure: 0x0104 = 1,
0x0202/03 coarse, 0x0204/05 gain, 0x0104 = 0 [holdon/holdoff tables]) -> (CSID/CSIPHY armed) -> **stream on: 0x0100 = 1** -> frames -> **stream off: 0x0100 = 0**. About 3300 transfers at the bit-banged 100 kHz is on the order of one second [estimate].
The tables are extracted by `tools/camsensor.py`, which since 2026-10-27 also decodes the modes and writes `res0..res8` for the V2 files (see 6b). Run-time sequences as the tables give them: stream on `0x0100=1`, off `0x0100=0`, group hold on `0x0104=1`, off `0x0104=0`; there is no exposure/gain table: the mode table itself sets 0x0202/03 (res4: 0x0460), 0x0204/05 (0) and 0x0340/41; for a change write 0x0104=1, 0x0202/03, 0x0204/05 (and 0x0340/41), 0x0104=0 [inf: Sony convention; the module's symbols give short-exposure 0x0224, short gain 0x0216, digital gain 0x020e, min line count 8, max analog gain 64.0, step 4 for res4].

## 6b. Tab format used by the kernel loader (`RWCTAB02`, little endian) [R: hk_camera.c `ctab_load`/`ctab_block`/`ctab_find`, structs at 1076-1087; tools/camsensor.py]
Made by `python3 -I tools/camsensor.py MODULE.bin OUT.tab` (V2 files since 2026-10-27: modes and `res0..` tables included; `-` as OUT lists only).
| Offset | Content |
|---|---|
| 0x00 | magic `RWCTAB02` (8 bytes) |
| 0x08 | 8 x u32: slave (8-bit, 0x34 -> 7-bit 0x1a = slave>>1), address bytes and data bytes of the ID read (2, 2), ID address (0x16), ID (0x689), ID mask (0xffff), lane assignment (0x210), number of modes (9; loader wants 1..16) |
| 0x28 | modes, 48 bytes each = 12 x u32: width, height, x, y, lanes (trios if C-PHY), cphy (1), settle (**0**: not in V2 files, use `cam_settle`), pixel_clock (the module's output-rate word, bit/s per lane = rate*10/lanes in hk_camera.c), line_length, frame_length, fps x 1000, data_type (0x2b) |
| after modes | two power sequences `pwrup`, `pwrdn`: name[8], count u32, then count x 12 bytes {kind u16 (0 MCLK, 1 VANA, 2 VDIG, 3 VIO, 6 CUSTOM_REG1, 8 RESET), value u16 (MCLK in kHz), then 4 zero bytes, delay_us u32} (same 12-byte `struct ctab_entry` as a register entry) |
| then | table count u32, then each table: name[8] (NUL padded), count u32, count x 12 bytes {address u16, data u16, address_bytes u8 (2), data_bytes u8 (1), operation u8 (0 = write), 0 u8, delay_us u32} |
IMX689 file: `/tmp/camfw/camera-imx689.tab`, 53384 bytes, 16 tables in this order: res0..res8, on, off, holdon, holdoff, init (3202 writes), tpgoff, tpgon. Look-up is by name (`res%u` for the mode index). No table has delays or polls; all entries are (2, 1, 0).
The loader's `sensor_table()` [R:2677-2715] writes entries as address16 + data8 over I2C, and requires operation 0.

## 7. Riskiest assumptions and read-only checks
Reads only; a read is safe only after its block's clock (3.1 S6). Never write a register that is not listed above.

| # | Assumption | Check (read) | If wrong |
|---|---|---|---|
| R1 | The sensor really emits 3-trio C-PHY (module `laneAssign` 0x0210, 0x0111=3) | after sensor `init`+`res4`, read sensor 0x0111 (expect 3), 0x0114 (expect 2); CSIPHY IRQ status 0x8b0.. and CSID RX_IRQ bits 11-13 | stop; D-PHY table in 3.6 (and lane_type 0, 4 lanes: sensor 0x0111=2/0x0114=3 are not in the module: do not invent) |
| R2 | CSIPHY settle count (V2 file has none) | CSID C+0x168 CRC, C+0x164 ECC counters, RX_IRQ bits 12/13 (CPHY SOT / PH_CRC), C+0x160 packet count after streaming; sweep 0x07..0x30 | the only parameter that is a sweep |
| R3 | WM stride / line pitch for frame mode on ver3 (ALIGN16(width*10/8)) | rung A with a width whose line is not a multiple of 16; compare memory against CSID byte counter (C+0x2e0) and WM ADDR_STATUS (I+0x1c68-74) | adjust IMAGE_CFG_2; try 0xffff (Android's initial `rsrc_data->stride`) |
| R4 | CSID-lite 0 can take PHY 0 | rung B: C+0x160 stays 0 although the PHY sees HS (CSIPHY status) | try `phy_sel` other values; or full CSID0/IFE0 |
| R5 | CAMNOC power-on defaults let WM writes through | read camnoc 0xac42000+0x1430/1434/1438/1440/1448 (3.3); WM ADDR_STATUS stuck, BUS_IRQ_STATUS0 bit4 never sets while CSID counts bytes and I+0x1a68 OVERFLOW_STATUS is set | write Android's five values (3.3) |
| R6 | no MNOC/DDR bandwidth vote needed (redwood worked without) | same symptom as R5 plus `bus` overflow; read I+0x1a68 | `instantnoodle_bcm_vote` on the camera's mnoc BCM (name unknown) |
| R7 | SMMU: bypass or stream mask 0x441 | S0: sCR0 bit10, IDR0/IDR1, the SMR/S2CR table (global registers only, never a context bank that a valid SMR owns); after run: global SGFSR/SGFSYNR1 | use S7b; if the hypervisor refuses an SMR write, log and stop |
| R8 | PLL0 free and MX/MMCX levels enough | camcc 0x0 MODE (bit0/bit31) before; PLL0 lock bit; RCG CMD bit0 clears; every CBCR bit31 clear | an RCG that does not update = rail too low: raise `mx.lvl` / `mmcx.lvl` |
| R9 | Block versions: CPAS 4.8.0, lite blocks | S6 reads: 0xac40000 (expect 0x00040800), IFE-lite +0x0/+0x4/+0x8, CSID-lite +0x0 | block not as modelled: stop |
| R10 | refgen is off at idle even on Android (idle log), ldoa9 is held by others | refgen 0x088e7080 bit0 before and after the write, RPMh vote result of `ldoa9` | PHY inert; vote/write as 2.2 |
| R11 | RAW10 bytes pass the RDI untouched (decode 0xf, packer PLAIN_128) | CSID byte counter = width*10/8*height; first bytes of the frame against the TPG colour bars | check CFG0 `fmt_shift` bits |

Hardware-safety notes: do not read CSID/IFE/PHY registers before their clocks; do not read SMMU context banks that are not ours; the GCC camera AHB clock and MMCX vote stay on at the end ([I:673-678]); one hardware run at a time (`./xl wait`).
