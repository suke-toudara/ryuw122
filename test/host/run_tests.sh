#!/usr/bin/env bash
# Compile and run the host-side tests for the RYUW122 parsing layer.
# No ESP-IDF required - only a C compiler.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

cc="${CC:-gcc}"
"$cc" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$root/components/ryuw122/include" \
    "$root/components/ryuw122/ryuw122_parse.c" \
    "$root/test/host/test_ryuw122_parse.c" \
    -o "$out/test_ryuw122_parse"

"$out/test_ryuw122_parse"
