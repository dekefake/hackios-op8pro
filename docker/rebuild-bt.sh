#!/bin/bash
# the Bluetooth fork's kernel build: copy the files, make sure bsd/conf/files lists them, build
cd /ws; export THEOS=/opt/theos
git config --global --add safe.directory '*' 2>/dev/null
. ./xnu-env.sh
for f in bsd/sys/instantnoodle.h bsd/dev/instantnoodle_i2c.c bsd/dev/instantnoodle_camera.c bsd/dev/instantnoodle_bt.c bsd/dev/hk_input.c bsd/dev/hk_camera.c bsd/dev/hk_ufs.c bsd/dev/hk_adsp.c bsd/dev/instantnoodle_slider.c bsd/dev/instantnoodle_touch.c iokit/Kernel/MiniIOMobileFramebuffer.cpp bsd/dev/hk_adreno.c bsd/dev/hk_buttons.c; do cp /host/xnu/files/$f /ws/xnu/files/$f; cp /host/xnu/files/$f /ws/xnu-src/xnu-8796.121.2/$f; done
# the phone's own Bluetooth firmware (mounted at /btfw by the caller, never in a repository) as a generated header for bt=4
if [ -f /btfw/htbtfw20.tlv ] && [ -f /btfw/htnv20.bin ]; then
python3 - <<'PYEOF'
def arr(name, path):
    d = open(path, 'rb').read()
    return 'static const uint8_t %s[%d] = {\n%s\n};\n' % (name, len(d), ',\n'.join(','.join('0x%02x' % b for b in d[i:i+16]) for i in range(0, len(d), 16)))
open('/ws/xnu-src/xnu-8796.121.2/bsd/dev/instantnoodle_btfw.h', 'w').write(arr('btfw_patch', '/btfw/htbtfw20.tlv') + arr('btfw_nvm', '/btfw/htnv20.bin'))
PYEOF
else rm -f /ws/xnu-src/xnu-8796.121.2/bsd/dev/instantnoodle_btfw.h; fi
F=/ws/xnu-src/xnu-8796.121.2/bsd/conf/files
grep -q instantnoodle_camera.c $F || sed -i 's#^bsd/dev/instantnoodle_slider.c\tstandard#bsd/dev/instantnoodle_slider.c\tstandard\nbsd/dev/instantnoodle_camera.c\tstandard#' $F
grep -q instantnoodle_bt.c $F || sed -i 's#^bsd/dev/instantnoodle_camera.c\tstandard#bsd/dev/instantnoodle_camera.c\tstandard\nbsd/dev/instantnoodle_bt.c\tstandard#' $F
grep -n 'instantnoodle_camera\|instantnoodle_bt' $F
./xnu-build.sh -j8 -k > /ws/kernel-build-bt.log 2>&1 || true
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
ls -l --time-style=full-iso $K; date; grep -c 'error:' /ws/kernel-build-bt.log; grep 'error:' /ws/kernel-build-bt.log | head -5
