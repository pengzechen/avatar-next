#!/usr/bin/env python3
"""三个架构 guest 的 initrd —— 从同一份源目录重建（x86_64 / aarch64 / rv64）

三个包的内容**完全一致**，只有 bin/busybox 是各架构各自的 ELF：

    bin/busybox     从旧包里原样搬过来（几 MB 二进制，不进 git）
    bin/sh          -> busybox 软链接（保留它，`rdinit=/bin/sh` 那个裸跑配方才还能用）
    init            三个架构共用的同一份 imgs/guests/initrd-common/init
    etc/passwd      只有一行 root —— busybox v1.32 的 ash 是从 passwd 里取家目录的
    etc/group       少了这两个文件 aarch64 的提示符会是 "/ #"，另两个架构是 "~ #"
    bin/ dev/ etc/ proc/ sys/ tmp/   六个空目录（git 存不了空目录，由工具补）

包内不再预置那 ~400 个 applet 软链接和 /etc/{inittab,shadow,securetty}：
它们只服务于已经删掉的 getty 登录那条路，applet 软链接现在由 init 里的
`busybox --install -s /bin` 在运行时生成（/bin 必须已存在，所以目录要先进包）。

为什么不用「解包 -> 覆盖 -> 重打」：aarch64 那个老包里有设备节点（dev/console 等），
非 root 解不出来（mknod 没权限）。这里直接在新c 流上重建，全程不落地。

    python3 tools/guest-initrd.py x86_64              # 重建 imgs/guests/x86_64/initrd.gz
    python3 tools/guest-initrd.py aarch64 --strip     # 顺便 strip 掉搬运过来的 busybox
    python3 tools/guest-initrd.py rv64 --show         # 只打印源 init，不动产物

压缩策略是**全裸 cpio**（三个包一致，guest 侧不解压）。老包里 gzip 过的那个会被
自动写成裸 cpio 并提示。要改回 gzip 加 --gzip。

⚠️ 产物要跑进 Avatar VMM 还得先进 rootfs 镜像（变体必须一致、kernel 放最后，
   否则内核被悄悄换成非 guest 变体）：
     make PLATFORM=qemu-virt-<p> GUEST_LINUX=1 rootfs
     make PLATFORM=qemu-virt-<p> GUEST_LINUX=1 kernel
"""

import gzip
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

MAGIC = b"070701"
HDR = 110                       # magic(6) + 13 个 8 位十六进制字段
S_IFREG, S_IFLNK, S_IFDIR = 0o100000, 0o120000, 0o040000

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = "imgs/guests/initrd-common"                    # 唯一的文本源目录（三个架构共用）
DIRS = ["bin", "dev", "etc", "proc", "sys", "tmp"]   # git 存不了空目录，固定由工具补
CARRY = ["bin/busybox"]                              # 从旧包原样搬的二进制

TARGETS = {
    "x86_64": "imgs/guests/x86_64/initrd.gz",
    "aarch64": "imgs/guests/aarch64/initrd.gz",
    "rv64": "imgs/guests/rv64/initrd.gz",
}

STRIP = {                                            # 交叉 strip 的名字（先查 PATH）
    "x86_64": "x86_64-linux-musl-strip",
    "aarch64": "aarch64-linux-musl-strip",
    "rv64": "riscv64-linux-musl-strip",
}
TRIPLE = {"x86_64": "x86_64", "aarch64": "aarch64", "rv64": "riscv64"}
TOOLCHAIN = "~/Desktop/Software/compiler/%s-linux-musl-cross/bin"


def pad4(n):
    return (-n) % 4


