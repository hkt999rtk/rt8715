/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without modification, 
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED 
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT 
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING 
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
  *
 * Author: Simon Goldschmidt
  *
  */
#ifndef LWIP_HDR_LWIPOPTS_H
#define LWIP_HDR_LWIPOPTS_H

#include <platform/platform_stdlib.h>
#include "platform_opts.h"

/* lwIP type override (required by sockets.h; avoids newlib size_t clash) */
#define SIZE_T         unsigned int

#define WIFI_LOGO_CERTIFICATION_CONFIG 0    //for ping 10k test buffer setting
    
/**
 * SYS_LIGHTWEIGHT_PROT==1: if you want inter-task protection for certain
 * critical regions during buffer allocation, deallocation and memory
 * allocation and deallocation.
 */
#define SYS_LIGHTWEIGHT_PROT    1

/* Define LWIP_COMPAT_MUTEX if the port has no mutexes and binary semaphores
 should be used instead */
#define LWIP_COMPAT_MUTEX       1
#define LWIP_COMPAT_MUTEX_ALLOWED       1

#define ETHARP_TRUST_IP_MAC     0
#define IP_REASSEMBLY           1
#define IP_FRAG                 1
#define ARP_QUEUEING            0
#define IP_FORWARD              1
#define IP_NAT                  0
#define LWIP_NETIF_API          1

#if 1   //by lzh
#define LWIP_NETBUF_RECVINFO            1
#define LWIP_NETIF_LOOPBACK             1
#define LWIP_NETIF_TX_SINGLE_PBUF		1
#define LWIP_TCP_SACK_OUT				0
#endif
/**
 * NO_SYS==1: Provides VERY minimal functionality. Otherwise,
 * use lwIP facilities.
 */
#define NO_SYS                  0

#ifndef CONFIG_DYNAMIC_TICKLESS
#define CONFIG_DYNAMIC_TICKLESS 0
#endif

/* ---------- Memory options ---------- */
/*
 * Keep lwIP pool/heap objects on a cache-line boundary.  The WLAN RX path can
 * then DMA directly into a PBUF_POOL payload without invalidating a cache line
 * shared with pbuf metadata or the next pool object.  PBUF_POOL_BUFSIZE is
 * also a multiple of 32 in the video application configuration below.
 */
#define MEM_ALIGNMENT           32

/*
 * MEM_SIZE: lwIP internal heap (ram_heap). Only effective when
 * MEM_LIBC_MALLOC == 0. CarPlay box scenario needs:
 *   - WIFI AP dual-stack (IPv4+IPv6)
 *   - NCM USB forwarding
 *   - HTTP Server + mDNS + Bonjour
 *   - DHCP Server + DNS
 * Conservative estimate: 24-32KB dynamic allocation.
 */
#if defined(CONFIG_VIDEO_APPLICATION)
    /* CarPlay mirroring: pbuf pool (250KB) lives in memp, but PBUF_RAM
     * allocations (NCM relay, HTTP body, etc.) still need ram_heap.
     * Reduced from 75KB to 32KB: the real bulk is in memp static pools. */
    #define MEM_SIZE                (360*TCP_MSS)
#elif CONFIG_ETHERNET
    /* Ethernet device, originally 6KB for iperf. CarPlay box real workload
     * (mDNS+HTTP+DHCP) needs more. */
    #define MEM_SIZE                (24*1024)
#elif defined(CONFIG_WLAN) && !defined(CONFIG_HIGH_TP_TEST)
    /* WIFI device (non-CarPlay): AP mode + DHCP + DNS + mDNS */
    #define MEM_SIZE                (16*1024)
#elif WIFI_LOGO_CERTIFICATION_CONFIG
    #define MEM_SIZE                (10*1024)
#elif defined(CONFIG_HIGH_TP_TEST)
    #define MEM_SIZE                (23*1024)
#else
    /* Default was 5KB; raised to 10KB for modern lwIP v2.1.2.
     * If RAM is extremely tight revert to 5KB, but ensure all
     * allocs go through memp pools. */
    #define MEM_SIZE                (5*1024)
