Introduction
OpenPGM is an open source implementation of the Pragmatic General Multicast (PGM) specification in RFC 3208 available at www.ietf.org. PGM is a reliable and scalable multicast protocol that enables receivers to detect loss, request retransmission of lost data, or notify an application of unrecoverable loss. PGM is a receiver-reliable protocol, which means the receiver is responsible for ensuring all data is received, absolving the sender of reception responsibility. PGM runs over a best effort datagram service, currently OpenPGM uses IP multicast but could be implemented above switched fabrics such as InfiniBand.

PGM is appropriate for applications that require duplicate-free multicast data delivery from multiple sources to multiple receivers. PGM does not support acknowledged delivery, nor does it guarantee ordering of packets from multiple senders.

PGM is primarly used on internal networks to help integrate disparate systems through a common communication platform. A lack of IPv4 multicast-enabled infrastructure leads to limited capability for internet applications, IPv6 promotes multicast to be a part of the core functionality of IP but may still be disabled on core routers. Support of Source-Specific Multicast (SSM) allows for improved WAN deployment by allowing end-point router filtering of unwanted source traffic.

Supported Platforms
Current preferred platforms are RHEL, Ubuntu, Debian, FreeBSD, OS X, Solaris, and Windows. Minimum x86 platform is P5 micro-architecture for high resolution timers, 80486 is targetable with low resolution timers and atomic operations.

Operation of PGM/IP will require special privileges on Unix and Microsoft Windows Vista and above. UDP encapsulation, labeled PGM/UDP, requires no special privileges and is supported everywhere except by PGM Routers.

Tested platforms include:

RHEL 4.8 (Scons-only), 5.9, 6.4 on x64.
Ubuntu 8.04 through 13.10 on x64.
Debian 7 on x64.
FreeBSD 8.0 on x64.
OS X 10.6, 10.7, 10.9 on x64.
Solaris 10 on SPARC64.
OpenSolaris 2009.06 on x64.
Windows 7 and Server 2008R2 on x64, Windows XP SP3 on x86.
Untested platforms of note: Solaris 11, AIX, HP/UX, Windows 8, Windows Server 2012/R2, Windows RT, iOS, Android.

Protocol Compatibility
Proprietary implementations of the PGM protocol include TIBCO SmartPGM (originally by WhiteBarn, bought by Talarian), Microsoft Windows XP/2003, IBM WebSphere MQ, and RT Logic's RTPGM. Network elements that provide PGM Router Assist are available from Cisco Systems, Juniper Networks, and Nortel Networks.

OpenPGM can interoperate with Microsoft's PGM implementation found in Windows XP and newer platforms, hence called MS-PGM. Microsoft's stack is currently limited to PGM/IPv4, with no support UDP encapsulation or IPv6.

Performance
Testing has shown on gigabit Ethernet direct connection speeds of 675mb/s can be obtained with 700mb/s rate limit and 1,500 byte frames. The packet-per-second rate can be increased by reducing the frame size, performance will be limited, the actual numbers should be verified with a performance test tool such as iperf.

For reference, a Broadcom Corporation NetXtreme BCM5704S Gigabit Ethernet adapter, default 20µs driver coalescing and ~86µs best ping, reports on Linux via iperf a maximum rate of 69,000pps with minimum 12 byte frames and on Windows Server 2008 R2 via NTttcp a rate of 187,000pps, whilst gigabit line capacity is 1,488,100pps with a standard IFG of 96ns (81,274pps at full 1,500 byte IP frame).

Line capacity may be achieved with kernel bypass technologies such as Mellanox Messaging Accelerator (VMA) or Solarflare's OpenOnload which require vendor specific 10 GigE accelerated NICs.

As a placeholder a closer equivalent to NTttcp on Linux would be pktgen which "mostly runs in kernel space" and generates up to 800,000pps before being limited by IEE 802.3x PAUSE frames, disabling that is required to reach a reported 1,488,033pps.

