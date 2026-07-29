#!/usr/bin/env python3
"""Small host-side HTTP benchmark for Avatar OS nginx.

Default target is the QEMU TAP address used by docs/app/nginx/readme.md.
The script intentionally uses only Python's standard library.
"""

from __future__ import annotations

import argparse
import http.client
import queue
import socket
import statistics
import threading
import time
from dataclasses import dataclass
from urllib.parse import urlparse


@dataclass
class Result:
    request_id: int
    worker_id: int
    ok: bool
    latency_ms: float
    connect_ms: float
    response_ms: float
    bytes_read: int
    status: int | None = None
    error: str | None = None


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    pos = (len(values) - 1) * pct / 100.0
    lo = int(pos)
    hi = min(lo + 1, len(values) - 1)
    frac = pos - lo
    return values[lo] * (1.0 - frac) + values[hi] * frac


def target_host_port(parsed) -> tuple[str, int]:
    port = parsed.port
    if port is None:
        port = 443 if parsed.scheme == "https" else 80
    return parsed.hostname, port


def make_connection(parsed, timeout: float) -> http.client.HTTPConnection:
    host, port = target_host_port(parsed)
    if parsed.scheme == "https":
        return http.client.HTTPSConnection(host, port, timeout=timeout)
    return http.client.HTTPConnection(host, port, timeout=timeout)


def request_once(conn: http.client.HTTPConnection, parsed, keepalive: bool) -> tuple[int, int]:
    path = parsed.path or "/"
    if parsed.query:
        path += "?" + parsed.query

    headers = {
        "Host": parsed.netloc,
        "Connection": "keep-alive" if keepalive else "close",
    }
    conn.request("GET", path, headers=headers)
    resp = conn.getresponse()
    body = resp.read()
    return resp.status, len(body)


def raw_connect(parsed, timeout: float) -> tuple[socket.socket, float]:
    if parsed.scheme != "http":
        raise ValueError("--trace-connect only supports http:// URLs")

    host, port = target_host_port(parsed)

    connect_start = time.perf_counter()
    sock = socket.create_connection((host, port), timeout=timeout)
    connect_ms = (time.perf_counter() - connect_start) * 1000.0
    sock.settimeout(timeout)
    return sock, connect_ms


def raw_request_on_socket(sock: socket.socket, parsed, keepalive: bool) -> tuple[int, int, float]:
    path = parsed.path or "/"
    if parsed.query:
        path += "?" + parsed.query

    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {parsed.netloc}\r\n"
        f"Connection: {'keep-alive' if keepalive else 'close'}\r\n"
        "\r\n"
    ).encode("ascii")

    response_start = time.perf_counter()
    sock.sendall(req)
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data.extend(chunk)

    header, _, rest = bytes(data).partition(b"\r\n\r\n")
    status = 0
    if header:
        parts = header.split(b"\r\n", 1)[0].split()
        if len(parts) >= 2:
            status = int(parts[1])

    content_length = None
    for line in header.split(b"\r\n")[1:]:
        name, sep, value = line.partition(b":")
        if sep and name.lower() == b"content-length":
            content_length = int(value.strip())
            break

    body = bytearray(rest)
    if content_length is not None:
        while len(body) < content_length:
            chunk = sock.recv(4096)
            if not chunk:
                break
            body.extend(chunk)
    else:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            body.extend(chunk)

    response_ms = (time.perf_counter() - response_start) * 1000.0
    return status, len(body), response_ms


def raw_request_once(parsed, timeout: float, keepalive: bool) -> tuple[int, int, float, float]:
    sock, connect_ms = raw_connect(parsed, timeout)
    try:
        status, bytes_read, response_ms = raw_request_on_socket(sock, parsed, keepalive)
        return status, bytes_read, connect_ms, response_ms
    finally:
        sock.close()


