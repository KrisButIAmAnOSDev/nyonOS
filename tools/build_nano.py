#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORTS = os.path.join(ROOT, "build", "ports")
TOOLCHAIN = os.environ.get("NYONOS_TOOLCHAIN", os.path.join(ROOT, "..", "nyonos-toolchain"))
SYSROOT = os.path.join(TOOLCHAIN, "sysroot")
CROSS = os.path.join(TOOLCHAIN, "toolchain16", "bin", "x86_64-nyonos-")
NCURSES = "6.5"
NANO = "8.5"

CACHED_BIN = os.path.join(ROOT, "tools", "nano.bin")
NANO_BIN = os.path.join(PORTS, f"nano-{NANO}", "src", "nano")
NCROOT = os.path.join(PORTS, "ncroot")
NCURSES_URL = f"https://ftp.gnu.org/gnu/ncurses/ncurses-{NCURSES}.tar.gz"
NANO_URL = f"https://www.nano-editor.org/dist/v8/nano-{NANO}.tar.xz"


def run(cmd, cwd=None, quiet=True):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.DEVNULL if quiet else None,
                       stderr=subprocess.STDOUT if quiet else None)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)} (rc={r.returncode})")


def fetch(url, dest):
    if not os.path.exists(dest):
        run(["curl", "-fsSL", "-o", dest, url])


def extract(path, dest):
    if os.path.isdir(dest):
        return
    if path.endswith(".xz"):
        with tarfile.open(path, "r:xz") as tf:
            tf.extractall(os.path.dirname(path))
    else:
        with tarfile.open(path, "r:gz") as tf:
            tf.extractall(os.path.dirname(path))


def patch_config_sub(path):
    with open(path) as f:
        s = f.read()
    subs = [
        ("| storm-chaos* | os2-emx* | rtmk-nova* | managarm-* \\",
         "| storm-chaos* | os2-emx* | rtmk-nova* | managarm-* | nyonos-* \\"),
        ("| storm-chaos* | os2-emx* | rtmk-nova*)",
         "| storm-chaos* | os2-emx* | rtmk-nova* | nyonos-*)"),
        ("| fiwix* | mlibc* | cos* | mbr* | ironclad* )",
         "| fiwix* | mlibc* | nyonos* | cos* | mbr* | ironclad* )"),
        ("| fiwix* )", "| fiwix* | nyonos* )"),
        ("linux-gnu*- | linux-android*- | linux-dietlibc*- | linux-llvm*- \\",
         "nyonos*- | linux-gnu*- | linux-android*- | linux-dietlibc*- | linux-llvm*- \\"),
        ("linux-gnu* | linux-dietlibc* | linux-android* | linux-newlib* \\",
         "nyonos* | linux-gnu* | linux-dietlibc* | linux-android* | linux-newlib* \\"),
    ]
    for old, new in subs:
        if old in s:
            s = s.replace(old, new, 1)
    with open(path, "w") as f:
        f.write(s)


def main():
    if os.path.exists(CACHED_BIN):
        print(CACHED_BIN)
        return

    if os.path.exists(NANO_BIN):
        shutil.copy2(NANO_BIN, CACHED_BIN)
        print(CACHED_BIN)
        return

    os.makedirs(PORTS, exist_ok=True)

    nc_dir = os.path.join(PORTS, f"ncurses-{NCURSES}")
    nano_dir = os.path.join(PORTS, f"nano-{NANO}")
    nc_tar = os.path.join(PORTS, "ncurses.tar.gz")
    nano_tar = os.path.join(PORTS, "nano.tar.xz")

    fetch(NCURSES_URL, nc_tar)
    extract(nc_tar, nc_dir)
    patch_config_sub(os.path.join(nc_dir, "config.sub"))

    fetch(NANO_URL, nano_tar)
    extract(nano_tar, nano_dir)
    patch_config_sub(os.path.join(nano_dir, "config.sub"))

    os.environ["PATH"] = os.path.join(TOOLCHAIN, "toolchain16", "bin") + os.pathsep + os.environ.get("PATH", "")

    cores = str(os.cpu_count() or 1)

    if not os.path.exists(os.path.join(NCROOT, "usr", "lib", "libncurses.a")):
        run(["env",
             f"CC={CROSS}gcc",
             f"CFLAGS=--sysroot={SYSROOT} -D_GNU_SOURCE -O2 -fPIC",
             f"CPPFLAGS=--sysroot={SYSROOT} -D_GNU_SOURCE",
             "./configure", "--host=x86_64-nyonos", "--prefix=/usr",
             "--without-ada", "--without-cxx-binding", "--without-manpages",
             "--without-progs", "--without-tests", "--without-shared", "--with-normal",
             "--disable-widec", "--without-debug", "--without-profile",
             "--disable-stripping", "--disable-db-install"], cwd=nc_dir)
        run(["make", "-j", cores], cwd=nc_dir)
        run(["make", "install", f"DESTDIR={NCROOT}"], cwd=nc_dir)

    libnc = os.path.join(NCROOT, "usr", "lib", "libncursesw.a")
    if not os.path.exists(libnc):
        os.symlink("libncurses.a", libnc)

    run(["env",
         f"CC={CROSS}gcc",
         f"CFLAGS=--sysroot={SYSROOT} -D_GNU_SOURCE -O2",
         f"CPPFLAGS=--sysroot={SYSROOT} -D_GNU_SOURCE -I{NCROOT}/usr/include",
         f"LDFLAGS=--sysroot={SYSROOT} -L{NCROOT}/usr/lib -static",
         "./configure", "--host=x86_64-nyonos", "--prefix=/usr",
         "--disable-nls", "--disable-utf8", "--disable-nanorc"], cwd=nano_dir)
    run(["make", "-j", cores], cwd=nano_dir)

    terminfo_dir = os.path.join(PORTS, "terminfo")
    os.makedirs(terminfo_dir, exist_ok=True)
    for d in ("x", "v"):
        src = os.path.join("/lib/terminfo", d)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(terminfo_dir, d), dirs_exist_ok=True)

    shutil.copy2(NANO_BIN, CACHED_BIN)
    print(CACHED_BIN)


if __name__ == "__main__":
    main()
