#!/usr/bin/env python3
# Downloads the Windows builds of Qt and the other libraries DiskAnvil needs from MSYS2's UCRT64
# repository, with everything they depend on, and unpacks them into one folder that the cross
# build uses as its sysroot. Each package is checked against the checksum in the repository list.
#   fetch-msys2.py <folder> <package>...
# Package names are without the mingw-w64-ucrt-x86_64- prefix (qt6-base, zstd...).
import hashlib
import io
import os
import subprocess
import sys
import tarfile
import urllib.request

REPO = "https://repo.msys2.org/mingw/ucrt64"
PREFIX = "mingw-w64-ucrt-x86_64-"
# Things a package lists as needed that aren't needed to build or run DiskAnvil on Windows (asked
# for by name, they still come: sqlite3, for its sqlite3.exe).
SKIP = {"python", "perl", "tcl", "tk", "sqlite3", "gdb", "cmake", "ninja", "pkgconf"}


def fetch(url):
    with urllib.request.urlopen(url) as reply:
        return reply.read()


def unzstd(data):
    return subprocess.run(["zstd", "-dcq"], input=data, capture_output=True, check=True).stdout


def read_repo(cache):
    path = os.path.join(cache, "ucrt64.db")
    if not os.path.exists(path):
        with open(path + ".part", "wb") as f:
            f.write(fetch(f"{REPO}/ucrt64.db"))
        os.replace(path + ".part", path)
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == b"\x28\xb5\x2f\xfd":
        data = unzstd(data)
    packages = {}
    provides = {}
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        for member in tar:
            if not member.name.endswith("/desc"):
                continue
            fields = {}
            key = None
            for line in tar.extractfile(member).read().decode().splitlines():
                if line.startswith("%") and line.endswith("%"):
                    key = line.strip("%")
                    fields[key] = []
                elif line and key:
                    fields[key].append(line)
            name = fields["NAME"][0]
            packages[name] = fields
            for p in fields.get("PROVIDES", []):
                provides.setdefault(p.split("=")[0].split(">")[0].split("<")[0], name)
    return packages, provides


def bare(dep):
    for sep in ">=<":
        dep = dep.split(sep)[0]
    return dep


def resolve(wanted, packages, provides):
    order = []
    seen = set()
    todo = [PREFIX + w for w in wanted]
    while todo:
        name = bare(todo.pop())
        name = name if name in packages else provides.get(name, name)
        if name in seen:
            continue
        seen.add(name)
        if name not in packages:
            sys.exit(f"not in the repository: {name}")
        if name.removeprefix(PREFIX) in SKIP and name.removeprefix(PREFIX) not in wanted:
            continue
        order.append(name)
        todo.extend(d for d in packages[name].get("DEPENDS", []) if d.startswith(PREFIX))
    return order


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__ or "fetch-msys2.py <folder> <package>...")
    root = os.path.abspath(sys.argv[1])
    cache = os.path.join(root, "packages")
    sysroot = os.path.join(root, "sysroot")
    os.makedirs(cache, exist_ok=True)
    os.makedirs(sysroot, exist_ok=True)
    packages, provides = read_repo(cache)
    names = resolve(sys.argv[2:], packages, provides)
    for name in sorted(names):
        desc = packages[name]
        filename = desc["FILENAME"][0]
        path = os.path.join(cache, filename)
        if not os.path.exists(path):
            print(f"  downloading {filename}", flush=True)
            data = fetch(f"{REPO}/{filename}")
            if hashlib.sha256(data).hexdigest() != desc["SHA256SUM"][0]:
                sys.exit(f"wrong checksum: {filename}")
            with open(path + ".part", "wb") as f:
                f.write(data)
            os.replace(path + ".part", path)
        stamp = os.path.join(sysroot, ".unpacked", filename)
        if os.path.exists(stamp):
            continue
        with open(path, "rb") as f:
            data = unzstd(f.read())
        with tarfile.open(fileobj=io.BytesIO(data)) as tar:
            members = [m for m in tar if not m.name.startswith(".")]
            tar.extractall(sysroot, members=members, filter="tar")
        os.makedirs(os.path.dirname(stamp), exist_ok=True)
        open(stamp, "w").close()
    print(f"{len(names)} packages in {sysroot}")


if __name__ == "__main__":
    main()
