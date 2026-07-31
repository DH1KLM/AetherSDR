FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Build dependencies for AetherSDR CI
# Must include everything CMakeLists.txt can find_package() for
RUN apt-get update && apt-get install -y \
    cmake \
    ninja-build \
    pkg-config \
    g++ \
    git \
    patch \
    curl \
    make \
    python3 \
    qt6-base-dev \
    qt6-multimedia-dev \
    qt6-websockets-dev \
    qt6-serialport-dev \
    qt6-base-private-dev \
    qt6-shader-baker \
    qt6-shadertools-dev \
    libgl1-mesa-dev \
    libasound2-dev \
    libfftw3-dev \
    libhidapi-dev \
    qtkeychain-qt6-dev \
    portaudio19-dev \
    libpipewire-0.3-dev \
    autoconf \
    automake \
    libtool \
    libssl-dev \
    libmosquitto-dev \
    ca-certificates \
    fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

# fonts-dejavu-core is listed explicitly rather than left to fontconfig's
# Recommends, which is where the image gets it today. Nothing is broken now —
# it is pinning a dependency the suite already relies on: the face/geometry
# tests measure REAL font metrics (cross_needle_meter_test lays the SWR numbers
# out with QFontMetricsF and renders the meter face), and with no font file
# installed Qt has nothing to measure and those cases fail. Verified locally by
# pointing FONTCONFIG_FILE at an empty config. The transitive route survives
# only until someone adds --no-install-recommends to the apt line above.
