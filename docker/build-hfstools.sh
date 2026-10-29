#!/bin/bash
cd /ws; export THEOS=/opt/theos
git config --global --add safe.directory '*' 2>/dev/null
rsync -a /host/tools/ /ws/tools/
. ./xnu-env.sh
tools/mk-hfstools.sh
ls -l xnu-deps/build/hfs/newfs_hfs
