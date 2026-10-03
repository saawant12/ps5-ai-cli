#!/usr/bin/env python3
"""Inspect SDK payload ELFs and optionally upload/run via Payload Manager v0.5.2."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import urllib.parse
import urllib.request
import deployments


def inspect_elf(data):
    """Screen for SDK payload structure, not proof of runtime compatibility."""
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("Expected a 64-bit little-endian ELF")
    elf_type, machine = struct.unpack_from("<HH", data, 16)
    if elf_type != 3 or machine != 62:
        raise ValueError("Expected an x86-64 position-independent SDK payload")
    entry, phoff = struct.unpack_from("<QQ", data, 24)
    phsize, phcount = struct.unpack_from("<HH", data, 54)
    if phsize != 56 or not 1 <= phcount <= 128 or phoff + phsize * phcount > len(data):
        raise ValueError("Invalid program-header table")
    loads, dynamic = [], []
    for index in range(phcount):
        kind, flags, offset, vaddr, _, filesz, memsz, align = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + index * phsize
        )
        if kind == 3:
            raise ValueError("PT_INTERP: requires an OS dynamic loader; not a PS5 SDK payload")
        if kind == 7:
            raise ValueError("PT_TLS: native ELF TLS is not supported by this port")
        if filesz > memsz or offset + filesz > len(data):
            raise ValueError("Invalid segment extent")
        if kind == 1:
            if align < 0x4000 or align & (align - 1) or (offset - vaddr) % align:
                raise ValueError("LOAD segment does not have PS5 16 KiB alignment")
            loads.append((flags, offset, vaddr, filesz))
        if kind == 2:
            if filesz % 16:
                raise ValueError("Invalid dynamic table size")
            dynamic = [struct.unpack_from("<qQ", data, p)
                       for p in range(offset, offset + filesz, 16)]
    if not any(flags & 1 and va <= entry < va + size for flags, _, va, size in loads):
        raise ValueError("Entrypoint is outside executable file-backed memory")
    if not dynamic or not any(tag == 0 for tag, _ in dynamic):
        raise ValueError("Missing terminated dynamic table")
    tags = {}
    needed_offsets = []
    for tag, value in dynamic:
        if tag == 0:
            break
        if tag == 1:
            needed_offsets.append(value)
        else:
            tags[tag] = value
    if 5 not in tags or 10 not in tags:
        raise ValueError("Missing dynamic string table")
    address, size = tags[5], tags[10]
    string_table = None
    for _, offset, va, filesz in loads:
        if va <= address and address + size <= va + filesz:
            start = offset + address - va
            string_table = data[start:start + size]
            break
    if string_table is None:
        raise ValueError("String table is outside file-backed memory")
    needed = []
    for offset in needed_offsets:
        end = string_table.find(b"\0", offset)
        if offset >= len(string_table) or end < 0:
            raise ValueError("Invalid shared-library name")
        name = string_table[offset:end].decode("ascii")
        if not re.fullmatch(r"lib[A-Za-z0-9_]+\.sprx", name):
            raise ValueError(f"Non-PS5 shared-library dependency: {name!r}")
        needed.append(name)
    if not {"libkernel_web.sprx", "libkernel_sys.sprx"}.intersection(needed):
        raise ValueError("Missing supported SDK libkernel_web.sprx or libkernel_sys.sprx dependency")
    # elfldr reads sections to determine payload size and apply relocations.
    shoff = struct.unpack_from("<Q", data, 40)[0]
    shsize, shcount = struct.unpack_from("<HH", data, 58)
    if shoff < 64 or shsize != 64 or not shcount or shoff + shsize * shcount > len(data):
        raise ValueError("Invalid section-header table required by elfldr")
    for index in range(shcount):
        section = struct.unpack_from("<IIQQQQIIQQ", data, shoff + index * shsize)
        if section[1] != 8 and section[4] + section[5] > len(data):
            raise ValueError("Section extends beyond the payload")
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "entry": hex(entry), "needed": needed, "execution_verified": False}


def manager_request(base, route, *, timeout=15, **kwargs):
    request = urllib.request.Request(base + route, **kwargs)
    # A launch is a mutation even though this API uses GET. Never retry it.
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read(1024 * 1024)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--manager", help="e.g. http://PS5_IP:8084")
    parser.add_argument("--run", action="store_true", help="Launch once after upload")
    parser.add_argument("--keep-old", action="store_true", help="Keep older uploads recorded by this workbench")
    parser.add_argument("--max-size-mib", type=int, default=64,
                        help="Payload size ceiling, 1–512 MiB (default: 64)")
    parser.add_argument("--timeout", type=int, default=15,
                        help="HTTP operation timeout in seconds, 1–300 (default: 15)")
    args = parser.parse_args()
    if args.run and not args.manager:
        parser.error("--run requires --manager")
    if not 1 <= args.max_size_mib <= 512:
        parser.error("--max-size-mib must be between 1 and 512")
    if not 1 <= args.timeout <= 300:
        parser.error("--timeout must be between 1 and 300")
    try:
        if args.elf.stat().st_size > args.max_size_mib * 1024 * 1024:
            raise ValueError(f"Payload exceeds the configured {args.max_size_mib} MiB ceiling")
        data = args.elf.read_bytes()
        info = inspect_elf(data)
        print(json.dumps(info, indent=2), flush=True)
        if not args.manager:
            print("Static structure passed. Execution and firmware compatibility remain unverified.")
            return 0
        parsed = urllib.parse.urlsplit(args.manager)
        if (parsed.scheme != "http" or not parsed.hostname or parsed.username
                or parsed.password or parsed.query or parsed.fragment or parsed.path not in ("", "/")):
            raise ValueError("Expected a Payload Manager HTTP base URL without credentials or path")
        base = args.manager.rstrip("/")
        name = f"{args.elf.stem}-{info['sha256'][:12]}.elf"
        if not re.fullmatch(r"[A-Za-z0-9_-]+\.elf", name):
            raise ValueError("Payload filename must contain only letters, numbers, underscores and dashes")
        inventory = json.loads(manager_request(base, "/list_payloads", timeout=args.timeout))["payloads"]
        if any(Path(path).name == name for path in inventory):
            raise ValueError("Identical payload already installed; use Payload Manager to rerun it deliberately")
        response = manager_request(
            base, "/manage:upload?" + urllib.parse.urlencode({"filename": name}),
            data=data, method="POST", headers={"Content-Type": "application/octet-stream"},
            timeout=args.timeout,
        )
        if response.strip() != b"OK":
            raise ValueError(f"Unexpected upload response: {response[:200]!r}")
        inventory = json.loads(manager_request(base, "/list_payloads", timeout=args.timeout))["payloads"]
        paths = [path for path in inventory if Path(path).name == name]
        if len(paths) != 1:
            raise ValueError("Could not uniquely locate the uploaded payload")
        print(f"Uploaded: {paths[0]}", flush=True)
        deployments.record(base, args.elf.stem, info["sha256"], paths[0])
        if not args.keep_old:
            for old in deployments.superseded(deployments.load(), base, args.elf.stem, paths[0], inventory):
                route = "/manage:delete?" + urllib.parse.urlencode({"filename": Path(old).name})
                response = manager_request(base, route, timeout=args.timeout)
                if response.strip() != b"OK":
                    raise ValueError(f"Failed to remove recorded obsolete payload {Path(old).name}")
                print(f"Removed superseded project payload: {Path(old).name}", flush=True)
        if args.run:
            response = manager_request(base, "/loadpayload:" + urllib.parse.quote(paths[0], safe="/"),
                                       timeout=args.timeout)
            if response.strip() != b"OK":
                raise ValueError(f"Unexpected launch response: {response[:200]!r}")
            print("Loader accepted the transfer. Verify execution from the probe notification/log.")
        return 0
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f"Payload operation failed: {error}", file=sys.stderr)
        if args.manager:
            print("No automatic retry. A timed-out upload or launch may still have completed.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