#endif

/* MEMP_NUM_PBUF: the number of memp struct pbufs. If the application
   sends a lot of data out of ROM (or other static memory), this
   should be set high. */
#define MEMP_NUM_PBUF           1024
#define MEMP_NUM_NETBUF         1024    //by lzh
/* MEMP_NUM_UDP_PCB: the number of UDP protocol control blocks. One
   per active UDP "connection". */
#define MEMP_NUM_UDP_PCB        50
/* MEMP_NUM_TCP_PCB: the number of simultaneously active TCP
   connections. CarPlay box: HTTP + mDNS + Bonjour = ~6-8 concurrent */
#define MEMP_NUM_TCP_PCB        50
/* MEMP_NUM_TCP_PCB_LISTEN: the number of listening TCP
   connections. */
#define MEMP_NUM_TCP_PCB_LISTEN 50
/* MEMP_NUM_TCP_SEG: the number of simultaneously queued TCP
   segments. */
#ifdef CONFIG_HIGH_TP_TEST
    #define MEMP_NUM_TCP_SEG        60
#else
    #define MEMP_NUM_TCP_SEG        40
#endif
/* MEMP_NUM_SYS_TIMEOUT: the number of simulateously active
   timeouts. */
#define MEMP_NUM_SYS_TIMEOUT    10

#define MEMP_NUM_NETCONN        60

/* ---------- Pbuf options ---------- */
/* PBUF_POOL_SIZE: the number of buffers in the pbuf pool. */
#if WIFI_LOGO_CERTIFICATION_CONFIG
    #define PBUF_POOL_SIZE          30 //for ping 10k test
#elif defined(CONFIG_HIGH_TP_TEST)
    #define PBUF_POOL_SIZE          60
#else
    #define PBUF_POOL_SIZE          8192
#endif

/* IP_REASS_MAX_PBUFS: Total maximum amount of pbufs waiting to be reassembled.*/
#if WIFI_LOGO_CERTIFICATION_CONFIG
    #define IP_REASS_MAX_PBUFS              30 //for ping 10k test
#else
    #define IP_REASS_MAX_PBUFS              10
#endif

/* PBUF_POOL_BUFSIZE: the size of each pbuf in the pbuf pool. */
#define PBUF_POOL_BUFSIZE       500


/* ---------- TCP options ---------- */
#define LWIP_TCP                1
#define TCP_TTL                 255

/* Controls if TCP should queue segments that arrive out of
   order. Define to 0 if your device is low on memory. */
#define TCP_QUEUE_OOSEQ         1

/* TCP Maximum segment size. */
#define TCP_MSS                 (1500 - 40)	  /* TCP_MSS = (Ethernet MTU - IP header size - TCP header size) */

/* TCP sender buffer space (bytes). */
#ifdef CONFIG_HIGH_TP_TEST
    #define TCP_SND_BUF             (10*TCP_MSS)
#else
    #define TCP_SND_BUF             (8*TCP_MSS)
#endif
/*  TCP_SND_QUEUELEN: TCP sender buffer space (pbufs). This must be at least
  as much as (2 * TCP_SND_BUF/TCP_MSS) for things to work. */

#ifdef CONFIG_HIGH_TP_TEST
    #define TCP_SND_QUEUELEN        (6* TCP_SND_BUF/TCP_MSS)
#else
    #define TCP_SND_QUEUELEN        (4* TCP_SND_BUF/TCP_MSS)
#endif

/* TCP receive window. */
#ifdef CONFIG_HIGH_TP_TEST
    #define TCP_WND                 (8*TCP_MSS)
#else
    #define TCP_WND                 (6*TCP_MSS)
#endif

#if defined(CONFIG_PLATFORM_8721D)
	#define MEM_SIZE    			  (7*1024)
	#define PBUF_POOL_SIZE   	  20
	#define TCP_WND  			  (5*TCP_MSS)
#endif

/* ---------- ICMP options ---------- */
#define LWIP_ICMP                       1

