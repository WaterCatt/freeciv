#!/bin/sh

set -eu

srcdir="$1"
builddir="$2"

tmpfile="$(mktemp /tmp/replay-test-file-XXXXXX)"
trap 'rm -f "${tmpfile}"' EXIT INT TERM

sh "${srcdir}/tests/replay_recording_toggle.sh" "${srcdir}" "${builddir}" "${tmpfile}"
replay_file="$(cat "${tmpfile}")"
sh "${srcdir}/tests/replay_loading_check.sh" "${srcdir}" "${builddir}" "${replay_file}"

echo "Replay-specific checks passed."
