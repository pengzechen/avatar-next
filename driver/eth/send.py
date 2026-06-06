
# 广播

# from scapy.all import Ether, Raw, sendp
# pkt = (
#     Ether(
#         dst="ff:ff:ff:ff:ff:ff",
#         src="02:00:00:00:00:01",
#         type=0x88b5,
#     )
#     / Raw(b"host-to-avatar-broadcast")
# )
# sendp(pkt, iface="tap0", count=3, inter=1, verbose=True)


# 单播
from scapy.all import Ether, Raw, sendp

pkt = (
    Ether(
        dst="52:54:00:12:34:56",
        src="02:00:00:00:00:01",
        type=0x88b5,
    )
    / Raw(b"host-to-avatar")
)

sendp(pkt, iface="tap0", count=3, inter=1, verbose=True)