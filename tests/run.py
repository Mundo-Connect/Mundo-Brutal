#!/usr/bin/env python3
"""Compile and test the production C implementation with kernel API stubs.

Requires Linux, Python 3 and a C compiler. For actual kernel APIs use make with
matching kernel headers; this harness is for arithmetic/lifecycle sanitizers.
"""
import os
import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent
HEADERS = ["linux/" + name + ".h" for name in (
    "module", "version", "limits", "jiffies", "hashtable", "jhash", "slab",
    "spinlock", "uaccess", "math64")]
HEADERS += ["net/tcp.h", "net/tls.h", "net/transp_v6.h"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", nargs="?", const="", metavar="BASELINE_C",
                        help="benchmark ACK callbacks, optionally compare another revision")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mundo-test-") as directory:
        temporary = pathlib.Path(directory)
        for name in HEADERS:
            path = temporary / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        if args.bench is not None:
            sources = [("current", ROOT.parent / "mundo.c")]
            if args.bench:
                sources.insert(0, ("baseline", pathlib.Path(args.bench).resolve(strict=True)))
            for label, source in sources:
                target = temporary / label
                subprocess.run([
                    os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-pthread",
                    "-I" + str(temporary), '-DMUNDO_SOURCE="' + str(source) + '"',
                    str(ROOT / "bench_ack.c"), "-o", str(target)
                ], check=True)
                for _ in range(5):
                    print(label, subprocess.check_output([str(target)], text=True).strip())
            return
        for version in ("0x050f00", "0x061200"):
            target = temporary / ("test-" + version)
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-g",
                "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
                "-fsanitize=" + os.environ.get("SANITIZERS", "address,undefined"),
                "-fno-omit-frame-pointer", "-pthread", "-I" + str(temporary),
                "-DLINUX_VERSION_CODE=" + version,
                str(ROOT / "test_mundo.c"), "-o", str(target)
            ], check=True)
            subprocess.run([str(target)], check=True)


if __name__ == "__main__":
    main()
