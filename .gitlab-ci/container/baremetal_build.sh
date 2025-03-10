#!/bin/bash

set -e
set -o xtrace

ROOTFS=/lava-files/rootfs-${arch}

INCLUDE_PIGLIT=1

dpkg --add-architecture $arch
apt-get update

# Cross-build test deps
BAREMETAL_EPHEMERAL=" \
        autoconf \
        automake \
        crossbuild-essential-$arch \
        git-lfs \
        libboost-dev:$arch \
        libdrm-dev:$arch \
        libegl1-mesa-dev:$arch \
        libelf-dev:$arch \
        libexpat1-dev:$arch \
        libffi-dev:$arch \
        libgbm-dev:$arch \
        libgl1-mesa-dev:$arch \
        libgles2-mesa-dev:$arch \
        libpciaccess-dev:$arch \
        libpcre3-dev:$arch \
        libpng-dev:$arch \
        libpython3-dev:$arch \
        libstdc++6:$arch \
        libtinfo-dev:$arch \
        libudev-dev:$arch \
        libvulkan-dev:$arch \
        libwaffle-dev:$arch \
        libxcb-keysyms1-dev:$arch \
        libxkbcommon-dev:$arch \
        python3-dev \
        qt5-default \
        qt5-qmake \
        qtbase5-dev:$arch \
        "

apt-get install -y --no-remove $BAREMETAL_EPHEMERAL

mkdir /var/cache/apt/archives/$arch

############### Create cross-files

. .gitlab-ci/container/create-cross-file.sh $arch

. .gitlab-ci/container/container_pre_build.sh

############### Create rootfs
KERNEL_URL=https://github.com/anholt/linux/archive/mesa-ci-2021-02-17-5.11.tar.gz

DEBIAN_ARCH=$arch . .gitlab-ci/container/lava_build.sh

############### Uninstall the build software

apt-get purge -y $BAREMETAL_EPHEMERAL

. .gitlab-ci/container/container_post_build.sh