def worker(worker_id: int, parsed, jobs: queue.Queue[int], results: list[Result],
           results_lock: threading.Lock, timeout: float, keepalive: bool,
           trace_connect: bool) -> None:
    conn: http.client.HTTPConnection | None = None
    raw_sock: socket.socket | None = None
    raw_connect_ms = 0.0

    while True:
        try:
            request_id = jobs.get_nowait()
        except queue.Empty:
            break

        start = time.perf_counter()
        status = None
        try:
            if trace_connect:
                if keepalive:
                    if raw_sock is None:
                        raw_sock, raw_connect_ms = raw_connect(parsed, timeout)
                    connect_ms = raw_connect_ms
                    raw_connect_ms = 0.0
                    status, bytes_read, response_ms = raw_request_on_socket(raw_sock, parsed, True)
                else:
                    status, bytes_read, connect_ms, response_ms = raw_request_once(parsed, timeout, False)
                elapsed_ms = (time.perf_counter() - start) * 1000.0
                result = Result(request_id=request_id, worker_id=worker_id,
                                ok=(200 <= status < 300), status=status,
                                latency_ms=elapsed_ms, connect_ms=connect_ms,
                                response_ms=response_ms, bytes_read=bytes_read)
            else:
                if conn is None:
                    conn = make_connection(parsed, timeout)
                    conn.connect()
                connect_ms = 0.0
                status, bytes_read = request_once(conn, parsed, keepalive)
                elapsed_ms = (time.perf_counter() - start) * 1000.0
                result = Result(request_id=request_id, worker_id=worker_id,
                                ok=(200 <= status < 300), status=status,
                                latency_ms=elapsed_ms, connect_ms=connect_ms,
                                response_ms=elapsed_ms - connect_ms,
                                bytes_read=bytes_read)
                if not keepalive:
                    conn.close()
                    conn = None
        except Exception as exc:  # noqa: BLE001 - benchmark reports any request failure.
            if conn is not None:
                try:
                    conn.close()
                except Exception:
                    pass
            conn = None
            if raw_sock is not None:
                try:
                    raw_sock.close()
                except Exception:
                    pass
            raw_sock = None
            raw_connect_ms = 0.0
            elapsed_ms = (time.perf_counter() - start) * 1000.0
            result = Result(request_id=request_id, worker_id=worker_id,
                            ok=False, status=status, latency_ms=elapsed_ms,
                            connect_ms=0.0, response_ms=0.0, bytes_read=0,
                            error=f"worker {worker_id}: {type(exc).__name__}: {exc}")

        with results_lock:
            results.append(result)

        jobs.task_done()

    if conn is not None:
        conn.close()
    if raw_sock is not None:
        raw_sock.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark Avatar OS nginx from host")
    parser.add_argument("url", nargs="?", default="http://192.168.100.2/",
                        help="target URL, default: http://192.168.100.2/")
    parser.add_argument("-n", "--requests", type=int, default=1000,
                        help="total requests, default: 1000")
    parser.add_argument("-c", "--concurrency", type=int, default=16,
                        help="parallel connections, default: 16")
    parser.add_argument("-t", "--timeout", type=float, default=5.0,
                        help="per-request socket timeout in seconds, default: 5")
    parser.add_argument("-w", "--warmup", type=int, default=0,
                        help="sequential warmup requests before timing, default: 0")
    parser.add_argument("--slow-ms", type=float, default=1000.0,
                        help="print successful requests slower than this many ms, default: 1000")
    parser.add_argument("--no-keepalive", action="store_true",
                        help="open a new TCP connection for every request")
    parser.add_argument("--trace-connect", action="store_true",
                        help="use raw HTTP and split TCP connect vs response timing")
    parser.add_argument("--fail-fast", action="store_true",
                        help="exit non-zero if any request fails")
    args = parser.parse_args()

    parsed = urlparse(args.url)
    if parsed.scheme not in ("http", "https") or not parsed.hostname:
        parser.error("url must be an absolute http:// or https:// URL")
    if args.requests <= 0:
        parser.error("--requests must be > 0")
    if args.concurrency <= 0:
        parser.error("--concurrency must be > 0")
    if args.warmup < 0:
        parser.error("--warmup must be >= 0")

    keepalive = not args.no_keepalive

    if args.warmup:
        print(f"warmup={args.warmup} target={args.url}")
        conn = make_connection(parsed, args.timeout)
        try:
            for _ in range(args.warmup):
                request_once(conn, parsed, keepalive=True)
        finally:
            conn.close()

    jobs: queue.Queue[int] = queue.Queue()
    for i in range(args.requests):
        jobs.put(i)

    results: list[Result] = []
    results_lock = threading.Lock()
    workers = min(args.concurrency, args.requests)

    print(f"target={args.url} requests={args.requests} concurrency={workers} timeout={args.timeout}s keepalive={keepalive}")

    start = time.perf_counter()
    threads = [
        threading.Thread(target=worker,
                         args=(i, parsed, jobs, results, results_lock, args.timeout,
                               keepalive, args.trace_connect),
                         daemon=True)
        for i in range(workers)
    ]

    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    elapsed = time.perf_counter() - start

    ok_results = [r for r in results if r.ok]
    failed_results = [r for r in results if not r.ok]
    latencies = sorted(r.latency_ms for r in ok_results)
    connect_latencies = sorted(r.connect_ms for r in ok_results if r.connect_ms > 0)
    response_latencies = sorted(r.response_ms for r in ok_results if r.response_ms > 0)
    bytes_total = sum(r.bytes_read for r in ok_results)
    rps = len(ok_results) / elapsed if elapsed > 0 else 0.0
    mbps = (bytes_total / (1024.0 * 1024.0)) / elapsed if elapsed > 0 else 0.0

    print()
    print(f"elapsed:     {elapsed:.3f} s")
    print(f"completed:   {len(results)}")
    print(f"ok:          {len(ok_results)}")
    print(f"failed:      {len(failed_results)}")
    print(f"throughput:  {rps:.2f} req/s")
    print(f"body rate:   {mbps:.3f} MiB/s")

    if latencies:
        print(f"lat avg:     {statistics.fmean(latencies):.3f} ms")
        print(f"lat median:  {percentile(latencies, 50):.3f} ms")
        print(f"lat p90:     {percentile(latencies, 90):.3f} ms")
        print(f"lat p99:     {percentile(latencies, 99):.3f} ms")
        print(f"lat max:     {latencies[-1]:.3f} ms")

        if connect_latencies:
            print(f"conn p50:    {percentile(connect_latencies, 50):.3f} ms")
            print(f"conn p99:    {percentile(connect_latencies, 99):.3f} ms")
            print(f"conn max:    {connect_latencies[-1]:.3f} ms")
        if response_latencies:
            print(f"resp p50:    {percentile(response_latencies, 50):.3f} ms")
            print(f"resp p99:    {percentile(response_latencies, 99):.3f} ms")
            print(f"resp max:    {response_latencies[-1]:.3f} ms")

        slow = sorted((r for r in ok_results if r.latency_ms >= args.slow_ms),
                      key=lambda r: r.latency_ms, reverse=True)
        if slow:
            print()
            print(f"slow requests >= {args.slow_ms:.1f} ms:")
            for result in slow[:10]:
                print(f"  req={result.request_id} worker={result.worker_id} "
                      f"status={result.status} latency={result.latency_ms:.3f}ms "
                      f"connect={result.connect_ms:.3f}ms response={result.response_ms:.3f}ms")

    if failed_results:
        print()
        print("first failures:")
        for result in failed_results[:10]:
            print(f"  req={result.request_id} worker={result.worker_id} "
                  f"status={result.status} latency={result.latency_ms:.3f}ms error={result.error}")

    return 1 if args.fail_fast and failed_results else 0


