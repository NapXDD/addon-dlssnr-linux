#!/usr/bin/env bash
# e2e regression test for issue #1: rapid model-settings retires used to overflow the fixed
# 8-slot graveyard (3 burials per retire, 240-frame hold), immediately releasing a texture the
# GPU was still reading -- device death. In the NGX demo that shows as a rendering freeze: the
# log stops mid-run while the process lives on.
#
# The test builds the addon with the e2e hooks (build.sh --test), runs the DLSS SDK sample app
# under Proton with DLSSNR_TEST_RETIRE_EVERY=100 (a forced Apply-style retire every 100
# evaluates -- deterministically inside the graveyard hold window at any fps), and asserts the
# pass keeps evaluating through dozens of retire/rebuild cycles.
#
# Requirements (same as run-demo.sh): the dlss-testbed checkout with GE-Proton and the demo
# prefix, plus nvngx_dlssnr.dll in the demo folder (copied from the WuWa install if missing).
set -uo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
TESTBED="$HOME/projects/dlss-testbed"
DEMO_DIR="$TESTBED/DLSS_Sample_App/bin/ngx_dlss_demo"
WUWA_DIR="$HOME/.local/share/Steam/steamapps/common/Wuthering Waves/Client/Binaries/Win64"
LOG="$DEMO_DIR/ReShade.log"

RUN_SECONDS=60
RETIRE_EVERY=100
MIN_RETIRES=20      # buggy build froze after 3
MIN_FINAL_EVAL=2000 # buggy build froze around eval 300

fail() { echo "FAIL: $*"; exit 1; }
skip() { echo "SKIP: $*"; exit 77; }

[[ -d "$DEMO_DIR" ]] || skip "demo testbed not found at $DEMO_DIR"
[[ -x "$TESTBED/run-demo.sh" ]] || skip "run-demo.sh not found"

if [[ ! -f "$DEMO_DIR/nvngx_dlssnr.dll" ]]; then
  [[ -f "$WUWA_DIR/nvngx_dlssnr.dll" ]] || skip "no nvngx_dlssnr.dll available for the demo"
  cp "$WUWA_DIR/nvngx_dlssnr.dll" "$DEMO_DIR/"
fi

echo "== building test build (hooks compiled in)"
bash "$HERE/build.sh" --test || fail "build failed"
cp "$HERE/build/dlssnr-linux.addon64" "$HERE/build/nvngx.dll_nrfwd.dll" "$DEMO_DIR/" \
  || fail "could not deploy the test build into the demo folder"

rm -f "$LOG"
echo "== running demo for ${RUN_SECONDS}s with a forced retire every $RETIRE_EVERY evaluates"
( cd "$TESTBED" && DLSSNR_TEST_RETIRE_EVERY=$RETIRE_EVERY timeout $RUN_SECONDS \
    ./run-demo.sh -d3d12 > logs/e2e-preset-crash.log 2>&1 )
ec=$?

[[ -f "$LOG" ]] || fail "no ReShade.log produced -- demo did not start"
grep -q "TEST MODE" "$LOG" || fail "test hook did not arm (TEST MODE line missing) -- not a --test build?"
grep -q "giving up" "$LOG" && fail "the NR pass gave up: $(grep 'giving up' "$LOG")"

retires=$(grep -c "retiring NR feature" "$LOG")
final_eval=$(grep -oE "EvaluateFeature #[0-9]+" "$LOG" | grep -oE "[0-9]+" | sort -n | tail -1)
final_eval=${final_eval:-0}

echo "   exit=$ec (124 = ran to timeout), retires=$retires, final NR evaluate=#$final_eval"

[[ "$ec" -eq 124 ]] || fail "demo exited early (exit $ec) -- process died mid-run"
[[ "$retires" -ge "$MIN_RETIRES" ]] || fail "only $retires retire cycles (need >= $MIN_RETIRES) -- rendering froze?"
[[ "$final_eval" -ge "$MIN_FINAL_EVAL" ]] || fail "evaluates stopped at #$final_eval (need >= $MIN_FINAL_EVAL)"

echo "PASS: survived $retires retire/rebuild cycles, still evaluating at #$final_eval"
