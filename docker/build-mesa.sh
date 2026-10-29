#!/bin/bash
cd /ws; export THEOS=/opt/theos
git config --global --add safe.directory '*' 2>/dev/null
rsync -a /host/tools/ /ws/tools/
. ./xnu-env.sh
which llvm-ar || ln -sf "$(ls /usr/bin/llvm-ar-* | head -1)" /usr/local/bin/llvm-ar
AIR=${AIR:-} DRIVERS="${DRIVERS:-softpipe,freedreno}" tools/mk-mesa.sh
ls -l xnu-deps/build/mesa/out
