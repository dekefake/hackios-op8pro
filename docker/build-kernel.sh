#!/bin/bash
# inside hackios-kernel-builder: /ws is a Docker volume (a case-sensitive Linux filesystem), /opt/theos the theos volume
set -x
cd /ws
export THEOS=/opt/theos
git config --global --add safe.directory '*' 
xnu-deps/setup.sh --stage1
[ -d xnu-src/xnu-8796.121.2 ] || xnu/apply.sh
xnu-deps/setup.sh --stage2
touch /tmp/kstart
./xnu-build.sh -j8 -k > /ws/kernel-build.log 2>&1 || true
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
ls -l $K && echo KERNEL_OK || echo KERNEL_MISSING
tail -5 /ws/kernel-build.log
