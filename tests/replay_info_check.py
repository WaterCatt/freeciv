#!/usr/bin/env python3

import gzip
import io
import struct
import sys


PLAYERS_TEXT_LEN = 512
INFO_TEXT_LEN = 128
OPTIONAL_INFO_SIZE = 4 + 4 + PLAYERS_TEXT_LEN + INFO_TEXT_LEN + INFO_TEXT_LEN


def read_u16(buf):
    data = buf.read(2)
    if len(data) != 2:
        raise ValueError("unexpected EOF reading u16")
    return struct.unpack("<H", data)[0]


def read_u32(buf):
    data = buf.read(4)
    if len(data) != 4:
        raise ValueError("unexpected EOF reading u32")
    return struct.unpack("<I", data)[0]


def read_u64(buf):
    data = buf.read(8)
    if len(data) != 8:
        raise ValueError("unexpected EOF reading u64")
    return struct.unpack("<Q", data)[0]


def read_string(buf):
    slen = read_u16(buf)
    data = buf.read(slen)
    if len(data) != slen:
        raise ValueError("unexpected EOF reading string")
    return data.decode("utf-8", errors="replace")


def read_fixed_string(buf, size):
    data = buf.read(size)
    if len(data) != size:
        raise ValueError("unexpected EOF reading fixed string")
    return data.split(b"\0", 1)[0].decode("utf-8", errors="replace")


def open_replay(path):
    with open(path, "rb") as f:
        magic = f.read(2)
    if magic == b"\x1f\x8b":
        return gzip.open(path, "rb")
    return open(path, "rb")


def main(path):
    with open_replay(path) as f:
        buf = io.BytesIO(f.read())

    if buf.read(8) != b"FCREPLAY":
        raise ValueError("missing replay magic")

    version = read_u16(buf)
    flags = read_u16(buf)
    chunk = buf.read(4)
    chunk_size = read_u32(buf)

    if version != 1:
        raise ValueError(f"unexpected replay version: {version}")
    if chunk != b"INFO":
        raise ValueError(f"expected INFO chunk, got {chunk!r}")

    info_start = buf.tell()
    fc_version = read_string(buf)
    capability = read_string(buf)
    ruleset = read_string(buf)
    scenario = read_string(buf)
    start_turn = read_u32(buf)
    start_year = read_u32(buf)
    timestamp = read_u64(buf)

    consumed = buf.tell() - info_start
    final_turn = start_turn
    duration = 0
    players = ""
    result = ""
    winner = ""

    if chunk_size >= consumed + OPTIONAL_INFO_SIZE:
        final_turn = read_u32(buf)
        duration = read_u32(buf)
        players = read_fixed_string(buf, PLAYERS_TEXT_LEN)
        result = read_fixed_string(buf, INFO_TEXT_LEN)
        winner = read_fixed_string(buf, INFO_TEXT_LEN)

    if not fc_version:
        raise ValueError("missing version string in INFO")
    if not capability:
        raise ValueError("missing capability string in INFO")
    if not ruleset:
        raise ValueError("missing ruleset name in INFO")

    print(f"ruleset={ruleset}")
    print(f"scenario={scenario}")
    print(f"start_turn={start_turn}")
    print(f"start_year={start_year}")
    print(f"final_turn={final_turn}")
    print(f"duration={duration}")
    print(f"players={players}")
    print(f"result={result}")
    print(f"winner={winner}")
    print(f"flags={flags}")
    print(f"timestamp={timestamp}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: replay_info_check.py REPLAY_FILE", file=sys.stderr)
        sys.exit(2)

    try:
        main(sys.argv[1])
    except Exception as exc:
        print(f"replay info parse failed: {exc}", file=sys.stderr)
        sys.exit(1)
