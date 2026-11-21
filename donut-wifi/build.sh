#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Build the MT7668 WLAN module out of tree. Run inside the lidar-build
# container (see README.md):
#   bash /work/lidar-wifi/build.sh [extra make args]
#
# KSRC   kernel source tree               (default /src/linux)
# KOBJ   prepared kernel build directory  (default /src/build/lidar)
# MOBJ   module output directory          (default /src/build/lidar-wifi-mod)
set -euo pipefail
KSRC=${KSRC:-/src/linux}
KOBJ=${KOBJ:-/src/build/lidar}
MOBJ=${MOBJ:-/src/build/lidar-wifi-mod}
here=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$MOBJ"
make -C "$KSRC" O="$KOBJ" M="$here" MO="$MOBJ" -j"$(nproc)" "${@:-modules}"