/* ---------- ARP options ----------- */
#define LWIP_ARP                        1

/* ---------- DHCP options ---------- */
/* Define LWIP_DHCP to 1 if you want DHCP configuration of
   interfaces. DHCP is not implemented in lwIP 0.5.1, however, so
   turning this on does currently not work. */
#define LWIP_DHCP               1


/* ---------- UDP options ---------- */
#define LWIP_UDP                1
#define UDP_TTL                 255
/* ---------- DNS options ---------- */
#define LWIP_DNS                        1

/* ---------- UPNP options --------- */
#define LWIP_UPNP		0

/* ---------- SO_SNDRCVTIMEO_NONSTANDARD options --------- */
#define LWIP_SO_SNDRCVTIMEO_NONSTANDARD 1

/* ---------- SO_REUSE options --------- */
#define SO_REUSE                        1

/* Support Multicast */
#define LWIP_IGMP                   1
#define LWIP_RAND()                 rand()
extern unsigned int sys_now(void);
#define LWIP_SRAND()                srand(sys_now())

#define LWIP_MTU_ADJUST 		1

/* Support TCP Keepalive */
#define LWIP_TCP_KEEPALIVE				1

/*LWIP_UART_ADAPTER==1: Enable LWIP_UART_ADAPTER when CONFIG_GAGENT is enabled, 
   because some GAGENT functions denpond on the following macro definitions.*/
#define LWIP_UART_ADAPTER                   0

#if LWIP_UART_ADAPTER || CONFIG_ETHERNET
#undef  LWIP_SO_SNDTIMEO        
#define LWIP_SO_SNDTIMEO                		1

#undef  SO_REUSE        
#define SO_REUSE                        			1

#undef MEMP_NUM_NETCONN                	
#define MEMP_NUM_NETCONN                	10

#undef TCP_WND                
#define TCP_WND                                       (4*TCP_MSS)

#define TCP_KEEPIDLE_DEFAULT			10000UL
#define TCP_KEEPINTVL_DEFAULT			1000UL
#define TCP_KEEPCNT_DEFAULT			10U
#endif

#if CONFIG_EXAMPLE_UART_ATCMD || CONFIG_EXAMPLE_SPI_ATCMD 
#undef  LWIP_SO_SNDTIMEO        
#define LWIP_SO_SNDTIMEO                		1

#undef  SO_REUSE        
#define SO_REUSE                        			1

#undef SO_REUSE_RXTOALL
#define SO_REUSE_RXTOALL				1

#undef MEMP_NUM_NETCONN
#define MEMP_NUM_NETCONN                	10

#undef MEMP_NUM_TCP_PCB
#define MEMP_NUM_TCP_PCB				(MEMP_NUM_NETCONN)

#undef MEMP_NUM_UDP_PCB
#define MEMP_NUM_UDP_PCB				(MEMP_NUM_NETCONN)

#undef TCP_WND                
#define TCP_WND                                       	(4*TCP_MSS)

#define TCP_KEEPIDLE_DEFAULT			10000UL
#define TCP_KEEPINTVL_DEFAULT			1000UL
#define TCP_KEEPCNT_DEFAULT			10U

#define ERRNO   1

#endif

#if CONFIG_EXAMPLE_AMAZON_ALEXA

#undef TCP_WND                
#define TCP_WND                                       	(4*TCP_MSS)

#undef TCP_SND_BUF
#define TCP_SND_BUF             (10*TCP_MSS)

#undef TCP_SND_QUEUELEN
#define TCP_SND_QUEUELEN        (4* TCP_SND_BUF/TCP_MSS)

#undef MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG        40

#endif

#if defined(CONFIG_VIDEO_APPLICATION)
#undef	LWIP_WND_SCALE
#define	LWIP_WND_SCALE                  1

#undef	TCP_RCV_SCALE
#define	TCP_RCV_SCALE                   2

#undef MEM_SIZE
#define MEM_SIZE (32*1024)

