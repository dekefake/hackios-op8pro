#!/bin/bash
# usage (in the builder container: /ws = hackios-op8pro, /stubs = this dir): mkshimtest.sh STOP_A|STOP_B OUT.img
set -e
cd /tmp
clang --target=aarch64-linux-gnu -c /stubs/shimtest.S -o shim.elf -DREQUIRES_KERNEL_HEADER=1 -DFD_BASE=0x9fc00000 -DFD_SIZE=0x300000 -DPAYLOAD_BASE=0xf0000000 -D$1
llvm-objcopy -O binary shim.elf shim.bin
python3 - "$2" <<'PY'
import gzip,sys,subprocess
shim=open('/tmp/shim.bin','rb').read()
fd=open('/ws/Mu-Silicium/Build/instantnoodlepPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd','rb').read()
pl=open('/ws/out/usb-payload.bin','rb').read()
blob=shim+fd+pl; blob+=b'\0'*(-len(blob)%16)
k=gzip.compress(blob,6,mtime=0)+open('/ws/Mu-Silicium/Resources/DTBs/instantnoodlep.dtb','rb').read()
open('/tmp/k','wb').write(k)
subprocess.check_call(['python3','/ws/Mu-Silicium/Resources/Scripts/mkbootimg.py','--kernel','/tmp/k','--ramdisk','/ws/Mu-Silicium/Resources/ramdisk','--header_version','1','--board','HACKiOS','-o',sys.argv[1]])
print(sys.argv[1],'shim',len(shim))
PY
