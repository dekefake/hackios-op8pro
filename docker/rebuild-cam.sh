#!/bin/bash
cd /ws; export THEOS=/opt/theos
git config --global --add safe.directory '*' 2>/dev/null
. ./xnu-env.sh
for f in bsd/sys/instantnoodle.h bsd/dev/instantnoodle_i2c.c bsd/dev/instantnoodle_camera.c bsd/dev/hk_camera.c bsd/dev/hk_ufs.c bsd/dev/hk_adsp.c bsd/dev/instantnoodle_slider.c bsd/dev/instantnoodle_touch.c iokit/Kernel/MiniIOMobileFramebuffer.cpp bsd/dev/hk_adreno.c bsd/dev/hk_buttons.c; do cp /host/xnu/files/$f /ws/xnu/files/$f; cp /host/xnu/files/$f /ws/xnu-src/xnu-8796.121.2/$f; done
F=/ws/xnu-src/xnu-8796.121.2/bsd/conf/files
grep -q instantnoodle_camera.c $F || sed -i 's#^bsd/dev/instantnoodle_slider.c\tstandard#bsd/dev/instantnoodle_slider.c\tstandard\nbsd/dev/instantnoodle_camera.c\tstandard#' $F
grep -n instantnoodle_camera $F
./xnu-build.sh -j8 -k > /ws/kernel-build3.log 2>&1 || true
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
ls -l --time-style=full-iso $K; date; grep -c 'error:' /ws/kernel-build3.log; grep 'error:' /ws/kernel-build3.log | head -5