A basic application-to-application round trip test with Ubuntu Linux on an Irwindale, single core Xeon circa 2005, has shown 100,000pps with 1KB payload packets at ~80µs latency, approximately 890mbs-1.

On Windows Server 2008 R2 on the same hardware the same application-to-application round trip test shows 30,000pps at ~90µs, approximately 250mbs-1. Win64 builds achieve a minor latency improvement over Win32 builds, throughput is unaffected. The weaker performance is partially attributable to lack of TX coalescing in Broadcom's Windows drivers which only support coalescing in newer NICs or in Linux.

It is recommended to use fewer packets of larger size with Windows platforms, by default Windows ships with a registry setting that limits to 10 non-multi-media packets-per-millisecond. Messages can be fragmented across multiple packets with a default limit of 64KB (for Microsoft DoS fix compatibility), pushing the fragmentation to the OS reduces the overheads of WinSock but increases the network load when losing individual fragments.

Consider that faster networking hardware and RDMA technologies will not resolve all problems with performance. On most modern architectures it is likely that checksum calculation will saturate CPU time at 400,000pps. Memory management of SKBs is the other significant performance point with thread caching allocators such as glibc's ptmalloc3, Google's tcmalloc, or jemalloc as found in Firefox and FreeBSD. There are architectures that can be pursued to trade allocator performance for flexibility but such schemes have limitations on modern NUMA hardware. For faster performance it is thus recommended to use multiple parallel streams with cores bound to each stream to run concurrently.

Active Branches
Important fixes are back-ported as appropriate through the following branches. * trunk: Development branch with Autotools changes for a common config.h, support for OSX 10.7, support for EkoPath compiler, remove Debian 4 support, remove pre-Wine 1.3 support, Scons wrapped Autotools, update poll/epoll decls in public headers, improve Windows network IP address support, rebase MSVC2010SP1's stdint.h, updated IP interface dump, new Windows timers: QPC, TSC, MTIMER, Windows TSC frequency read from registry. * release-5-1: The current stable branch with tiered rate limiting and introduces support for IPv6 scoped network addresses, for example ff08::1%eth0, and includes reading %SystemRoot%\System32\drivers\etc\networks on Windows or directly reading /etc/networks on Unix, bypassing NSS and hence supporting IPv6 addressing. * release-3-0: Legacy branch without GLib dependencies. Features only POSIX support as MSVC compilation too complex. * release-2-1: Legacy branch with GLib dependencies. Supports POSIX and cross compiles for MSVC via MinGW. * release-1-2-0: Legacy branch with internal threading and GLib dependencies. Only Linux support, architectural changes required for full POSIX and Win32.

Dormant Branches
New and experimental features are developed in separate branches awaiting merging with the trunk if they prove conducive. * release-2-0-recvmmsg: Support for the new non-POSIX (Linux 2.6.33) call recvmmsg() that allows passing of multiple incoming packets with one call. Increases potential transfer bandwidth at expense of latency, overall barely minimum advantage for significant coding complexity. * release-2-0-chunk-allocator: An optimised transmit window allocator that allocates chunks as multiple of the transports packet size. Increases potential transfer bandwidth at the cost of splitting memory management into separate transmit and receive compartments and additional complexity for the application developer. * release-2-0-chunk-tx-and-rx: An optimised transmit and receive window allocator that allocates chunks as multiple of the transports packet size, breaks zero-copy on rx. Increases potential transfer bandwidth further than transmit chunking alone at an increased cost of application coding limitations and complexity. * release-2-0-sendmmsg: Merge of recvmmsg & chunk-allocator branches with support for the sendmmsg() API call that allows passing of multiple outgoing packets with one call. Performance recorded at the same or worse levels than without kernel support. Note this branch is dated before sendmmsg() actually hitting Linux mainline in 3.0. * release-5-0: Stable branch with BSD socket semantics and PGMCC. PGMCC only works up to 10,000 packets-per-second and so deemed only experimental and hence an unsupported feature. * release-5-0-iocp-send: Support for Windows IOCP on original data transmissions. Yields 10% performance improvement at cost of extra threads and unbounded post queue.
