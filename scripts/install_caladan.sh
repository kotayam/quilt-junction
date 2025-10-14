#!/bin/bash
set -xe

# Globals
SCRIPT_DIR=$(dirname $(readlink -f $0))
ROOT_DIR=${SCRIPT_DIR}/../
CALADAN_DIR=${ROOT_DIR}/lib/caladan
CALADAN_PATCHES_DIR=${ROOT_DIR}/lib/patches/caladan

# Install Linux packages
sudo -E apt install -y make cmake pkg-config libnl-3-dev libnl-route-3-dev libnuma-dev uuid-dev libssl-dev libaio-dev libcunit1-dev libclang-dev libncurses-dev meson python3-pyelftools

cd $CALADAN_DIR/../
git submodule update --init --recursive -f caladan

# Apply patches
cd $CALADAN_DIR/
git -c user.name="x" -c user.email="x" am $CALADAN_PATCHES_DIR/*

prev=$(cat "$ROOT_DIR/lib/.caladan_installed_ver" 2>&1 || true)
cur=$(cat "$CALADAN_PATCHES_DIR"/* | sha256sum)

# Install Caladan
if [ "$prev" != "$cur" ] || [ ! -f $CALADAN_DIR/deps/pcm/build/src/libpcm.a ]; then
    # Build submodules first
    make submodules

    # Clean and build main Caladan components
    make clean
    make -j $(nproc)

    # Build ksched after main components
    cd ksched
    make clean
    KDIR=/lib/modules/$(uname -r)/build make -j $(nproc)
    cd ..

    # Run machine setup if needed (modified for CI environment)
    if [ -f "./scripts/setup_machine.sh" ]; then
        # Modify any system settings needed for CI
        sudo ./scripts/setup_machine.sh || echo "Machine setup failed, but continuing..."
    fi
fi

cat $CALADAN_PATCHES_DIR/* | sha256sum > $CALADAN_DIR/../.caladan_installed_ver
