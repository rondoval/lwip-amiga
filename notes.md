# lwip-amiga notes

Conceptual phase complete 2026-07-10 — decisions and open questions captured in
[planning/concept.md](planning/concept.md). (Working title "stack-ng" → renamed
**lwip-amiga**.)

TL;DR: lwIP core (submodule + Amiga port layer), core-locking direct path,
bsdsocket.library built on the raw API with Exec signals as the blocking primitive,
clean break from SANA-II (adapter netif possible later), new batched zero-copy
`netdev` driver ABI designed for wire speed, genet.device first. IPv4+DHCP+DNS scope,
fresh minimal config, DHCP by default. GPL-2.0-or-later own code, BSD-2 ABI headers.

Refinements 2026-07-10 (round 2): lwip-amiga is a submodule of emu68-driver-stack
(standalone build = Linux host harness only; ABI header lives here, genet includes it);
RX buffers are driver-generated with a release hook (geometry/headroom/copy-break stay
driver-internal), TX pool drawn from a driver-provided DMA allocator — PCIe DMA reaches
only Emu68 expansion RAM (not Chip, not Zorro/motherboard Fast), so reachability is
strictly driver-side knowledge.

## Phase 0 status (2026-07-10) — COMPLETE

- [x] Repo scaffolded (README, LICENSE.md + LICENSES/, .gitignore, include/netdev.h placeholder)
- [x] lwIP submodule pinned to STABLE-2_2_1_RELEASE
- [x] Host harness green: `./build/harness/smoke` — 256 KB loopback TCP echo, verified byte-exact
- [x] Wired into emu68-driver-stack (components/lwip-amiga submodule + add_stack_project;
      superproject changes uncommitted, pending review)
- [x] bsdsocket.library 4.0 skeleton builds and links (m68k, zero warnings): full 139-slot
      LVO table generated from the NDK sfd (scripts/gen-vectors.py), all vectors stubbed;
      library deliberately not installed so it can't ride into driver packages, only the
      netdev ABI header installs
- [x] Bonus: unmodified lwIP core (core+ipv4+ethernet) compiles clean for m68k with
      bebbo gcc (`lwipcore_check` target) — toolchain risk retired

Verified natively (/opt/m68k-amigaos); container build not yet exercised — for fresh
clones docker-build needs `git submodule update --init --recursive` (nested lwIP).

Amiga-side conventions learned: exec calls via `__NOLIBBASE__` +
`EXEC_BASE_NAME (*(struct ExecBase **)4UL)` before `proto/exec.h` (no writable SysBase).

## Phase 1 — netdev ABI design (in progress)

- [x] include/netdev.h draft 1 (2026-07-10): NSCMD block 0x4900 w/ IOStdReq control
      ops + ATTACH exchanging contexts/ops tables/caps (xhci context-ABI idiom);
      driver-owned RX w/ cookies + prefix-consume batches both ways; offset-based TX
      L4 csum (GENET TSB shape); dual RX csum modes (VALID/RAW); declarative RX
      filter; STOP/DETACH quiesce protocol; pack(2) + layout asserts (compile-checked
      m68k via src/bsdsocket/netdev_abi_check.c). NETDEV_ABI_VERSION 1.
- [x] planning/netdev-abi.md — rationale, GENET hardware mapping (TSB/RSB/`DMA_TX_DO_CSUM`
      present but unplumbed in today's driver), open questions before freeze, genet
      Phase 1 implementation list
- [x] TCP behavior spike (harness/tcpbench, lossy echoif): window scaling carries
      256 KB windows ✓, fast retransmit absorbs 0.5% loss ✓, BUT 2% loss → RTO
      collapse (lwIP emits SACKs, never *uses* received ones). Documented ceiling,
      fine for clean wired LAN — see planning/netdev-abi.md findings.
- [x] Port layer v1 (compiles m68k clean, `netstack` static lib): real
      port/amiga/include/lwipopts.h (NO_SYS=1 + external core semaphore = the
      core-locking model with Exec primitives; heap → driver DMA allocator via
      netstack_malloc; per-netif csum ctrl), port/amiga/netstack.c (singleton, core
      lock, EClock→ms time, xorshift rand, DMA-aware heap with origin headers),
      port/amiga/netdev_if.c (full glue: TX pbuf→SG + L4 csum offsets + pseudo-seed,
      RX pbuf_custom wrap + release-hook recycle, TxDone pbuf_free, link events,
      per-netif checksum switches from caps). RAW-csum folding deferred (software
      check until measurable on genet).
- [x] ABI review (user) — netdev.h gained the pseudo-header-seed clarification
- [x] genet.device netdev conversion (2026-07-10, per user: clean cut, separate
      branches): `netdev` branch of emu68-genet-driver, v4.0, SANA-II personality
      DELETED (openers/read rings/buffer callbacks/mcast ranges/events/throughput/
      specialstats/mib/genet-stats tool/TX staging slab all removed). Zero-copy both
      directions: TX = SG descriptors straight onto the ring (CachePreDMA outside
      the Forbid window, one doorbell per batch, cookies at EOP slots), RX = pool of
      512×2048 driver buffers swapped through ring slots, handed up in batches;
      SPSC rings decouple foreign-task RxRelease + lazy-reclaim TxDone from the
      unit task (sole nso_* caller). TXDMA_DONE IRQ re-enabled (MBUF_DONE coalesced)
      as the stack's reclaim signal. v1 caps: COALESCE + LINK_EVENTS (csum offload
      = next increment: TSB/RSB regs exist but unplumbed). Builds clean, ROM check
      passes. Netdev cmake package exported by lwip-amiga (Netdev::netdev_headers);
      superproject orders lwip-amiga before genet.
