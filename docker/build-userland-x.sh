#!/bin/bash
cd /ws; export THEOS=/opt/theos
git config --global --add safe.directory '*' 2>/dev/null
. ./xnu-env.sh
REST=1 bash -x userland/build.sh > /ws/ubuild-x.log 2>&1; echo "exit status: $?" >> /ws/ubuild-x.log
