#!/bin/bash
cd /ws; export THEOS=/opt/theos
printf '#!/bin/sh\necho UTC\n' > /usr/local/bin/timedatectl; chmod +x /usr/local/bin/timedatectl
git config --global --add safe.directory '*' 2>/dev/null
rsync -a /host/userland/ /ws/userland/
rsync -a /host/tools/ /ws/tools/
mkdir -p /ws/fw-gpu && rsync -a /host/fw-gpu/ /ws/fw-gpu/
. ./xnu-env.sh
GL=1 REST=1 userland/build.sh
ls -l userland/root.apfs userland/system.apfs
