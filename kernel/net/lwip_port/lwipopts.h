#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_RAW                        1

#define LWIP_DHCP                       0
#define LWIP_DNS                        0
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define LWIP_NETIF_API                  0

#define LWIP_STATS                      0
#define LWIP_DEBUG                      0

#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   8
#define MEM_SIZE                        (64 * 1024)

#define PBUF_POOL_SIZE                  32
#define PBUF_POOL_BUFSIZE               1536

#define MEMP_NUM_PBUF                   32
#define MEMP_NUM_TCP_PCB                16
#define MEMP_NUM_TCP_PCB_LISTEN         8
#define MEMP_NUM_TCP_SEG                32
#define MEMP_NUM_NETBUF                 0
#define MEMP_NUM_NETCONN                0

#define TCP_MSS                         1460
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                16
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_LISTEN_BACKLOG              1

#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_HOSTNAME             0

#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_ICMP             1

#define LWIP_PLATFORM_BYTESWAP          1
#define LWIP_PLATFORM_HTONS(x)          __builtin_bswap16(x)
#define LWIP_PLATFORM_HTONL(x)          __builtin_bswap32(x)

#endif /* LWIPOPTS_H */
