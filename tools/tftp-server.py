#!/usr/bin/env python3
"""
高性能 TFTP 服务器 — AF_PACKET 原始套接字（绕过 Hyper-V/WSL2 对 UDP 69 端口的占用）
支持 RFC 2348 blksize / RFC 2349 tsize / RFC 7440 windowsize 真滑动窗口传输。

用法:
    sudo python3 tools/tftp-server.py [--root /srv/tftp] [--iface enx207bd2d4d4e9]

U-Boot 端加速（在 U-Boot 命令行执行一次，之后 saveenv 保存）:
    setenv tftpblocksize 1468
    setenv tftpwindowsize 32
期望速度: ~10–30 MiB/s（原 75 KiB/s → 提升约 150–400×）
"""
import socket, struct, os, threading, argparse, time, select as _select

DEFAULT_ROOT  = "/home/ajax/Proj/OS/Thread-Process-Lock/avatar/imgs"
DEFAULT_IFACE = "enx207bd2d4d4e9"
TFTP_PORT     = 69
MAX_BLKSIZE   = 1468   # MTU 1500 - IP 20 - UDP 8 - TFTP 4
MAX_WINDOW    = 128    # RFC 7440 最大窗口（U-Boot 通常请求 ≤ 32）
MAX_RETRIES   = 8
TIMEOUT       = 2.0

# ── 以太网帧解析 ────────────────────────────────────────────────────────────

def parse_frame(frame):
    if len(frame) < 14: return None
    if struct.unpack('!H', frame[12:14])[0] != 0x0800: return None  # IPv4 only
    ip = frame[14:]
    if len(ip) < 20 or ip[9] != 17: return None                     # UDP only
    ihl = (ip[0] & 0xF) * 4
    src_ip = socket.inet_ntoa(ip[12:16])
    udp = ip[ihl:]
    if len(udp) < 8: return None
    src_port, dst_port = struct.unpack('!HH', udp[:4])
    return src_ip, src_port, dst_port, udp[8:]

# ── 每个 RRQ 的处理线程 ─────────────────────────────────────────────────────

def send_error(tx, addr, code, msg):
    tx.sendto(struct.pack('!HH', 5, code) + msg.encode() + b'\x00', addr)