if __name__ == "__main__":
    raise SystemExit(main())


"""

ajax@ajax-BOD-WXX9:~/Desktop/Project/Kernel/avatar-next$ python3 docs/app/nginx/bench.py -n 2000 -c 4 -w 5 --slow-ms 100 --no-keepalive 
warmup=5 target=http://192.168.100.2/
target=http://192.168.100.2/ requests=2000 concurrency=4 timeout=5.0s keepalive=False

elapsed:     1.561 s
completed:   2000
ok:          2000
failed:      0
throughput:  1281.05 req/s
body rate:   0.059 MiB/s
lat avg:     3.109 ms
lat median:  3.041 ms
lat p90:     3.543 ms
lat p99:     4.174 ms
lat max:     6.827 ms
resp p50:    3.041 ms
resp p99:    4.174 ms
resp max:    6.827 ms
ajax@ajax-BOD-WXX9:~/Desktop/Project/Kernel/avatar-next$ python3 docs/app/nginx/bench.py -n 2000 -c 4 -w 5 --slow-ms 100 
warmup=5 target=http://192.168.100.2/
target=http://192.168.100.2/ requests=2000 concurrency=4 timeout=5.0s keepalive=True

elapsed:     1.264 s
completed:   2000
ok:          2000
failed:      0
throughput:  1582.42 req/s
body rate:   0.072 MiB/s
lat avg:     2.517 ms
lat median:  0.609 ms
lat p90:     0.721 ms
lat p99:     0.937 ms
lat max:     1262.239 ms
resp p50:    0.609 ms
resp p99:    0.937 ms
resp max:    1262.239 ms

slow requests >= 100.0 ms:
  req=3 worker=3 status=200 latency=1262.239ms connect=0.000ms response=1262.239ms
  req=1 worker=1 status=200 latency=1261.868ms connect=0.000ms response=1261.868ms
  req=1003 worker=0 status=200 latency=642.327ms connect=0.000ms response=642.327ms
  req=2 worker=2 status=200 latency=621.781ms connect=0.000ms response=621.781ms
  
"""