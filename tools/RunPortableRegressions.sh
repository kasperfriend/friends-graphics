#!/usr/bin/env bash
# Build and run every portable regression suite in one command.
#
# This is the same battery the "Portable regressions" job in
# .github/workflows/build.yml runs on every commit; locally it replaces the
# per-tool g++ lines documented across tools/*.cpp, builds every suite's
# translation units IN PARALLEL (the water and underwater suites each pull
# megabytes of headers, so serial compiles dominated the job's wall time),
# and then runs the suites one at a time so their file writes never overlap.
#
# Usage:
#   tools/RunPortableRegressions.sh                # CI battery, parallel builds
#   tools/RunPortableRegressions.sh --san          # every suite under ASan+UBSan
#   tools/RunPortableRegressions.sh --only water,wind
#   tools/RunPortableRegressions.sh --jobs 4
#
# The suites and their exact flags mirror the workflow job. If a suite's line
# changes here, change it there (and in the tool's header comment) in the same
# commit - the three used to be free to drift; now this script is the one place
# CI reads.
set -u
cd "$(dirname "$0")/.."

MODE=ci
ONLY=
JOBS=$(nproc 2>/dev/null || echo 4)
while [ $# -gt 0 ]; do
    case "$1" in
        --san)  MODE=san ;;
        --only) ONLY="$2"; shift ;;
        --jobs) JOBS="$2"; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

OUT=build/portable-tests
mkdir -p "$OUT/logs"

# name | output binary | extra compile flags | sources...
# The zone/area audit is checked here too: it guards the generated table in
# docs/water-zone-profile-audit.md against the shipped profiles.
if [ -z "$ONLY" ]; then
    if ! python3 tools/GenerateWaterZoneAudit.py --check; then
        echo "FAILED: water zone profile audit" >&2
        exit 1
    fi
fi

SUITES=(
    "cloud-climate|-O2 -std=c++20 -pthread|tools/CloudClimateRegression.cpp src/Environment/EnvironmentProfile.cpp src/Environment/EnvironmentDatabase.cpp"
    "cloud-layer|-O2 -std=c++20 -pthread|tools/CloudLayerRegression.cpp"
    "wind|-O2 -std=c++20 -Wall -Wextra -Werror -pedantic|tools/WindRegression.cpp"
    "frame-clock|-O2 -std=c++20|tools/FrameClockRegression.cpp"
    "water|-O2 -std=c++20 -Wall -Wextra -Werror|tools/WaterRegression.cpp"
    "device-health|-O2 -std=c++20 -Wall -Wextra -Werror|tools/DeviceHealthRegression.cpp"
    "ambient-occlusion|-O2 -std=c++20 -Wall -Wextra -Werror -pedantic|tools/AmbientOcclusionRegression.cpp"
    "underwater|-O2 -std=c++20 -Wall -Wextra -Werror -pedantic|tools/UnderwaterRegression.cpp"
    "local-lighting|-O2 -std=c++20 -Wall -Wextra -Werror -pedantic|tools/LocalLightingRegression.cpp"
    "camera-anchoring|-O2 -std=c++20 -Wall -Wextra -Werror -pedantic|tools/CameraAnchoringRegression.cpp"
    "shader-source|-O2 -std=c++20 -Wall -Wextra -Werror|tools/ShaderSourceRegression.cpp"
    "location-status|-O2 -std=c++20 -Wall -Wextra -Werror|tools/LocationStatusRegression.cpp"
    "file-sink|-O2 -std=c++20 -Wall -Wextra -Werror -pedantic -Itools/fixtures|tools/FileSinkRegression.cpp"
    "location-tuning-cache|-O2 -std=c++20 -Wall -Wextra -Itools/fixtures|tools/LocationTuningCacheRegression.cpp tools/LocationTuningCacheOtherTu.cpp"
    # The fingerprint walks pointers into the materialised section maps, so the
    # crossing suite also runs under the sanitizers. Its absolute millisecond
    # budgets step aside there - they are about a frame, and an instrumented
    # build is not one.
    "crossing|-O2 -std=c++20 -Wall -Wextra -Werror -pedantic -Itools/fixtures|tools/LocationTuningCrossingRegression.cpp"
    "crossing-san|-std=c++20 -g -Wall -Wextra -Werror -pedantic -Itools/fixtures -fsanitize=address,undefined|tools/LocationTuningCrossingRegression.cpp"
    # The overlay geometry and the tracked-state mirror are exercised under the
    # sanitizers wherever they run, so the suite and the build agree.
    "overlay-san|-std=c++20 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -g|tools/OverlayLayoutRegression.cpp"
    "tracked-state-san|-std=c++20 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -g|tools/TrackedRenderStateRegression.cpp"
)

# In sanitizer mode every suite is rebuilt instrumented: same coverage, deeper
# checks. The O2 line's optimisation level is kept so loop-heavy mirrors do not
# take a different path through their math than the tested one.
SAN_FLAGS="-fsanitize=address,undefined -g"

wanted() {
    [ -z "$ONLY" ] && return 0
    local n="$1" item
    local IFS=','
    for item in $ONLY; do [ "$item" = "$n" ] && return 0; done
    return 1
}

compile_one() {
    local entry="$1"
    IFS='|' read -r name flags sources <<<"$entry"
    local extra=""
    if [ "$MODE" = san ]; then extra="$SAN_FLAGS"; fi
    # shellcheck disable=SC2086
    if g++ $flags $extra $sources -o "$OUT/$name" 2>"$OUT/logs/$name.build.log"; then
        return 0
    fi
    return 1
}

echo "== Building ${JOBS} at a time (mode: $MODE)"
pids=()
failed_builds=""
for entry in "${SUITES[@]}"; do
    IFS='|' read -r name _ <<<"$entry"
    wanted "$name" || continue
    compile_one "$entry" &
    pids+=("$!")
    # Throttle: wait for a slot when the batch is full.
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.2; done
done
build_status=0
for pid in "${pids[@]}"; do
    wait "$pid" || build_status=1
done
if [ "$build_status" -ne 0 ]; then
    echo "One or more suites failed to compile (see $OUT/logs/*.build.log):" >&2
    for entry in "${SUITES[@]}"; do
        IFS='|' read -r name _ <<<"$entry"
        wanted "$name" || continue
        if [ ! -x "$OUT/$name" ]; then
            echo "  $name:" >&2
            tail -5 "$OUT/logs/$name.build.log" | sed 's/^/    /' >&2
        fi
    done
    exit 1
fi

echo "== Running suites"
failures=""
for entry in "${SUITES[@]}"; do
    IFS='|' read -r name _ <<<"$entry"
    wanted "$name" || continue
    start=$(date +%s)
    if "$OUT/$name" >"$OUT/logs/$name.run.log" 2>&1; then
        echo "PASS $(printf '%-22s' "$name") $(($(date +%s) - start))s"
    else
        rc=$?
        echo "FAIL $(printf '%-22s' "$name") rc=$rc  (log: $OUT/logs/$name.run.log)"
        tail -10 "$OUT/logs/$name.run.log" | sed 's/^/    /'
        failures="$failures $name"
    fi
done

if [ -n "$failures" ]; then
    echo "FAILED suites:$failures" >&2
    exit 1
fi
echo "ALL PORTABLE REGRESSIONS PASSED"
