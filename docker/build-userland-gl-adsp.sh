#!/bin/bash
cd /ws; export THEOS=/opt/theos
printf '#!/bin/sh\necho UTC\n' > /usr/local/bin/timedatectl; chmod +x /usr/local/bin/timedatectl
git config --global --add safe.directory '*' 2>/dev/null
rsync -a /host/userland/ /ws/userland/
rsync -a /host/tools/ /ws/tools/
mkdir -p /ws/fw-wifi && rsync -a /host/fw-wifi/ /ws/fw-wifi/ && rm -f /ws/fw-wifi/board-2.bin; mkdir -p /ws/fw-audio /ws/fw-haptic && rsync -a /host/fw-audio/ /ws/fw-audio/ && rsync -a /host/fw-haptic/ /ws/fw-haptic/
mkdir -p /ws/fw-gpu && rsync -a /host/fw-gpu/ /ws/fw-gpu/
mkdir -p /ws/ios165/rootfs/System/Library/Audio/Plug-Ins/HAL && rsync -a /host/ios165/rootfs/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver /ws/ios165/rootfs/System/Library/Audio/Plug-Ins/HAL/
. ./xnu-env.sh
GL=1 REST=1 ADSP=1 userland/build.sh
ls -l userland/root.apfs userland/system.apfs
