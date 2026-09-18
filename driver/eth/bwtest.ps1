<#
.SYNOPSIS
    Avatar OS / SG2002 以太网带宽测试 —— UDP blast 到板子的 bwtest sink。

.DESCRIPTION
    搭配 kernel/net/bwtest.c 使用。往 <板子IP>:1235 打 UDP 包，每条 datagram
    前 8 字节是 "AVBW"(0x41564257，小端) magic + 小端递增序号，其后是填充。

    板子会在**串口**上每秒打一行速率统计（[bwtest] ... MB/s ... lost=... ooo=...）；
    本脚本给出的是 PC 侧的发送总量和平均速率，并把结果写进日志文件。

    两边要对照着看：PC 侧是"发了多少"，板子侧是"收了多少、丢了多少"。

.PARAMETER Target
    板子 IP，默认 192.168.7.1。

.PARAMETER Port
    bwtest sink 端口，默认 1235（与 TCP echo 1234 / HTTP 80 不冲突）。

.PARAMETER Count
    发送包数，默认 20000。1400B 时约 28 MB，线速下约 2.4 秒。

.PARAMETER Size
    每条 datagram 字节数，默认 1400。可试 1472（不会触发 IP 分片的上限）。

.PARAMETER OutFile
    结果日志路径，默认 bwtest-<时间戳>.log（写到当前目录）。

.EXAMPLE
    .\bwtest.ps1
    .\bwtest.ps1 -Count 200000
    .\bwtest.ps1 -Target 192.168.7.1 -Count 200000 -OutFile run1.log

.NOTES
    首次运行若被脚本执行策略拦住，用：
        powershell -ExecutionPolicy Bypass -File .\bwtest.ps1
    或先放开当前会话：
        Set-ExecutionPolicy -Scope Process Bypass

    1400 字节载荷时，100 Mbps 线上的理论上限：
        线上每帧 = 1400 (载荷) + 28 (IP+UDP 头) + 38 (前导/帧间隙/FCS) = 1466 B
        有效带宽 = 100 × 1400 / 1466 ≈ 95.5 Mbps ≈ 11.4 MB/s
    板子跑到 11 MB/s 以上、且 lost=0，就说明以太网和驱动这层基本是线速了。
#>

param(
    [string]$Target  = '192.168.7.1',
    [int]   $Port    = 1235,
    [int]   $Count   = 20000,
    [int]   $Size    = 1400,
    [string]$OutFile = "bwtest-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"
)

$ErrorActionPreference = 'Stop'

if ($Size -lt 8)  { throw "Size 必须 >= 8（要放得下 8 字节测试头）" }
if ($Count -lt 1) { throw "Count 必须 >= 1" }

# 超过 1472 会触发 IP 分片，板子收到的包数会翻倍、速率也就不可比了
if ($Size -gt 1472) {
    Write-Warning "Size=$Size > 1472 会触发 IP 分片，测出的速率不可直接和上限比较"
}

$udp = New-Object System.Net.Sockets.UdpClient
$udp.Connect($Target, $Port)

# 固定部分（magic + 填充）预先填好，循环里只重写序号那 4 个字节，
# 避免每包都新建 byte[] 造成 GC 压力。
$buf = New-Object byte[] $Size
$buf[0] = 0x57; $buf[1] = 0x42; $buf[2] = 0x56; $buf[3] = 0x41   # 0x41564257 小端
for ($i = 8; $i -lt $Size; $i++) { $buf[$i] = [byte]($i -band 0xFF) }

Write-Host ("发送 {0} 包 x {1} B 到 {2}:{3} ..." -f $Count, $Size, $Target, $Port)

$chunk = [math]::Max(1, [int]($Count / 4))
$sw = [System.Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $Count; $i++) {
    $buf[4] = [byte]($i -band 0xFF)
    $buf[5] = [byte](($i -shr 8)  -band 0xFF)
    $buf[6] = [byte](($i -shr 16) -band 0xFF)
    $buf[7] = [byte](($i -shr 24) -band 0xFF)
    [void]$udp.Send($buf, $Size)

    if (($i % $chunk) -eq 0) { Write-Host ("  {0} / {1}" -f $i, $Count) }
}
$sw.Stop()
$udp.Close()

$mb   = $Count * $Size / 1MB   # 1MB = 1048576
$secs = $sw.Elapsed.TotalSeconds
$mbs  = $mb / $secs
$mbps = $mbs * 8

@(
    "=== Avatar bwtest @ $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ==="
    "目标        : ${Target}:${Port}"
    "包数 x 包长 : $Count x $Size B"
    "发送总量    : $([math]::Round($mb, 2)) MB"
    "耗时        : $([math]::Round($secs, 3)) s"
    "PC 侧速率   : $([math]::Round($mbs, 2)) MB/s  ($([math]::Round($mbps, 1)) Mbps)"
    "理论上限    : 95.5 Mbps / 11.4 MB/s  (1400B 载荷 @ 100 Mbps 线速)"
    "板子侧统计  : 见串口 [bwtest] 行，重点看 lost / ooo 是否为 0"
) | Tee-Object -FilePath $OutFile
