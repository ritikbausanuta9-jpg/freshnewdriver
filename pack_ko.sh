#!/bin/sh
# pack_ko.sh — Pack neko_drv.ko into a self-extracting 6.1.ko.sh
# Matches the original 6.1.ko.sh format exactly:
#   - __EMBEDDED_KO_B64__ marker
#   - Color output
#   - dmesg tail check
# Usage: sh pack_ko.sh <input.ko> <output.sh>
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

# ── header ────────────────────────────────────────────────────────────────
printf '#!/bin/sh\n'                                                    > "$OUT"
printf '# neko_drv — stealth shm memory driver GKI 6.1.x\n'          >> "$OUT"
printf '# Usage: sh ./6.1.ko.sh\n'                                    >> "$OUT"
printf 'set -e\n'                                                      >> "$OUT"
printf 'mkdir -p /data/local/tmp\n'                                    >> "$OUT"
printf 'TMP=$(mktemp /data/local/tmp/nekodrvXXXXXX)\n'                >> "$OUT"
printf 'TMPKO="${TMP}.ko"\n'                                           >> "$OUT"
printf 'mv "$TMP" "$TMPKO"\n'                                          >> "$OUT"
printf 'TMP="$TMPKO"\n'                                                >> "$OUT"
printf 'trap "rm -f \"$TMP\"" EXIT INT TERM\n'                        >> "$OUT"
printf 'base64 -d << '"'"'__EMBEDDED_KO_B64__'"'"' > "$TMP"\n'       >> "$OUT"

# ── base64 blob ──────────────────────────────────────────────────────────
echo "$B64" | fold -w 76                                              >> "$OUT"
printf '__EMBEDDED_KO_B64__\n'                                        >> "$OUT"

# ── loader (matches original 6.1.ko.sh style) ───────────────────────────
printf 'COLOR_GREEN=$(printf '"'"'\\033[0;32m'"'"')\n'                >> "$OUT"
printf 'COLOR_RED=$(printf '"'"'\\033[0;31m'"'"')\n'                  >> "$OUT"
printf 'RESET_SEQ=$(printf '"'"'\\033[0m'"'"')\n'                     >> "$OUT"
printf 'set +e\n'                                                      >> "$OUT"
printf 'rmmod neko_drv 2>/dev/null || true\n'                         >> "$OUT"
printf 'sleep 0.3\n'                                                   >> "$OUT"
printf 'ERROR_MSG=$(insmod "$TMP" 2>&1)\n'                            >> "$OUT"
printf 'INSMOD_STATUS=$?\n'                                           >> "$OUT"
printf 'set -e\n'                                                      >> "$OUT"
printf 'if [ $INSMOD_STATUS -eq 0 ]; then\n'                         >> "$OUT"
printf '    echo "${COLOR_GREEN}[+] neko_drv loaded — /dev/neko ready${RESET_SEQ}"\n' >> "$OUT"
printf '    dmesg | tail -30 | grep -i neko || true\n'               >> "$OUT"
printf 'else\n'                                                        >> "$OUT"
printf '    echo "${COLOR_RED}[-] insmod failed${RESET_SEQ}"\n'       >> "$OUT"
printf '    echo "$ERROR_MSG"\n'                                      >> "$OUT"
printf '    dmesg | tail -30 | grep -i neko || true\n'               >> "$OUT"
printf '    exit 1\n'                                                  >> "$OUT"
printf 'fi\n'                                                          >> "$OUT"

chmod +x "$OUT"

KO_SIZE=$(wc -c < "$KO")
SH_SIZE=$(wc -c < "$OUT")
echo "[+] neko_drv.ko  : ${KO_SIZE} bytes"
echo "[+] 6.1.ko.sh    : ${SH_SIZE} bytes"
echo "[+] SHA256        : $(sha256sum "$OUT")"
