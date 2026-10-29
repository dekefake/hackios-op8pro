#!/bin/bash
cd /ws; export THEOS=/opt/theos
printf '#!/bin/sh\necho UTC\n' > /usr/local/bin/timedatectl; chmod +x /usr/local/bin/timedatectl
[ -e /usr/share/zoneinfo/UTC ] || { apt-get update -qq; apt-get install -y -qq tzdata; }
git config --global --add safe.directory '*'
. ./xnu-env.sh
echo "ld64: $(command -v ld64)"
REST=1 userland/build.sh
ls -l userland/*.apfs
