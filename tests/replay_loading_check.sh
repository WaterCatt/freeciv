#!/bin/sh

set -eu

srcdir="$1"
builddir="$2"
replay_file="$3"

client="${builddir}/freeciv-qt"
info_py="${srcdir}/tests/replay_info_check.py"

python3 "${info_py}" "${replay_file}" > /tmp/replay_info_new.txt

grep '^ruleset=' /tmp/replay_info_new.txt > /dev/null
grep '^start_turn=' /tmp/replay_info_new.txt > /dev/null
grep '^final_turn=' /tmp/replay_info_new.txt > /dev/null

timeout 12s sh -c 'cd "$1" && "$2" --replay "$3" --debug 3 -- -platform offscreen > "$4" 2>&1' sh "${srcdir}" "${client}" "${replay_file}" /tmp/replay_client_new.log || true
grep "Loaded replay '" /tmp/replay_client_new.log > /dev/null

plain_replay="$(mktemp /tmp/replay-plain-XXXXXX.fcreplay)"
gzip -dc "${replay_file}" > "${plain_replay}"

python3 "${info_py}" "${plain_replay}" > /tmp/replay_info_plain.txt
grep '^ruleset=' /tmp/replay_info_plain.txt > /dev/null

timeout 12s sh -c 'cd "$1" && "$2" --replay "$3" --debug 3 -- -platform offscreen > "$4" 2>&1' sh "${srcdir}" "${client}" "${plain_replay}" /tmp/replay_client_plain.log || true
grep "Loaded replay '" /tmp/replay_client_plain.log > /dev/null

rm -f "${plain_replay}"

  legacy_file="${srcdir}/replay-20260410-185052.fcreplay"
if [ -f "${legacy_file}" ]; then
  timeout 12s sh -c 'cd "$1" && "$2" --replay "$3" --debug 3 -- -platform offscreen > "$4" 2>&1' sh "${srcdir}" "${client}" "${legacy_file}" /tmp/replay_client_legacy.log || true
  grep "Loaded replay '" /tmp/replay_client_legacy.log > /dev/null
fi

echo "replay loading and metadata checks passed"