def parse(buf):
    """把 newc 流切成条目列表（含 TRAILER），用于搬运/校验。"""
    out, off = [], 0
    while off + HDR <= len(buf):
        if buf[off:off + 6] != MAGIC:
            raise SystemExit("offset %d: 不是 newc 头 %r" % (off, buf[off:off + 6]))
        f = [int(buf[off + 6 + 8 * i:off + 14 + 8 * i], 16) for i in range(13)]
        namesize, size = f[11], f[6]
        nstart = off + HDR
        name = buf[nstart:nstart + namesize - 1].decode("utf-8", "surrogateescape")
        dstart = nstart + namesize + pad4(nstart + namesize)
        data = buf[dstart:dstart + size]
        end = dstart + size + pad4(dstart + size)
        out.append({"name": name, "mode": f[1], "size": size, "data": data,
                    "uid": f[2], "gid": f[3], "mtime": f[5], "raw": buf[off:end]})
        off = end
        if name == "TRAILER!!!":
            break
    return out


def build(name, mode, data, uid=0, gid=0, mtime=0, ino=0, nlink=1):
    """按 newc 造一个条目（含尾部对齐）。"""
    nm = name.encode() + b"\0"
    hdr = MAGIC + b"".join(b"%08X" % v for v in
                           (ino, mode, uid, gid, nlink, mtime, len(data),
                            0, 0, 0, 0, len(nm), 0))
    out = hdr + nm
    out += b"\0" * pad4(len(out))
    out += data
    out += b"\0" * pad4(len(out))
    return out


def norm(name):
    """包内名字归一成 'a/b' 形式（"." / "./x" / "x" 都一样）。"""
    name = name.rstrip("\0")
    while name.startswith("./"):
        name = name[2:]
    return "" if name == "." else name


def kind(mode):
    return {S_IFREG: "reg", S_IFLNK: "lnk", S_IFDIR: "dir"}.get(mode & 0o170000, "?")


def snapshot(buf):
    """名字 -> (mode, size, 内容 hash)，用于重建前后对比。"""
    d = {}
    for e in parse(buf):
        if e["name"] == "TRAILER!!!":
            continue
        d[norm(e["name"])] = (kind(e["mode"]), e["size"],
                              hashlib.md5(e["data"]).hexdigest())
    return d


def strip_busybox(arch, data):
    """跑交叉 strip。已经是 stripped 的二进制再 strip 一次是幂等的。"""
    tool = shutil.which(STRIP[arch])
    if not tool:
        cand = os.path.expanduser(TOOLCHAIN % TRIPLE[arch]) + "/" + STRIP[arch]
        tool = cand if os.path.exists(cand) else None
    if not tool:
        raise SystemExit("找不到 %s（PATH 和 %s 都没有）"
                         % (STRIP[arch], os.path.expanduser(TOOLCHAIN % TRIPLE[arch])))
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "busybox")
        with open(p, "wb") as fp:                     # busybox 按 argv[0] 分派，
            fp.write(data)                            # 文件名不影响 strip
        before = os.path.getsize(p)
        subprocess.run([tool, p], check=True)
        out = open(p, "rb").read()
    if out[:4] != b"\x7fELF":
        raise SystemExit("strip 之后不是 ELF，别写进包里")
    return out, before, len(out), os.path.basename(tool)