/*
 * Keep a normal Ethernet/VLAN frame in one DRAM-backed pool pbuf.  The old
 * 500-byte setting chained a 1500-byte frame across 3-4 pbufs, adding extra
 * allocation/free, scatter-copy and checksum traversal on every RX packet.
 * 1600 is cache-line aligned and leaves headroom above a tagged frame.
 */
#undef PBUF_POOL_BUFSIZE
#define PBUF_POOL_BUFSIZE 1600

/*
 * PBUF_POOL_SIZE: total RX frames.  Keep the existing 1000-entry burst
 * capacity; unlike the previous 500-byte buffers, one entry now normally
 * represents one complete frame.  The pool is allocated from DRAM.
 */
#undef PBUF_POOL_SIZE
#define PBUF_POOL_SIZE 1000

/*
 * Keep large WLAN RX frames in the driver's skb storage until lwIP releases
 * them.  This removes the skb-to-pbuf payload copy.  The fixed wrapper limit
 * prevents delayed TCP consumers from exhausting the WLAN skb data pool;
 * allocation/clone failures transparently use the existing copy path.
 */
#define CONFIG_WLAN_RX_ZERO_COPY       1
#define WLAN_RX_ZERO_COPY_MIN_LEN      1024
#define WLAN_RX_ZERO_COPY_POOL_SIZE    256
#define WLAN_RX_ZERO_COPY_STATS        0
#define WLAN_RX_ZERO_COPY_SELFTEST     1
#define WLAN_RX_ZERO_COPY_DRAIN_TIMEOUT_MS 5000

#undef MEMP_NUM_NETBUF
#define MEMP_NUM_NETBUF 80

//#undef DEFAULT_UDP_RECVMBOX_SIZE
//#define DEFAULT_UDP_RECVMBOX_SIZE 60

#undef IP_REASS_MAX_PBUFS
#define IP_REASS_MAX_PBUFS 40

/*
 * TCP_SND_BUF: per-connection send buffer. Raised to 65535*4 (~262KB)
 * matching the known-working COM3 configuration for CarPlay reliability.
 */
#undef TCP_SND_BUF
#define TCP_SND_BUF (65535 * 4)

/* TCP_SNDLOWAT: must be < u16_t overflow (0xFFFF - 4*MSS ≈ 59695).
 * 65535/2 = 32767 matches COM3 known-working config. */
#define TCP_SNDLOWAT (65535 / 2)

#undef TCP_SND_QUEUELEN
#define TCP_SND_QUEUELEN (12*TCP_SND_BUF/TCP_MSS)

#undef MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG TCP_SND_QUEUELEN

/*
 * TCP_WND: per-connection receive window.  Board profiling observed short
 * bursts reaching about 80% of the former 128-KiB window.  Keep twice that
 * headroom so those bursts do not throttle the sender.  TCP_RCV_SCALE must
 * be 2: a scale of 1 can advertise at most 65535*2 bytes on the wire.
 */
#undef TCP_WND
#define TCP_WND (65535 * 4)

/*
 * CONFIG_ETHERNET lowered MEMP_NUM_NETCONN to 10 above;
 * CarPlay mirroring (HTTP + mDNS + Bonjour + AirPlay) needs more.
 */
#undef MEMP_NUM_NETCONN
#define MEMP_NUM_NETCONN        60

#endif
/* --------------- end of CONFIG_VIDEO_APPLICATION --------------- */

/* ---------- Statistics options ---------- */
#define LWIP_STATS 0
#define LWIP_PROVIDE_ERRNO 1
// #define LWIP_ERRNO_STDINCLUDE

/*
 * The Cortex-M33 build keeps lwIP's Internet checksum in ITCM. Algorithm 4
 * retains algorithm 3's byte-exact handling of odd addresses and lengths, but
 * its aligned hot loop uses inline Thumb-2 LDMIA/ADDS/ADCS instructions to
 * consume four 32-bit words per iteration without a carry-test branch for
 * every word. Algorithm 3 remains the portable reference/fallback in
 * inet_chksum.c; no separate assembly source is required by the build.
 */