- [x] netdev-test bring-up tool (2026-07-10): hosted CLI diagnostic — ATTACH
      (prints caps), netdevif_create, static-IP or DHCP config, START, 100 ms tick
      loop; lwIP answers ARP/ICMP itself so ping-from-another-host is the test.
      Ctrl-C teardown exercises STOP/DETACH; DETACH failure = RX-buffer leak check.
      Installs to C/ (pull from packaging before release). netdev.h clarified: TX
      segments may live inside same-unit RX buffers (ICMP echo reuses RX pbufs).
- [x] **HW FIRST LIGHT (2026-07-10): it just works** — netdev-test + netdev
      genet.device on real PiStorm/CM4 hardware: DHCP bound, ping answered, clean
      teardown. Zero-copy vertical slice validated on first try.
- [x] TSB/RSB checksum offload increment (2026-07-10, HW TEST PENDING):
      genet: RBUF_64B_EN + TBUF_64B_EN on (ALIGN_2B off — deterministic +64 data
      offset, unaligned reads are free here); RX parses the RSB → NDRF_CSUM_RAW
      (~rx_csum) + NDRF_CSUM_VALID from the OK bit (never on fragments); TX sends
      a per-slot 64-byte TSB as its own SOP descriptor (only tx_csum_info written;
      +1 BD per packet) with LV|start|offset|PROTO_UDP from the descriptor.
      caps += NDCF_TX_L4CSUM|RX_CSUM_RAW|RX_CSUM_VALID. netdev.h: NDTF_L4_UDP.
      glue: pseudo-seed + UDP flag on TX; RX policy = VALID passes, RAW-only
      frames fold-verified in ndif_rx_input (bad → drop + ndi_RxCsumBad; UDP
      zero-csum and fragments pass — documented gap, FCS covers the wire).
      HW-validation canaries: DHCP still binding proves TSB-UDP TX + RSB parse;
      ping proves the +64 RX offset; ndi_RxCsumBad staying 0 proves the RAW
      fold semantics. Split-TSB (own descriptor, not contiguous with data) is
      the one HW-unproven assumption — if TX dies, that's the suspect.
- [ ] HW test round 2: DHCP + ping again (offloads active), then TCP via socktest

## Phase 2 — bsdsocket.library socket layer (2026-07-10, HW TEST PENDING)

Core socket layer implemented on the lwIP raw API, builds clean (native + container):

- **Per-opener child bases**: LibOpen copies the root base (jump table included);
  each opener carries errno (+SetErrnoPtr/byte/word/long redirection), a wait
  signal bit, a 64-entry fd table, a WaitSelect timer, DNS/hostent storage,
  break mask (SBTC_BREAKMASK / SetSocketSignals). Root owns netstack, the stack
  task and the socket Exec pool (used under the core lock only).
- **Stack task in the library**: first OpenLibrary starts it — netstack init,
  genet ATTACH/START via the netdev ABI, DHCP; 100 ms lwIP tick; expunge stops it.
  No NIC → loopback-only stack, still functional.
- **Blocking = Exec signals**: lwIP callbacks (under the core lock) Signal() the
  owning task; wait pattern clears the signal under the lock, so no lost wakeups.
  Break signals (Ctrl-C by default) → EINTR with the signal re-posted.
- **Implemented LVOs (37)**: socket/bind/listen/accept/connect/send/sendto/recv/
  recvfrom/shutdown/CloseSocket/setsockopt/getsockopt/getsockname/getpeername/
  IoctlSocket(FIONBIO,FIONREAD)/WaitSelect(full autodoc semantics incl. user
  signal mask + break repost + sets-unmodified-on-error)/SetSocketSignals/
  getdtablesize/Errno/SetErrnoPtr/SocketBaseTagList(BREAKMASK,SIG*,ERRNO*,HERRNO*,
  DTABLESIZE)/Inet_NtoA/inet_addr/aton/ntop/pton/LnaOf/NetOf/MakeAddr/network/
  gethostbyname(lwIP DNS, blocking)/gethostbyaddr(no reverse yet)/gethostname/
  gethostid/getprotobyname/-number. The rest stay on LibStub (notably
  ObtainSocket/ReleaseSocket/Dup2Socket/sendmsg/recvmsg/GetSocketEvents + the
  Roadshow interface-config family).
