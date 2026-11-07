#!/bin/bash
# Linux/POSIX runner for the headless Ghidra naming pass.
#
# run_ghidra.sh beside this file is a Git-Bash/Windows script (it hardcodes
# analyzeHeadless.bat and /c/tools/ghidra). This is the same pass against a
# native Linux Ghidra, and it uses the Java export script because Ghidra 12.x
# removed Jython.
#
#   GHIDRA_HOME=/opt/ghidra_12.2_PUBLIC \
#   IMAGE=/mnt/docker/sites/shadow/IMAGE/shadow.img \
#   tools/ghidra_naming/run_ghidra_linux.sh
#
# Env:
#   GHIDRA_HOME  required - the ghidra_*_PUBLIC install
#   JAVA_HOME    optional - a JDK the install accepts (Ghidra 12.2 wants 21+;
#                25 is what this project uses)
#   IMAGE        required - flat image to import (IMAGE/shadow.img is produced
#                by tools/xbe_image.py and is exactly what this wants: 13
#                sections laid out at their VAs, base 0x00010000)
#   PROJ         optional - Ghidra project directory (default /tmp/ghidra_proj)
#   OUT          optional - export directory (default /tmp/ghidra_export)
#   DECOMPILE    optional - "decompile" to also emit decompiled.json
#   GROUP        optional - write XBR_<name>=<addr> lines for the recompiler
#                (default: off; see the note at the end)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
GHIDRA_HOME="${GHIDRA_HOME:-}"
IMAGE="${IMAGE:-}"
PROJ="${PROJ:-/tmp/ghidra_proj}"
OUT="${OUT:-/tmp/ghidra_export}"
DECOMPILE="${DECOMPILE:-nodecompile}"
PROJ_NAME="${PROJ_NAME:-shadow}"

[ -n "$GHIDRA_HOME" ] || { echo "ERROR: set GHIDRA_HOME" >&2; exit 1; }
[ -n "$IMAGE" ]       || { echo "ERROR: set IMAGE (a flat image to import)" >&2; exit 1; }
[ -x "$GHIDRA_HOME/support/analyzeHeadless" ] || {
    echo "ERROR: $GHIDRA_HOME/support/analyzeHeadless not found" >&2; exit 1; }
[ -f "$IMAGE" ] || { echo "ERROR: $IMAGE not found" >&2; exit 1; }

mkdir -p "$PROJ" "$OUT"

echo "GHIDRA_HOME : $GHIDRA_HOME"
echo "IMAGE       : $IMAGE"
echo "PROJECT     : $PROJ/$PROJ_NAME"
echo "EXPORT      : $OUT"
echo "DECOMPILE   : $DECOMPILE"
echo

"$GHIDRA_HOME/support/analyzeHeadless" "$PROJ" "$PROJ_NAME" \
    -import "$IMAGE" \
    -loader BinaryLoader \
    -loader-baseAddr 0x10000 \
    -processor "x86:LE:32:default" \
    -cspec windows \
    -overwrite \
    -scriptPath "$HERE/ghidra_scripts" \
    -preScript SetAnalysisOptions.java \
    -postScript ExportXbeNames.java "$OUT" "$DECOMPILE" 4000 30

echo
echo "Exports in $OUT:"
ls -l "$OUT" 2>/dev/null

# Next step (not run here, because it rewrites a tracked file):
#   py -3 tools/ghidra_naming/merge_names.py --apply
# That rewrites tools/disasm/output/functions.json with the recovered names and
# writes ghidra_names.json; re-run the recompiler afterwards to get the names
# into the generated C.
