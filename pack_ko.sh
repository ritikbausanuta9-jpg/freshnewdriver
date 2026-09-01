#!/bin/sh
# pack_ko.sh — Pack neko_drv.ko into a self-extracting 6.1.ko.sh
# Usage: sh pack_ko.sh <input.ko> <output.sh>
set -e

KO="$1"
OUT="$2"

if [ -z "$KO" ] || [ -z "$OUT" ]; then
    echo "Usage: $0 <neko_drv.ko> <6.1.ko.sh>"
    exit 1
fi

B64=$(base64 "$KO" | tr -d '\n')

# Write header
printf '#!/bin/sh\n' > "$OUT"
printf '# neko_drv — stealth memory driver for Android GKI 6.1.x\n' >> "$OUT"
printf '# Usage: sh ./6.1.ko.sh\n' >> "$OUT"
printf 'set -e\n' >> "$OUT"
printf 'mkdir -p /data/local/tmp\n' >> "$OUT"
printf 'TMP=$(mktemp /data/local/tmp/nekodrvXXXXXX)\n' >> "$OUT"
printf 'TMPKO="${TMP}.ko"\n' >> "$OUT"
printf 'mv "$TMP" "$TMPKO"\n' >> "$OUT"
printf 'TMP="$TMPKO"\n' >> "$OUT"
printf 'trap "rm -f \\"$TMP\\"" EXIT INT TERM\n' >> "$OUT"
printf 'base64 -d << '"'"'__B64__'"'"' > "$TMP"\n' >> "$OUT"

# Write base64 blob (76 chars per line for compat)
echo "$B64" | fold -w 76 >> "$OUT"

printf '__B64__\n' >> "$OUT"

# Write loader
printf '\n' >> "$OUT"
printf '_kv() { uname -r | cut -d. -f1,2; }\n' >> "$OUT"
printf '_ver=$(_kv)\n' >> "$OUT"
printf 'case "$_ver" in\n' >> "$OUT"
printf '    6.1) : ;;\n' >> "$OUT"
printf '    *) echo "[!] Warning: kernel $_ver (tested on 6.1.x)" ;;\n' >> "$OUT"
printf 'esac\n' >> "$OUT"
printf '\n' >> "$OUT"
printf 'rmmod neko_drv 2>/dev/null || true\n' >> "$OUT"
printf 'sleep 0.2\n' >> "$OUT"
printf '\n' >> "$OUT"
printf 'if insmod "$TMP"; then\n' >> "$OUT"
printf '    echo "[+] neko_drv loaded — /dev/neko ready"\n' >> "$OUT"
printf 'else\n' >> "$OUT"
printf '    echo "[-] insmod failed. Is GKI signing enforced?"\n' >> "$OUT"
printf '    echo "    Try: magisk policy or KernelSU allow-list"\n' >> "$OUT"
printf '    exit 1\n' >> "$OUT"
printf 'fi\n' >> "$OUT"

chmod +x "$OUT"
echo "[+] Packed: $OUT ($(wc -c < "$OUT") bytes)"
echo "SHA256: $(sha256sum "$OUT")"
