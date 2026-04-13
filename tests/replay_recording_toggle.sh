#!/bin/sh

set -eu

srcdir="$1"
builddir="$2"
outfile="$3"

server="${builddir}/freeciv-server"
replay_dir="${HOME}/.freeciv/replays"

mkdir -p "${replay_dir}"

latest_replay_after() {
  marker="$1"
  find "${replay_dir}" -maxdepth 1 -type f -name 'replay-*.fcreplay' -newer "${marker}" | sort | tail -n 1
}

run_server() {
  port="$1"
  setting="$2"
  logfile="$3"

  timeout 30s sh -c '
    cd "$5"
    {
      printf "set minplayers 0\n"
      printf "set aifill 2\n"
      printf "set timeout 1\n"
      printf "set replayrecording %s\n" "$1"
      printf "start\n"
      sleep 4
      printf "quit\n"
    } | "$2" -p "$3" > "$4" 2>&1
  ' sh "${setting}" "${server}" "${port}" "${logfile}" "${srcdir}"
}

disabled_marker="$(mktemp "${replay_dir}/.replay-disabled.XXXXXX")"
run_server 5570 disabled /tmp/replay_record_disabled.log
if [ -n "$(latest_replay_after "${disabled_marker}")" ]; then
  echo "recording disabled run still created replay file" >&2
  exit 1
fi
rm -f "${disabled_marker}"

enabled_marker="$(mktemp "${replay_dir}/.replay-enabled.XXXXXX")"
run_server 5571 enabled /tmp/replay_record_enabled.log
replay_file="$(latest_replay_after "${enabled_marker}")"
rm -f "${enabled_marker}"

if [ -z "${replay_file}" ]; then
  echo "recording enabled run did not create replay file" >&2
  exit 1
fi

case "${replay_file}" in
  "${replay_dir}"/*) ;;
  *)
    echo "replay file not stored in dedicated replay directory: ${replay_file}" >&2
    exit 1
    ;;
esac

gzip -t "${replay_file}"

printf '%s\n' "${replay_file}" > "${outfile}"
printf 'recorded_replay=%s\n' "${replay_file}"