#if defined(CONFIG_VIDEO_APPLICATION)
#define LWIP_CHKSUM_ALGORITHM            4
#endif

/*
   --------------------------------------
   ---------- Checksum options ----------
   --------------------------------------
*/

/* 
Certain platform allows computing and verifying the IP, UDP, TCP and ICMP checksums by hardware:
 - To use this feature let the following define uncommented.
 - To disable it and process by CPU comment the  the checksum.
*/
//Do checksum by lwip - WLAN nic does not support Checksum offload
//#define CHECKSUM_BY_HARDWARE 


#ifdef CHECKSUM_BY_HARDWARE
  /* CHECKSUM_GEN_IP==0: Generate checksums by hardware for outgoing IP packets.*/
  #define CHECKSUM_GEN_IP                 0
  /* CHECKSUM_GEN_UDP==0: Generate checksums by hardware for outgoing UDP packets.*/
  #define CHECKSUM_GEN_UDP                0
  /* CHECKSUM_GEN_TCP==0: Generate checksums by hardware for outgoing TCP packets.*/
  #define CHECKSUM_GEN_TCP                0 
  /* CHECKSUM_CHECK_IP==0: Check checksums by hardware for incoming IP packets.*/
  #define CHECKSUM_CHECK_IP               0
  /* CHECKSUM_CHECK_UDP==0: Check checksums by hardware for incoming UDP packets.*/
  #define CHECKSUM_CHECK_UDP              0
  /* CHECKSUM_CHECK_TCP==0: Check checksums by hardware for incoming TCP packets.*/
  #define CHECKSUM_CHECK_TCP              0
#else
  /* CHECKSUM_GEN_IP==1: Generate checksums in software for outgoing IP packets.*/
  #define CHECKSUM_GEN_IP                 1
  /* CHECKSUM_GEN_UDP==1: Generate checksums in software for outgoing UDP packets.*/
  #define CHECKSUM_GEN_UDP                1
  /* CHECKSUM_GEN_TCP==1: Generate checksums in software for outgoing TCP packets.*/
  #define CHECKSUM_GEN_TCP                1
  /* CHECKSUM_CHECK_IP==1: Check checksums in software for incoming IP packets.*/
  #define CHECKSUM_CHECK_IP               1
  /* CHECKSUM_CHECK_UDP==1: Check checksums in software for incoming UDP packets.*/
  #define CHECKSUM_CHECK_UDP              1
  /* CHECKSUM_CHECK_TCP==1: Check checksums in software for incoming TCP packets.*/
  #define CHECKSUM_CHECK_TCP              1
#endif


/*
   ----------------------------------------------
   ---------- Sequential layer options ----------
   ----------------------------------------------
*/
/**
 * LWIP_NETCONN==1: Enable Netconn API (require to use api_lib.c)
 */
#define LWIP_NETCONN                    1

/*
   ------------------------------------
   ---------- Socket options ----------
   ------------------------------------
*/
/**
 * LWIP_SOCKET==1: Enable Socket API (require to use sockets.c)
 */
#define LWIP_SOCKET                     1

/*
   -----------------------------------
   ---------- DEBUG options ----------
   -----------------------------------
*/

#define LWIP_DEBUG                      0


/*
   ---------------------------------
   ---------- OS options ----------
   ---------------------------------
*/

#define TCPIP_THREAD_STACKSIZE			1000
#define TCPIP_MBOX_SIZE					6
#define DEFAULT_UDP_RECVMBOX_SIZE		6
#define DEFAULT_TCP_RECVMBOX_SIZE		6
#define DEFAULT_RAW_RECVMBOX_SIZE		6
#define DEFAULT_ACCEPTMBOX_SIZE			6
#define DEFAULT_THREAD_STACKSIZE		500
#define TCPIP_THREAD_PRIO				(configMAX_PRIORITIES - 2)

