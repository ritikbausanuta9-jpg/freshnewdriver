#!/bin/sh
# pack_ko.sh — Pack neko_drv.ko into a self-extracting 6.1.ko.sh
# Usage: sh pack_ko.sh <input.ko> <output.sh>
# Compatible with Android toybox sh/busybox sh
set -e

KO="$1"
OUT="$2"

if [ -z "$KO" ] || [ -z "$OUT" ]; then
    echo "Usage: $0 <neko_drv.ko> <6.1.ko.sh>"
    exit 1
fi

if [ ! -f "$KO" ]; then
    echo "Error: $KO not found"
    exit 1
fi

B64=$(base64 "$KO" | tr -d '\n')

# ── Write shell header ──────────────────────────────────────────────────
printf '#!/bin/sh\n'                                          > "$OUT"
printf '# neko_drv v2 — stealth shm driver for Android GKI 6.1.x\n' >> "$OUT"
printf '# Usage: sh ./6.1.ko.sh\n'                           >> "$OUT"
printf '# Auto-generated — do not edit\n'                    >> "$OUT"
printf 'set -e\n'                                             >> "$OUT"
printf 'mkdir -p /data/local/tmp\n'                          >> "$OUT"

# mktemp: Android toybox does NOT support suffix after XXX
# so we create temp file then rename to .ko
printf 'TMP=$(mktemp /data/local/tmp/nekodrvXXXXXX)\n'      >> "$OUT"
printf 'TMPKO="${TMP}.ko"\n'                                  >> "$OUT"
printf 'mv "$TMP" "$TMPKO"\n'                                 >> "$OUT"
printf 'TMP="$TMPKO"\n'                                       >> "$OUT"
printf 'trap "rm -f \\"$TMP\\"" EXIT INT TERM\n'            >> "$OUT"

# base64 decode the embedded .ko
printf 'base64 -d << '"'"'__NEKOB64__'"'"' > "$TMP"\n'      >> "$OUT"

# ── Write base64 blob (76 chars per line for maximum compat) ───────────
echo "$B64" | fold -w 76                                     >> "$OUT"
printf '__NEKOB64__\n'                                        >> "$OUT"

# ── Write loader ────────────────────────────────────────────────────────
printf '\n'                                                   >> "$OUT"
printf '# Kernel version check\n'                            >> "$OUT"
printf '_kv() { uname -r | cut -d. -f1,2; }\n'             >> "$OUT"
printf '_ver=$(_kv)\n'                                        >> "$OUT"
printf 'case "$_ver" in\n'                                    >> "$OUT"
printf '  6.1) : ;;\n'                                        >> "$OUT"
printf '  *) printf "[!] Warning: kernel %s (built for 6.1.x)\\n" "$_ver" ;;\n' >> "$OUT"
printf 'esac\n'                                               >> "$OUT"
printf '\n'                                                   >> "$OUT"

# unload old instance silently
printf '# Unload previous instance if present\n'             >> "$OUT"
printf 'rmmod neko_drv 2>/dev/null || true\n'               >> "$OUT"
printf 'sleep 0.3\n'                                          >> "$OUT"
printf '\n'                                                   >> "$OUT"

# insmod
printf '# Load driver\n'                                      >> "$OUT"
printf 'if insmod "$TMP"; then\n'                             >> "$OUT"
printf '    printf "[+] neko_drv loaded successfully\\n"\n'  >> "$OUT"
printf '    printf "[+] Device: /dev/neko\\n"\n'             >> "$OUT"
printf '    printf "[+] Mode: shared-memory zero-ioctl\\n"\n' >> "$OUT"
printf 'else\n'                                               >> "$OUT"
printf '    printf "[-] insmod failed\\n"\n'                  >> "$OUT"
printf '    printf "    Ensure: KernelSU/Magisk with GKI module signing disabled\\n"\n' >> "$OUT"
printf '    exit 1\n'                                         >> "$OUT"
printf 'fi\n'                                                 >> "$OUT"

chmod +x "$OUT"

SIZE=$(wc -c < "$OUT")
echo "[+] Packed: $OUT"
echo "    Size: ${SIZE} bytes"
if command -v sha256sum >/dev/null 2>&1; then
    echo "    SHA256: $(sha256sum "$OUT" | cut -d' ' -f1)"
fi
