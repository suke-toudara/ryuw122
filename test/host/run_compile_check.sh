#!/usr/bin/env bash
# Compile the driver and the application against the stub ESP-IDF headers in
# test/host/stubs. This catches syntax, type and format-string errors without a
# full ESP-IDF installation - it is a smoke check, not a replacement for
# `idf.py build`.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
stubs="$root/test/host/stubs"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

cc="${CC:-gcc}"
flags=(-std=c11 -Wall -Wextra -Wformat=2 -Werror -c
       -I"$stubs" -I"$stubs/freertos" -I"$root/components/ryuw122/include")

echo "== driver"
"$cc" "${flags[@]}" "$root/components/ryuw122/ryuw122.c" -o "$out/ryuw122.o"
"$cc" "${flags[@]}" "$root/components/ryuw122/ryuw122_parse.c" -o "$out/parse.o"

echo "== application (ANCHOR role)"
"$cc" "${flags[@]}" -I"$stubs/anchor" "$root/main/app_main.c" -o "$out/anchor.o"

echo "== application (TAG role)"
"$cc" "${flags[@]}" -I"$stubs/tag" "$root/main/app_main.c" -o "$out/tag.o"

echo "compile check passed"
