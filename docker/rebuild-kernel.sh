#!/bin/bash
cd /ws; export THEOS=/opt/theos
git config --global --add safe.directory '*'
rm -rf xnu-src/xnu-8796.121.2/BUILD
./xnu-build.sh -j8 -k > /ws/kernel-build.log 2>&1 || true
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
ls -l $K && echo KERNEL_OK || echo KERNEL_MISSING