#if defined(CONFIG_VIDEO_APPLICATION)
#undef TCPIP_MBOX_SIZE
#define TCPIP_MBOX_SIZE					600
#undef DEFAULT_UDP_RECVMBOX_SIZE
#define DEFAULT_UDP_RECVMBOX_SIZE		600
#undef DEFAULT_TCP_RECVMBOX_SIZE
#define DEFAULT_TCP_RECVMBOX_SIZE		600
#undef DEFAULT_RAW_RECVMBOX_SIZE
#define DEFAULT_RAW_RECVMBOX_SIZE		600
#undef DEFAULT_ACCEPTMBOX_SIZE
#define DEFAULT_ACCEPTMBOX_SIZE			100 //6
#undef MEMP_NUM_TCPIP_MSG_INPKT
#define MEMP_NUM_TCPIP_MSG_INPKT		600
#endif

/* Added by Realtek. For DHCP server reply unicast DHCP packets before the ip actually assigned. */
#define ETHARP_SUPPORT_STATIC_ENTRIES   1

/* Added by Realtek start */
#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1
#define LWIP_DNS_LEGACY_SUPPORT 0
/* Added by Realtek end */

/* Extra options for lwip_v2.0.2 which should not affect lwip_v1.4.1 */
#define LWIP_TCPIP_CORE_LOCKING         1
#define LWIP_TCPIP_TIMEOUT              1
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_SO_RCVBUF                  1
#define LWIP_SOCKET_SET_ERRNO           0
#undef LWIP_DEBUG
#define LWIP_RAW                        1
#define LWIP_AUTOIP                     1
#define TCPIP_THREAD_NAME              "TCP_IP"
#define LWIP_HTTPD                      0
#define LWIP_HTTPD_CGI                  1
#define LWIP_HTTPD_SSI                  1
#define LWIP_HTTPD_SUPPORT_POST         1
#define LWIP_HTTPD_TIMING               1
#define LWIP_HTTPD_DYNAMIC_HEADERS      1
#define LWIP_HTTPD_CUSTOM_FILES         1
#define LWIP_HTTPD_DYNAMIC_FILE_READ    1 

/*
 * MEM_LIBC_MALLOC: when 1, lwIP uses libc malloc()/free() = FreeRTOS heap.
 * This means MEM_SIZE's ram_heap is NOT allocated and MEM_SIZE is ignored.
 *
 * When MEM_LIBC_MALLOC=1, all lwIP dynamic allocations (pbuf/netconn/PCB)
 * compete with the rest of the system on FreeRTOS heap. If FreeRTOS heap
 * is exhausted (Mem 0.00M symptom), lwIP connections silently fail.
 *
 * Set to 0 to isolate lwIP onto its own ram_heap (sized by MEM_SIZE above),
 * with memp pools continuing to use static pre-allocated arrays.
 *
 * CAREFUL: with MEM_LIBC_MALLOC=0, mem_malloc() returns NULL on exhaustion
 * instead of trying to borrow from OS heap. Ensure MEM_SIZE is sufficient.
 */
#define MEM_LIBC_MALLOC                 1

#define LWIP_IPV6                       1
#if LWIP_IPV6
#define LWIP_IPV6_MLD                   1
#define LWIP_IPV6_AUTOCONFIG            1
#define LWIP_ICMP6                      1
#undef  MEMP_NUM_SYS_TIMEOUT
#define MEMP_NUM_SYS_TIMEOUT            13
#define IPV6_FRAG_COPYHEADER            1
#define LWIP_IPV6_DHCP6                 1

/* mDNS responder and multicast listener discovery */
#define LWIP_MDNS_RESPONDER             1
#undef  LWIP_NUM_NETIF_CLIENT_DATA
#define LWIP_NUM_NETIF_CLIENT_DATA      3
#define MDNS_RESP_USENETIF_EXTCALLBACK  1
#define MDNS_MAX_SERVICES               10
#define MEMP_NUM_MLD6_GROUP             20
#define LWIP_NETIF_EXT_STATUS_CALLBACK  1
#endif
     
#include "lwip/init.h"                  //for version control

#endif /* LWIP_HDR_LWIPOPTS_H */
