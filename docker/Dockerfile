# Build environment for the webOS WPE WebKit browser: crosstool-ng toolchain + Buildroot.
# Runs as the host user (see tools/in-docker.sh) so files in the bind mounts stay owned by them.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential gcc g++ make cmake ninja-build meson pkg-config \
      bison flex gperf help2man texinfo libtool libtool-bin autoconf automake gawk \
      libncurses-dev python3 python3-dev python3-venv unzip wget curl ca-certificates rsync bc file patch \
      perl cpio git ruby unifdef xz-utils bzip2 lzip zstd gettext \
      && rm -rf /var/lib/apt/lists/*
ARG CTNG_VERSION=1.27.0
RUN cd /tmp && wget -q https://github.com/crosstool-ng/crosstool-ng/releases/download/crosstool-ng-${CTNG_VERSION}/crosstool-ng-${CTNG_VERSION}.tar.xz \
    && tar xf crosstool-ng-${CTNG_VERSION}.tar.xz && cd crosstool-ng-${CTNG_VERSION} \
    && ./configure --prefix=/opt/ct-ng && make -j"$(nproc)" && make install && rm -rf /tmp/crosstool-ng-*
ENV PATH=/opt/ct-ng/bin:$PATH
