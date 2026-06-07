# sw/Dockerfile.build - firmware build environment (NOT the sim image).
# Bundles zig (via pip) + python + a RISC-V objdump so users can produce
# .bin / .rs / .dis without installing a native toolchain.
#
#   podman build -t bio-fw-build -f sw/Dockerfile.build sw/
#   podman run --rm -v "$PWD/sw:/sw:z" bio-fw-build -Dmodule=blink -Demit-listing=true
#
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip \
        binutils-riscv64-linux-gnu \
    && pip3 install --break-system-packages ziglang==0.15.2 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /sw
ENTRYPOINT ["python3", "-m", "ziglang", "build"]
