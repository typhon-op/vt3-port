#!/usr/bin/env bash
#
# Builds and runs every host test in the port: the parts of the runtime that can be checked without
# a phone. Each test is compiled into build/hosttests/ with -Wall -Wextra, then run; the summary is
# one line per test and the exit status is 0 only when every test built and passed.
#
#   tools/hosttests.sh        # summary
#   tools/hosttests.sh -v     # also print each test's full output
#
# build/hosttests/<name>.log holds the output of each test (and .build.log a failed compile).
set -uo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cc=${CC:-cc}
out="$root/build/hosttests"
verbose=0
[ "${1:-}" = "-v" ] && verbose=1

names=()
exes=()
build_failed=0

# build <name> <sources...> -- <link flags...>
build() {
    local name=$1
    shift
    local srcs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do
        srcs+=("$1")
        shift
    done
    [ "${1:-}" = "--" ] && shift

    local exe="$out/$name" log="$out/$name.build.log"
    if ! "$cc" -O1 -Wall -Wextra -o "$exe" "${srcs[@]}" -I"$root/runtime/include" "$@" >"$log" 2>&1; then
        printf '  BUILD FAILED  %s\n' "$name"
        sed 's/^/                /' "$log"
        build_failed=1
        return 1
    fi
    names+=("$name")
    exes+=("$exe")
}

mkdir -p "$out"

build touch          tools/touchtest/TouchHostTest.c runtime/src/ui/touch_core.c \
                     runtime/src/ui/touch_config.c -- -lm
build touch-menu     tools/touchtest/TouchMenuHostTest.c runtime/src/ui/touch_menu_layout.c -- -lm
build quick-menu     tools/touchtest/MenuHostTest.c runtime/src/ui/menu_layout.c -- -lm
build thermal        tools/touchtest/ThermalHostTest.c runtime/src/core/thermal.c --
build lifecycle      tools/touchtest/LifecycleHostTest.c runtime/src/core/guest.c \
                     runtime/src/core/log.c --
build audio          tools/audiotest/AudioHostTest.c runtime/src/audio/mix_core.c \
                     runtime/src/audio/ringbuf.c -- -lm -lpthread
build streamsim      tools/audiotest/StreamSim.c runtime/src/audio/mix_core.c -- -lm

printf '\nhost tests (%s)\n' "$cc"
printf '  %s\n' "--------------------------------------------------------------------------------"

fail=0
passed=0
for i in "${!names[@]}"; do
    name=${names[$i]}
    exe=${exes[$i]}
    log="$out/$name.log"

    "$exe" >"$log" 2>&1
    status=$?
    summary=$(grep -E '^[0-9]+ checks' "$log" | tail -1)
    [ -z "$summary" ] && summary="(no summary line; see $log)"

    if [ $status -eq 0 ]; then
        printf '  pass  %-11s %s\n' "$name" "$summary"
        passed=$((passed + 1))
    else
        printf '  FAIL  %-11s %s\n' "$name" "$summary"
        grep -E '^FAIL' "$log" | head -20 | sed 's/^/                /'
        fail=1
    fi
    if [ $verbose -eq 1 ]; then
        sed 's/^/                /' "$log"
    fi
done

printf '  %s\n' "--------------------------------------------------------------------------------"
if [ $build_failed -ne 0 ] || [ $fail -ne 0 ]; then
    printf 'FAILED: %d of %d test(s) passed (logs in %s)\n' \
        "$passed" "${#names[@]}" "${out#"$root"/}"
    exit 1
fi
printf 'OK: all %d host test(s) passed\n' "$passed"
exit 0