def handle_rrq(client_ip, client_port, filename, options, tftp_root):
    filepath = os.path.join(tftp_root, os.path.basename(filename))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # 加大内核 UDP 缓冲区：避免大窗口突发时被迫等待内核排队
    tx.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)   # 4 MB 发送缓冲
    tx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)   # 1 MB 接收缓冲
    tx.bind(('0.0.0.0', 0))
    addr = (client_ip, client_port)

    try:
        with open(filepath, 'rb') as f:
            data = f.read()
    except FileNotFoundError:
        print(f"[TFTP] ERROR: not found: {filepath}", flush=True)
        send_error(tx, addr, 1, 'File not found')
        tx.close(); return

    file_size   = len(data)
    # 协商选项（key 统一转小写）
    opts        = {k.lower(): v for k, v in options.items()}
    blksize     = min(int(opts.get(b'blksize',    512)),  MAX_BLKSIZE)
    window_size = min(int(opts.get(b'windowsize', 1)),    MAX_WINDOW)

    print(f"[TFTP] {filename} ({file_size:,}B) -> {addr}  "
          f"blksize={blksize} win={window_size}", flush=True)

    # ── OACK（有选项时必须回复，含 tsize 告知文件大小）──────────────────────
    oack_fields = {}
    if b'blksize'    in opts: oack_fields[b'blksize']    = str(blksize).encode()
    if b'tsize'      in opts: oack_fields[b'tsize']      = str(file_size).encode()
    if b'windowsize' in opts: oack_fields[b'windowsize'] = str(window_size).encode()

    if oack_fields:
        oack = struct.pack('!H', 6)  # opcode OACK = 6
        for k, v in oack_fields.items():
            oack += k + b'\x00' + v + b'\x00'
        for retry in range(MAX_RETRIES):
            tx.sendto(oack, addr)
            r, _, _ = _select.select([tx], [], [], TIMEOUT)
            if r:
                try:
                    ack, _ = tx.recvfrom(4)
                    op, blk = struct.unpack('!HH', ack)
                    if op == 4 and blk == 0:
                        break
                except Exception:
                    pass
            print(f"[TFTP] OACK timeout (retry {retry})", flush=True)
        else:
            print("[TFTP] OACK failed, aborting", flush=True)
            tx.close(); return

    # ── 预打包所有 DATA 包（一次性完成，省去传输循环里的编码开销）──────────
    pkts = []
    off  = 0
    while True:
        chunk   = data[off : off + blksize]
        blk_num = (len(pkts) + 1) & 0xFFFF
        pkts.append(struct.pack('!HH', 3, blk_num) + chunk)
        if len(chunk) < blksize:
            break
        off += blksize
    total = len(pkts)

    # ── RFC 7440 真滑动窗口传输 ──────────────────────────────────────────────
    # base: 第一个未 ACK 的包索引（0-based）
    # nxt:  下一个待发包索引
    # 每收到一个 ACK 立即补发新包，管道始终满载，窗口间无空泡。
    t_start = time.monotonic()
    base    = 0
    nxt     = 0
    retries = 0

    while base < total:
        # 补充发送：把窗口内未发的包全部打出去
        while nxt < min(base + window_size, total):
            tx.sendto(pkts[nxt], addr)
            nxt += 1

        # 等 ACK（使用 select 而非 settimeout，避免 Python 异常开销）
        r, _, _ = _select.select([tx], [], [], TIMEOUT)
        if not r:
            retries += 1
            if retries >= MAX_RETRIES:
                print(f"[TFTP] transfer failed at block {base + 1}", flush=True)
                tx.close(); return
            print(f"[TFTP] timeout blk={base+1}, retransmit win (retry {retries})", flush=True)
            nxt = base   # 回退到窗口起点重发
            continue

        try:
            raw, _ = tx.recvfrom(4)
            op, blk = struct.unpack('!HH', raw)
        except Exception:
            continue
        if op != 4:
            continue

        retries = 0
        # 将 block 编号映射回包索引（倒序查找，优先匹配最新的 ACK）
        for i in range(nxt - 1, base - 1, -1):
            if (i + 1) & 0xFFFF == blk:
                base = i + 1   # 滑动窗口基准前进
                break
        # 收到 ACK 后立刻回到循环顶部补发新包（热路径）

    elapsed = time.monotonic() - t_start
    speed   = file_size / elapsed / (1024 * 1024)
    print(f"[TFTP] done!  {file_size:,}B  {elapsed:.2f}s  {speed:.2f} MiB/s", flush=True)
    tx.close()

# ── 主循环：监听原始以太网帧 ────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root',  default=DEFAULT_ROOT)
    ap.add_argument('--iface', default=DEFAULT_IFACE)
    args = ap.parse_args()

    raw = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0800))
    raw.bind((args.iface, 0))
    print(f"[TFTP] listening on {args.iface}:{TFTP_PORT}  root={args.root}", flush=True)
    print(f"[TFTP] U-Boot 加速: setenv tftpblocksize 1468; setenv tftpwindowsize 32", flush=True)

    while True:
        frame, _ = raw.recvfrom(65536)
        r = parse_frame(frame)
        if r is None: continue
        src_ip, src_port, dst_port, payload = r
        if dst_port != TFTP_PORT or len(payload) < 2: continue
        opcode = struct.unpack('!H', payload[:2])[0]
        if opcode == 1:  # RRQ
            parts    = payload[2:].split(b'\x00')
            filename = parts[0].decode(errors='replace')
            opts = {}
            i = 2
            while i + 1 < len(parts) and parts[i]:
                opts[parts[i].lower()] = parts[i+1]; i += 2
            print(f"[TFTP] RRQ '{filename}' from {src_ip}:{src_port}  opts={opts}", flush=True)
            threading.Thread(target=handle_rrq,
                             args=(src_ip, src_port, filename, opts, args.root),
                             daemon=True).start()

if __name__ == '__main__':
    main()

if __name__ == '__main__':
    main()