- **TCP recv** consumes driver RX pbufs via pbuf_free_header (zero copies until
  the app buffer); **TCP send** loops tcp_write/COPY into DMA-backed heap with
  sndbuf blocking. UDP/RAW get bounded datagram queues (32) with sender address.
- lwIP 2.2 heap hooks are MEM_CUSTOM_ALLOCATOR/MEM_CUSTOM_MALLOC (the old
  mem_clib_* names are gone — gotcha).
- Link: libnix without startup code (-nostartfiles only); lwIP heap routed to
  netstack_malloc so nothing touches constructor-dependent libc state.
- **socktest** (install/C/): OpenLibrary → inet_addr/gethostbyname → TCP
  connect/send/recv via hand-rolled LVO inlines; default sends HTTP/1.0 GET.
  bsdsocket.library staged at install/Testing/ (LIBS: install only after
  on-target validation).

HW test: copy Testing/bsdsocket.library to LIBS:, then `socktest <host> 80`.
That exercises DHCP + DNS + TCP through the whole zero-copy netdev path — and
with the TSB/RSB increment active, TCP checksums ride the hardware.

Second LVO wave (same day): Dup2Socket (socket refcounting), sendmsg/recvmsg
(iovec scatter-gather; dgram side coalesces/scatters one message),
ObtainSocket/ReleaseSocket/ReleaseCopyOfSocket (root-level parking lot keyed by
id, UNIQUE_ID(-1) allocation; wakeups target the *current* owner — a shared
copy wakes only the last claimant, noted limitation), vsyslog→Kprintf via own
32-bit-varargs formatter (syslog alias shares the slot), getservbyname/-port +
static services table, netdb iterators (set/get/end for proto+serv; networks
return NULL), gethostbyname_r/gethostbyaddr_r (caller-buffer hostent),
In_LocalAddr/In_CanForward, AddDomainNameServer/RemoveDomainNameServer/
Obtain-/ReleaseDomainNameServerList (mapped to lwIP dns_setserver slots,
Roadshow node layout), Get-/SetDefaultDomainName. Plus a correctness fix:
pointer-returning unimplemented LVOs now stub to NULL (LibStubNull), not -1.

## LVO scoreboard (139 slots = 121 functions + 18 reserved)

**Implemented: 67** — the socket core (16: socket bind listen accept connect
sendto send recvfrom recv shutdown setsockopt getsockopt getsockname
getpeername IoctlSocket CloseSocket), WaitSelect + SetSocketSignals +
getdtablesize, errno family (Errno SetErrnoPtr SocketBaseTagList), inet utils
(9: Inet_NtoA inet_addr Inet_LnaOf Inet_NetOf Inet_MakeAddr inet_network
inet_aton inet_ntop inet_pton), resolver (gethostbyname gethostbyaddr
gethostbyname_r gethostbyaddr_r gethostname gethostid), netdb (13:
getprotobyname/-number getservbyname/-port getnetbyname/-addr + 9 set/get/end
iterators), Dup2Socket, sendmsg recvmsg, vsyslog(+syslog),
ObtainSocket/ReleaseSocket/ReleaseCopyOfSocket, In_LocalAddr In_CanForward,
DNS config (4) + default domain (2).

**Stubbed: 54** (LibStub -1 / LibStubNull for pointer returns), by group:
- GetSocketEvents (1) — needs per-base event queue + SBTC_SIGEVENTMASK wiring;
  next candidate, some GUI apps poll it
- getaddrinfo family (4): getaddrinfo freeaddrinfo gai_strerror getnameinfo —
  worth implementing (modern ports use it); moderate effort
- Roadshow interface-config family (10): AddInterfaceTagList
  ConfigureInterfaceTagList ObtainInterfaceList ReleaseInterfaceList
  QueryInterfaceTagList CreateAddrAllocMessageA DeleteAddrAllocMessage
  BeginInterfaceConfig AbortInterfaceConfig RemoveInterface — config UX
  decision pending (our stack self-configures via DHCP; a Query subset would
  serve status tools)
- route API (5): AddRouteTagList DeleteRouteTagList ChangeRouteTagList(priv)
  FreeRouteInfo GetRouteInfo — lwIP has no route table beyond netif+gateway
- monitor/stats (3): AddNetMonitorHookTagList RemoveNetMonitorHook
  GetNetworkStatistics — GetNetworkStatistics worth faking from lwIP stats
  eventually
- server support (2): ProcessIsServer ObtainServerSocket — inetd model;
  ObtainSocket family covers the mechanism apps actually need
- Roadshow data (3): ObtainRoadshowData ReleaseRoadshowData ChangeRoadshowData
- mbuf_* (11) — AmiTCP mbuf compat over pbufs; near-zero modern app value,
  implement only if a real app demands it
- bpf_* (8) — packet capture (tcpdump-class); would map to a promisc RAW
  netif tap; future
- ipf_* (7, ==private) — Roadshow-internal IP filter; intentionally never

Deferred smalls: reverse DNS (gethostbyaddr returns dotted quad), SO_RCVTIMEO,
static-IP env config for the stack task, shared-socket wakeup fan-out.
