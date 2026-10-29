# Linux builder for the OnePlus 8 Pro UEFI (Mu-Silicium / edk2) and for UEFIReader (.NET). arm64 on Apple silicon.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
      git python3 python3-pip python3-venv python3-dev build-essential lld clang llvm uuid-dev nasm \
      gcc-aarch64-linux-gnu gettext locales gnupg ca-certificates curl xz-utils unzip make \
      dotnet-sdk-8.0 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
