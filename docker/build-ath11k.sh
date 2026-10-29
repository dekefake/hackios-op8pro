#!/bin/bash
cd /ws; export THEOS=/opt/theos
git config --global --add safe.directory '*' 2>/dev/null
rsync -a /host/wifi/ /ws/wifi/
rsync -a /host/tools/ /ws/tools/
. ./xnu-env.sh
tools/mk-ath11k.sh
ls -l xnu-deps/build/ath11k/out/ath11k.o
