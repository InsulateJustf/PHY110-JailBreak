#!/bin/bash
# Unpack the PHY110 16.0.10.500 full OTA and rebuild a symbolized vmlinux.
# Run from the repo root: bash work/src/extract.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$ROOT/work"
Z="$WORK/ota/PHY110_16.0.10.500.zip"
OUT="$WORK/out"

export PATH="$WORK/tools/bin:$PATH"
export UV_TOOL_DIR="$WORK/tools/uv"
export UV_TOOL_BIN_DIR="$WORK/tools/bin"

mkdir -p "$OUT"
echo "=== zip ==="
ls -l "$Z"
EXPECT=8999380534
GOT=$(stat -f %z "$Z")
[ "$GOT" = "$EXPECT" ] || { echo "INCOMPLETE: $GOT != $EXPECT"; exit 1; }

echo "=== md5 (expect 991421501418658ab1ba6aff31554632) ==="
md5 -q "$Z"

echo "=== payload extract ==="
cd "$WORK/out"
unzip -o -q "$Z" payload.bin payload_properties.txt

echo "=== partitions ==="
payload_dumper --partitions boot,vendor_boot,init_boot --out "$OUT" payload.bin

echo "=== vmlinux rebuild ==="
vmlinux-to-elf "$OUT/boot.img" "$OUT/vmlinux.elf"
ls -l "$OUT/vmlinux.elf"