def collect_src(src_dir):
    """源目录 -> [(rel, 绝对路径)]，含软链接。"""
    out = []
    for root, _dirs, fns in os.walk(src_dir):
        for fn in fns:
            p = os.path.join(root, fn)
            out.append((norm(os.path.relpath(p, src_dir)), p))
    return sorted(out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = {a for a in sys.argv[1:] if a.startswith("--")}
    if len(args) != 1 or args[0] not in TARGETS:
        raise SystemExit("用法: %s {%s} [--strip] [--gzip] [--show]"
                         % (sys.argv[0], "|".join(TARGETS)))
    arch = args[0]
    img = os.path.join(ROOT, TARGETS[arch])
    src = os.path.join(ROOT, SRC)

    if "--show" in opts:
        print("===== %s =====" % os.path.join(SRC, "init"))
        sys.stdout.write(open(os.path.join(src, "init")).read())
        return

    raw_old = open(img, "rb").read()
    was_gz = raw_old[:2] == b"\x1f\x8b"
    old = gzip.decompress(raw_old) if was_gz else raw_old
    old_entries = {norm(e["name"]): e for e in parse(old) if e["name"] != "TRAILER!!!"}

    # 1) 搬运二进制。uid/gid 一律归 0（旧包里是宿主用户的 1000，三个包不一致，
    #    而 guest 里就是 root，留着只会让 ls -l 在各架构上显示的数字不一样）
    carry = []
    for name in CARRY:
        if name not in old_entries:
            raise SystemExit("旧包 %s 里没有 %s，没法搬" % (TARGETS[arch], name))
        e = old_entries[name]
        carry.append(dict(name=name, data=e["data"], mtime=e["mtime"]))

    note = ""
    if "--strip" in opts:
        for c in carry:
            data, b, a, tool = strip_busybox(arch, c["data"])
            if a != b:
                note = "  strip: %s %d -> %d B (-%d%%)" % (tool, b, a, (b - a) * 100 // b)
            c["data"] = data

    # 2) 组装：目录 -> 搬运的二进制 -> 源目录里的文件（顺序：目录在前）
    out, names = [], []
    for i, d in enumerate(DIRS):
        out.append(build("./" + d, S_IFDIR | 0o755, b"", ino=i + 1))
        names.append(d)
    for i, c in enumerate(carry):
        out.append(build("./" + c["name"], S_IFREG | 0o755, c["data"],
                         mtime=c["mtime"], ino=len(DIRS) + i + 1))
        names.append(c["name"])
    for i, (rel, path) in enumerate(collect_src(src)):
        st = os.lstat(path)
        if os.path.islink(path):
            out.append(build("./" + rel, S_IFLNK | 0o777,
                             os.readlink(path).encode(), mtime=int(st.st_mtime),
                             ino=len(DIRS) + len(carry) + i + 1))
        else:
            out.append(build("./" + rel, S_IFREG | (st.st_mode & 0o7777),
                             open(path, "rb").read(), mtime=int(st.st_mtime),
                             ino=len(DIRS) + len(carry) + i + 1))
        names.append(rel)
    out.append(build("TRAILER!!!", 0, b""))

    new = b"".join(out)
    if "--gzip" in opts:
        new = gzip.compress(new, 9)

    # 3) 校验：条目集合必须**正好**是 DIRS+CARRY+源文件；init 内容必须等于源文件
    new_snap, old_snap = snapshot(new), snapshot(old)
    if sorted(new_snap) != sorted(names):
        raise SystemExit("!! 重打包后的条目对不上：%s" % sorted(set(new_snap) ^ set(names)))
    want_init = open(os.path.join(src, "init")).read()
    init_entry = next(e for e in parse(new) if norm(e["name"]) == "init")
    if init_entry["data"] != want_init.encode():
        raise SystemExit("!! 包内 init 与 %s/init 不一致" % SRC)
    if not init_entry["mode"] & 0o111:
        raise SystemExit("!! %s/init 没有执行位（chmod +x 一下）" % SRC)
    for c in carry:
        if not c["data"]:
            raise SystemExit("!! %s 空掉了" % c["name"])
    if os.readlink(os.path.join(src, "bin/sh")) != "busybox":
        raise SystemExit("!! %s/bin/sh 应该是指向 busybox 的软链接" % SRC)

    tmp = img + ".new"
    open(tmp, "wb").write(new)
    os.replace(tmp, img)

    print("已重建 %s (%d -> %d bytes, %s)" %
          (TARGETS[arch], len(raw_old), len(new),
           "gzip" if "--gzip" in opts else "裸 cpio"))
    print("条目 %d -> %d：%s" %
          (len(old_snap), len(new_snap), ", ".join(sorted(new_snap))))
    print("bin/busybox %d B（内容%s）%s" %
          (len(carry[0]["data"]), "已变（strip 过）" if note else "与旧包一致", note))
    if was_gz and "--gzip" not in opts:
        print("注意：旧包是 gzip，按全裸策略写成了裸 cpio")


if __name__ == "__main__":
    main()
