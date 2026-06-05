# bio-sim simulator image.
#
# Build the simulator ONCE, here, at image-build time. `docker run` then just
# executes the compiled binary against a config the user mounts in -- no
# recompiling per run.
#
# Layers are ordered least-changing -> most-changing so Docker's cache does
# the right thing: editing RTL or the harness does NOT reinstall Verilator.

# Fully-qualified so Podman resolves it without a registry prompt (Docker is
# fine with the short name too).
FROM docker.io/library/ubuntu:24.04

# --- toolchain layer (rarely changes -> stays cached across source edits) ---
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        verilator \
        build-essential \
        perl \
        zlib1g-dev \
        ca-certificates \
        curl \
    && rm -rf /var/lib/apt/lists/*

# --- project layer (your code; copied AFTER the toolchain) ------------------
WORKDIR /opt/bio-sim
# Copy ONLY what `make build` consumes. Configs, firmware, and output traces
# are runtime inputs mounted at /work -- keeping them OUT of the image means
# editing a config or dropping a new .bin never invalidates this layer and
# never forces a re-verilate.
COPY Makefile rtl.f ./
COPY rtl/ rtl/
COPY sim/ sim/

# --- build the simulator into the image -------------------------------------
# Produces /opt/bio-sim/build/bio_sim. Also fetches sim/json.hpp (needs
# network during build, which Docker has by default).
RUN make build

# --- runtime ----------------------------------------------------------------
# The user mounts their working dir onto /work; the config path passed to
# `docker run` is resolved relative to /work, and any trace file the config
# names is written back into /work (i.e. onto the host).
WORKDIR /work
ENTRYPOINT ["/opt/bio-sim/build/bio_sim"]
CMD ["smoke.json"]