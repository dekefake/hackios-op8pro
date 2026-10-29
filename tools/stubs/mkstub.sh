#!/bin/bash
# usage (inside the builder container, /ws = hackios-op8pro, /stubs = this dir): mkstub.sh STUB.S OUT.img [dtb]
set -e
S=$1; O=$2; DTB=${3:-/ws/Mu-Silicium/Resources/DTBs/instantnoodlep.dtb}
cd /tmp
clang --target=aarch64-linux-gnu -c /stubs/$S -o stub.elf -DFD_BASE=0x9FC00000 -DFD_SIZE=0x300000
llvm-objcopy -O binary stub.elf stub.bin
python3 - "$DTB" "$O" <<'PY'
import gzip,sys,subprocess
stub=open('/tmp/stub.bin','rb').read()
k=gzip.compress(stub,6,mtime=0)+open(sys.argv[1],'rb').read()
open('/tmp/stub.kernel','wb').write(k)
subprocess.check_call(['python3','/ws/Mu-Silicium/Resources/Scripts/mkbootimg.py','--kernel','/tmp/stub.kernel','--ramdisk','/ws/Mu-Silicium/Resources/ramdisk','--header_version','1','--board','HACKiOS','-o',sys.argv[2]])
print(sys.argv[2],len(stub),'bytes of stub')
PY
