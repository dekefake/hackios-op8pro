#!/bin/bash
# runs inside hackios-uefi-builder with Mu-Silicium mounted at /work
set -e
cd /work
. /work/.venv-build/bin/activate
# the README's nasm workaround: the system nasm in place of the NuGet extdep (needs no Mono)
for p in Linux-x86-64 Linux-aarch64 Linux-arm64; do
  D=Mu_Basecore/BaseTools/Bin/mu_nasm_extdep/$p
  mkdir -p $D
  ln -sf /usr/bin/nasm /usr/bin/ndisasm $D/
done
echo 'version: 20016.1.1' > Mu_Basecore/BaseTools/Bin/mu_nasm_extdep/extdep_state.yaml
python3 build_uefi.py -d "${1:-instantnoodlep}" -r RELEASE
