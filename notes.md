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

Third LVO wave (2026-07-14): **SO_SNDTIMEO/SO_RCVTIMEO** — per-socket ms
deadlines honored in recv/send blocking loops via sb_wait_to() (arms the
opener's WaitSelect timer for the *remaining* time each iteration and fully
reclaims it — the deadline math, not the timer request, is authoritative;
timeout => partial count if any, else EWOULDBLOCK; get/set as struct
timeval). **getaddrinfo/freeaddrinfo/gai_strerror/getnameinfo** (sb_gai.c):
RFC 2553 IPv4 subset on the netinclude/netdb.h ABI — socktype fan-out
(STREAM+DGRAM when unspecified), numeric + services-table service lookup,
AI_PASSIVE/AI_NUMERICHOST/AI_CANONNAME (canonname = queried name, no CNAME
chase), whole result list in ONE AllocMem block behind a hidden size header
(freeaddrinfo takes the list head); getnameinfo is numeric-host only (no
reverse DNS; NI_NAMEREQD => EAI_NONAME) + services table for names.
Drive-by fix: WaitSelect's abort paths now clear the stale timer-port
signal after AbortIO/WaitIO (was latent — a leftover signal turns the next
timed wait into an instant timeout; sb_wait_to shares that port). Builds
clean, HW test pending.

## LVO scoreboard (139 slots = 121 functions + 18 reserved)

**Implemented: 72** — the socket core (16: socket bind listen accept connect
sendto send recvfrom recv shutdown setsockopt getsockopt getsockname
getpeername IoctlSocket CloseSocket), WaitSelect + SetSocketSignals +
getdtablesize, errno family (Errno SetErrnoPtr SocketBaseTagList), inet utils
(9: Inet_NtoA inet_addr Inet_LnaOf Inet_NetOf Inet_MakeAddr inet_network
inet_aton inet_ntop inet_pton), resolver (gethostbyname gethostbyaddr
gethostbyname_r gethostbyaddr_r gethostname gethostid), netdb (13:
getprotobyname/-number getservbyname/-port getnetbyname/-addr + 9 set/get/end
iterators), Dup2Socket, sendmsg recvmsg, vsyslog(+syslog),
ObtainSocket/ReleaseSocket/ReleaseCopyOfSocket, In_LocalAddr In_CanForward,
DNS config (4) + default domain (2), getaddrinfo family (4: getaddrinfo
freeaddrinfo gai_strerror getnameinfo), GetSocketEvents + SO_EVENTMASK +
SBTC_SIGEVENTMASK (full AmiTCP V4 async event API).

**Stubbed: 49** (LibStub -1 / LibStubNull for pointer returns), by group:
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

Deferred smalls: reverse DNS (gethostbyaddr and getnameinfo return dotted
quads), static-IP env config for the stack task, shared-socket wakeup
fan-out.

## Open investigations

**Upload-test wedge (2026-07-14) — OPEN, diagnostics in place.** With the
pri-10 + SO_*TIMEO + gai build: Amelinium works, speedtest download ~215
Mb/s (>= Roadshow's ~200), but upload wedges after its second round. The
second round's impossible reading (1200 kB @ 179.9 Mb/s on a ~70 Mb/s
uplink) is app-side accounting meeting our 256 KB TCP_SND_BUF (send()
returns when data enters the send buffer; likely multi-connection too) —
not a bug by itself. The wedge is real and this is the FIRST sustained
heavy-TX workload the netdev path has seen: download only exercises
~54-byte ACK frames on TX; upload sends full-size multi-BD frames
(TSB + header pbuf + payload = 3 BDs/frame) at ring capacity. Desk audit
of tx submit/reclaim (u8 ring pointers wrap 256 ✓, TSB slot keying ✓,
free-BD check vs TX_RECLAIM_THRESHOLD ✓, cookie refcounts balance on
rexmit-while-on-ring ✓, DMA pool auto-grows ✓, TxDone flush-on-any-wake ✓)
found nothing conclusive. Added for the next run: `ndTxRejected` counter +
debug `txh:` line at the mib 200 ms cadence — pkt+/rej+ deltas, out= (BDs
on ring), free=, hwcons=, done= (TxDone ring depth). Read it as:
- wedged with out= pinned high, hwcons frozen -> TDMA halted (descriptor
  bug; prime suspect = the multi-BD/TSB path under load);
- wedged with out=0, pkt+0 -> stack-side stall (lwIP/bsdsocket wait);
- rej+ climbing, hwcons moving -> ring pressure only (throughput, no bug);
- pcap showing same-seq retransmits the server never ACKs -> frames
  corrupted on the wire (TX csum offload under multi-BD suspect).
First debug-HIGH run (log 01.14.50): upload COMPLETED (per-packet KprintfH
throttled TX to ~590 pps — no ring pressure: rej 0 everywhere, out<=2,
zero MAC/RX error counters, no drops/asserts; whole log clean). The wedge
is rate-dependent — repro needs near-full-speed TX. Right config:
EMU68_DEBUG_HIGH=OFF but DEBUG on (keeps mib:/txh: lines, drops per-packet
prints) + switch pcap.

**WEDGE ANALYSIS (2026-07-14, log 01.39.25 + lan2.pcap) — corruption
mechanism proven, first-cause instrumented, root cause still open.**
txh proved the TX ring innocent: 120-220 Mb/s with rej+0/out=0 right up to
the stop — then `[lwip] ASSERT: mss_local is too small` (tcp_write,
tcp_out.c:489) followed by a wild 2-byte write (Z3 far ffffff08) and total
TX silence. Our LWIP_PLATFORM_ASSERT RETURNED, so tcp_write's next line's
u16 subtraction underflowed (space ~65K) → monster segment → header writes
through a trashed pointer → stack wedged. Non-debug builds asserted
SILENTLY — same wedge.

Wire facts (lan2.pcap, upload = parallel streams to 94.124.4.42:8080):
SYN-ACK window 62720/wscale 7 (NOT small), server MSS option 1412 (< our
1460 — real), streams 62901/902/903 uploaded and closed cleanly, 62905
died MID-FLOW: 26 MB in 3.8 s, retrans storm at 01:40:34.62, server
dup-ACKing with a healthy ~1.1 MB window, then the amiga never transmitted
again (no FIN/RST). So the assert hit an ESTABLISHED flow with
mss_local pinned at 1412 — meaning last_unsent->len somehow EXCEEDED mss,
which every static path forbids (rexmit/persist/oversize bookkeeping all
audited clean; snd_wnd_max monotone). Something subtler (or prior silent
corruption) — hence instrumentation, not another theory.

Second wedge run (2026-07-14 20:54 log): corruption WITHOUT the tcp_write
assert — upload at full rate, then one wild 2-byte write (far ff000008,
value 0x2A2) and TOTAL silence (even genet's timer prints stop = hard
system-wide corruption). No TCPGUARD/assert print — either the corrupter
bypassed tcp_write this time (assert was a symptom, not the source) or the
deployed library predates the 08:30 TCPGUARD build (AExplorer still
failing hints the same; the new build banner below removes deployment
ambiguity forever). The wild-write values (0x018E, 0x02A2) look like
pseudo-header sums = ndif_l4_offsets' 2-byte checksum seed going through a
trashed pbuf payload — a downstream symptom; the first corrupter is still
at large. Escalated instrumentation (see below): heap canaries + poison +
TX pbuf gate.

Third round (2026-07-14 21:37 + 21:49): the fully-instrumented DEBUG build
passed BOTH tests clean — zero guard hits, so the corruption simply didn't
occur (layout/timing Heisenbug; poison-on-free and canaries shift the
heap). The NON-debug build then CRASHED ~18 s into upload with Emu68's
first-ever precise decode: unhandled ARM page fault in the CachePreDMA
cache-clean loop (`dc cvac, x5`) on VA 0xF000D000, len 0x218 (536) —
i.e. genet TX submit priming a SEGMENT WITH A GARBAGE PAYLOAD POINTER,
third segment of a chain (regs suggest a clobbered pbuf->next). Same
corrupted-chain family as the wild 2-byte writes. The debug-only pbuf gate
would have intercepted exactly this — hence release-only crash.
Consequences (built clean):
- genet TX submit phase 0 (RELEASE-SAFE): every SG segment validated with
  dma_addr_reachable() + length bounds BEFORE CachePreDMA; a garbage head
  descriptor is consumed-and-completed as dropped (ndTxBad counter, txh
  `bad=` field) — a lost frame instead of a dead machine. Also hardens the
  netdev ABI contract for any future stack.
- lwipopts (DEBUG): MEMP_OVERFLOW_CHECK 1 + MEMP_SANITY_CHECK 1 — tcp_seg/
  pcb pools are static arrays OUTSIDE the guarded netstack heap; now lwIP
  polices them too.
Guard tiering (same day): the layout-changing instrumentation (heap
canaries/poison, memp checks) moved to the DEBUG_HIGH tier — plain DEBUG
builds are now layout-faithful to the reproducing configuration while
keeping every non-layout probe (pbuf gate, TCPGUARD, halting asserts,
banner, txh). Repro config = BACKEND=pistorm, DEBUG_HIGH=OFF; forensics
config = DEBUG_HIGH=ON (slow, guards armed).
**CORRUPTION MECHANISM IDENTIFIED (2026-07-14 22:25 run).** The halting
assert caught it upstream with zero damage: `[lwip] ASSERT: inconsistent
oversize vs. space — task 'AmiSpeedTest subprocess' halted` (tcp_write
phase 1). `pcb->unsent_oversize` goes STALE-HIGH — and phase 1 trusts it
to append new data into the last unsent segment's pbuf tail, so a stale
value writes PAST the real allocation into the neighboring heap block.
Every observed symptom is downstream of that overflow: clobbered
pbuf->next / payload pointers -> wild 2-byte csum-seed writes, the
CachePreDMA crash, the "mss_local is too small" assert on mangled fields.
lwIP's assert only catches EGREGIOUS staleness (ovz > space); milder
staleness (ovz <= space but > the pbuf's real room) overflowed SILENTLY —
which is why most corruption runs showed no assert. The bookkeeping
desync's root cause inside lwIP (rto/rexmit/split/enqueue interplay, all
tail-update paths audited and individually correct-looking) is still to
be pinned — but the class is now closed:
- TCPGUARD-OVZ in sb_tcp_send (INTERIM, all builds): unsent_oversize is
  clamped to 0 before every tcp_write — phase-1 tail reuse disabled (only
  small-write coalescing is lost; bulk streams build full segments in
  phase 3 anyway). When the clamped value actually violated the invariant,
  the full pcb/segment state is printed (evidence stream for the root
  cause + upstream lwIP report).
Note: this state was reachable ONLY under sustained loss/rexmit pressure
(upload into a 70 Mb/s uplink at 200+ Mb/s), matching all failures being
upload-side. Upstream report material: reproducible stale unsent_oversize
with TCP_OVERSIZE=TCP_MSS + heavy RTO on lwIP 2.2.1.

Clamp v1 gotcha (2026-07-14 23:33 run, "always asserts on upload"): DEBUG
builds define LWIP_DEBUG -> TCP_OVERSIZE_DBGCHECK=1, which keeps a SHADOW
copy of the counter in the segment (last_unsent->oversize_left) and
asserts pcb==shadow at every tcp_write entry (tcp_out.c "unsent_oversize
mismatch"). Zeroing only the pcb side tripped it deterministically on the
first oversize write. Fixed: the clamp zeroes the shadow in lockstep
(#if TCP_OVERSIZE_DBGCHECK), and lwIP's mismatch check is re-implemented
in the guard as a non-halting TCPGUARD-DESYNC evidence print — a
pcb-vs-shadow mismatch is precisely the one-sided bookkeeping bug being
hunted, so DEBUG builds now log it instead of halting. Also added
TCPGUARD-OVZ0: unsent==NULL with nonzero unsent_oversize (unambiguous
staleness; would halt tcp_write's "pcb->unsent is NULL" assert) is
printed and clamped too.

Hardening + instrumentation applied (all built clean):
- netstack_platform_diag latches the message (netstack_assert_msg) and
  freezes the calling task — lwIP requires a non-returning assert handler;
  any core assert is now a deterministic logged stall, never corruption.
- sb_tcp_send gates on s->connecting (blocking send waits for established
  honoring SO_SNDTIMEO; non-blocking gets EWOULDBLOCK). Matches
  sb_sock_writable. Independently valuable AND closes the write-in-
  SYN_SENT hazard class for real: pcb->mss starts at 1460 and SHRINKS to
  the peer's MSS option (1412 here) at SYN-ACK, and lwIP also RESETS
  snd_wnd_max to the raw handshake window then — either would trip the
  assert on pre-queued full-MSS data. Upstream lwIP report candidates.
- TCPGUARD (sb_tcp_send, marked TEMP): re-derives tcp_write's mss_local
  check against the last unsent segment BEFORE calling tcp_write; on
  violation prints the full pcb state (mss/swm/swnd/len/flags/state/qlen/
  sndbuf/oversize) and waits it out like a full send buffer instead of
  corrupting. Next repro either prints the exact numbers or just works.
- Heap guards in netstack_malloc/free (DEBUG builds): tail canary catches
  overruns, poison-on-free (0xDD fill) makes use-after-free loud and
  catches double-free via the poisoned origin; guard hits print and freeze
  the caller. DMA reads of freed TX buffers now put 0xDD on the wire —
  greppable in a pcap.
- TX pbuf sanity gate in ndif_linkoutput (DEBUG builds): implausible
  payload pointers/lengths are logged and dropped BEFORE the checksum-seed
  write — prevents the wild 2-byte store and pinpoints the moment lwIP
  hands over a corrupted chain.
- Build banner: "[bsdsocket] stack task up, build <date time>" at stack
  start — every log now identifies its exact binary.

**RX ring overrun (2026-07-14, log 01.39.25): `RDMA discarded 711
frame(s)` during the download phase — the 256-slot RX ring outran the unit
task at ~215 Mb/s. Non-fatal (TCP recovers) but it costs goodput; perf
pass candidates: bigger BUDGET, RX ring/pool sizing, coalescing tuning.**

**AExplorer round 2 (2026-07-14 21:17 log, DEBUG_HIGH): the REAL blocker
was the event API.** With SIGIO delivery in place the trace advanced to:
accept OK -> SocketBaseTagList(SIGEVENTMASK, now accepted) ->
`setsockopt(fd, SOL_SOCKET, SO_EVENTMASK=0x2001)` -> ENOPROTOOPT ->
"Error initializing socket connection" -> teardown. IMPLEMENTED the full
AmiTCP V4 event API (build clean, HW test pending): SO_EVENTMASK get/set
(per-socket FD_* subscription, libraries/bsdsocket.h bits ACCEPT 0x01 ..
CLOSE 0x40; setting the mask synthesizes events from CURRENT state — data
can predate the subscription, AExplorer's exact sequence), sb_event()
recorder wired into all callbacks (recv=READ/CLOSE, sent=WRITE,
connected=CONNECT, err=ERROR|CLOSE, accept=ACCEPT, dgram=READ) signalling
sigEventMask, and GetSocketEvents (fd-order scan, drains per socket,
listener with queued connections keeps FD_ACCEPT pending per autodoc).
LVO count: 72 implemented / 49 stubbed.

**AExplorer upload failure (2026-07-14, earlier same day) — first fix.**
AmiTCP-style async SIGIO was stored but never delivered: AExplorer accepts
the PC's connection (request bytes already queued), closes its listener,
arms async notification via SocketBaseTagList, then parks in Wait() — we
never Signal()ed sigIoMask, so it sat 3.6 s, hit its own timeout, reported
a generic "couldn't create socket" and tore down (log: accept 01:16:28.9,
silent until close 01:16:32.6). Also SBTC_SIGEVENTMASK was an unknown tag
(TagList call FAILED with its index). Fixed: sb_wake now delivers
(sigBit | sigIoMask | sigEventMask); arming a non-zero mask (TagList or
SetSocketSignals) delivers one immediate signal so readiness that predates
the mask isn't lost (AExplorer's exact sequence); SBTC_SIGEVENTMASK is
stored+delivered (real GetSocketEvents queue still pending). Builds clean,
HW retest pending.

**Whole-stack ~1 s freeze at the core-lock layer (2026-07-13 evening) — ROOT
CAUSE of the amispeedtest failure; exec/Emu68-level, our code audited clean.**
Evidence: pistorm log 22.03.54 + switch pcap (both in stack root), timestamps
offset ~230 ms (serial behind pcap).

- amispeedtest conn #1 (server list, 159.69.45.92:80) works end-to-end in
  ~250 ms. Conn #2: SYN out 54.449, SYN-ACK **at the NIC** 20 ms later
  (54.469, genet logged `rx: … tcpfl 0x12`) — but lwIP only processed it at
  55.46, 991 ms late; the app's 1 s WaitSelect expired just before, it closed
  the socket and aborted. The server/network were fine; the stall is local.
- During the stall the WHOLE stack froze: the genet unit task stuck between
  its `rx:` log and `ndif_rx_input`'s `netstack_lock()` (RX IRQ stayed
  masked, ring accumulated 10 frames per mib), the bsdsocket stack task
  missed every 100 ms tick (its overdue slowtmr SYN-rexmit hit the wire at
  55.4609 — pcb still SYN_SENT, proving the tick ran *before* the SYN-ACK
  input), genet ~200 ms housekeeping (mib) silent. The only on-time event was
  the app's own timer.device 1 s request; within ~20 ms of it firing
  everything resumed in sequence stack-tick → unit-RX → app-close (wire:
  rexmit 55.4609, handshake-ACK 55.4636, FIN 55.4663).
- Onset: the session's first *contended* ns_Core handoff burst — the unit
  task's nso_RxInput queued behind the app's WaitSelect scan (whose debug
  prints stretch the hold to ~30 ms); that handoff succeeded, and the
  unit task's immediately-following Obtain (next RX round, ~1 ms later,
  semaphore free — the app re-acquired it instantly at its timeout) hung
  ~978 ms. Both pri-5 tasks slept on a free semaphore for ~1 s.
- m68k-side code audited clean: WaitSelect/sb_wait use the correct
  clear-signal-under-lock pattern, every LVO traversed is lock-balanced,
  the genet RX path has no other blocking point, Kprintf is a synchronous
  per-byte trap (log order = execution order). Priorities: app 0, stack
  task 5, unit task 5.
- **LEADING THEORY (revised same evening): Executive priority starvation.**
  The system runs Executive (dynamic scheduler); tasks at pri <= 5 are inside
  its managed band and CPU-users get demoted — semstress's "pri 5" roles were
  observed running at pri 1. So the stack task and genet unit task (both
  static pri 5) are dynamically demoted, and during the freeze they weren't
  un-dispatchable — they were READY but **starved by a CPU-burning task**:
  almost certainly amispeedtest's main task busy-polling its subprocess (the
  9 Emu68 JIT `SUBI.B` diagnostics 34 ms before the freeze = a countdown
  spin loop being compiled). A spin-then-nap poller keeps scoring
  "interactive" under decay scheduling while the quiet driver tasks sink.
  Fits everything: freeze onset right after conn #1's RX/TX burst (driver
  tasks maximally decayed), the app's timer-wake running FIRST (fresh
  sleeper gets boosted), driver tasks resuming ~20 ms later (aging /
  rebalance ~1 s), and semstress running clean for 500+ s on the affected
  Emu68 (no hog in the mix, and its roles all flattened to pri 1 anyway —
  which also says the semaphore primitive itself is healthy). The earlier
  exec/Emu68-JIT lost-wakeup theory is demoted to fallback.
- Repro tool: **semstress** (src/semstress, installs to C/) — recreates the
  app/unit/tick topology around one SignalSemaphore, times every
  ObtainSemaphore, monitor reports "!!!" lines (>THRESH ms) + wedge states +
  **live ln_Pri per role** (Executive demotion visible). Executive-repro
  knobs: `HOG 1` spawns a pri-0 spin-then-nap CPU burner (HOGBUSY/HOGIDLE,
  default 40/20 ms), `PRI n` sets unit/tick static priority.
  Test matrix under Executive: `PRI 5 HOG 1` → expect "!!!" stalls;
  `PRI 6` (or 10) `HOG 1` → expect clean; Executive disabled + `PRI 5
  HOG 1` → expect clean (plain exec: pri 5 always beats a pri 0 hog).
  Cleanest end-to-end confirm needs no code at all: disable Executive,
  rerun amispeedtest debug-off.
- Fix APPLIED 2026-07-14 (HW test pending): priority audit found exactly
  the two network tasks inside the band (nvme unit = 10, xhci unit = 30
  were already safe) — genet DEFAULT_UNIT_TASK_PRIORITY and the bsdsocket
  stack task are now 10. ENV:genet.prefs UNIT_TASK_PRIORITY still
  overrides genet's. Residual
  hazard to keep in mind: ns_Core is held at the CALLER's priority during
  every LVO, so a managed-band app holding it can still be starved
  (classic inversion, no priority inheritance in exec) — hold times are
  µs with debug off; if it ever bites, a SetTaskPri priority-ceiling in
  netstack_lock/unlock is the known workaround.

**Same-session API findings (2026-07-13):**
- `setsockopt(SOL_SOCKET, SO_SNDTIMEO/SO_RCVTIMEO)` (0x1005/0x1006) →
  ENOPROTOOPT (errno 42) — AmiKit Live Update sets both every connection.
  IMPLEMENTED 2026-07-14 (see third LVO wave above). SO_ERROR getsockopt
  was already fine (non-blocking connect + WaitSelect validated end-to-end
  by conn #1).
- Amelinium was not in this log; the stubbed getaddrinfo family it likely
  needs is IMPLEMENTED 2026-07-14 — retest Amelinium with the new library
  + priorities before digging further.

**Router-side first-packet drop (2026-07-13 morning) — RE-EXAMINE.**
Original evidence: first inbound packet of a fresh through-NAT flow after
TX-idle lost upstream of the NIC (MIB uc+0, zero FCS/RBUF/RDMA errors),
server's +1 s SYN-ACK retransmit arrives fine. NEW datapoint (evening): the
repeat offender 216.58.213.0:80 (msInternetStatus's hardcoded probe) is
unreachable from the *Linux box on the same LAN* too — every 5 s cycle its
1 s connect fails and the 1.1.1.1 fallback succeeds in 8 ms. So at least
part of the earlier evidence rests on probes to a host that is dead for
everyone; recheck idle-then-connect against hosts that verifiably answer
before pursuing the router flow-offload theory. LWIP_TCP_RTO_TIME 500
mitigation stays for now. Note: the 1 s freeze above can also masquerade as
"first packet after idle lost" in app-level symptoms — distinguish via MIB
(freeze: frames DO reach the MAC and are handed up late; router drop: uc+0).

## Throughput optimization (2026-07-15, plan approved)

Baseline (non-debug): download 211 Mb/s vs 866 Mb/s ISP line (~24%); upload
saturates the 77 Mb/s uplink. TCPGUARD-OVZ clamp HW-validated (5 clean
speedtest cycles, 23:48 log). Plan: measurement-first, phased —
`~/.claude/plans/imperative-kindling-pudding.md` has the full version.

Root-cause arithmetic for the 711-frame RDMA overrun: 4 download streams ×
TCP_WND 256 KB / 1460 ≈ 716 RX buffers parked in socket recv queues vs 256
pool spares (512 pool − 256 ring) → pool dry → ring starves → rdmadrop →
loss → RTO (lwIP emits but does not CONSUME SACKs). Internet single stream
is window-capped: 256 KB / 15 ms ≈ 140 Mb/s. RX ring is HW-fixed at 256
descriptors (descriptor RAM abuts the RDMA register block).

**Phase 0 — measurement infra (DONE 2026-07-15, builds clean both variants):**
- `struct NetDevStats` grew named loss-point counters (96 → 128 bytes,
  ABI is an internal prototype): nds_RxOverruns (rdmadrop), nds_RxFifoOvfl
  (live RBUF_OVFL_CNT read), nds_RxPoolDry (new split off ndRxDropped),
  nds_RxBackpressure, nds_TxRejected, nds_TxBad, nds_IrqRx, nds_RxPoolFree
  (gauge). Driver+tools ship together; a size mismatch fails GET_STATS with
  NDERR_BADPARAMS.
- NEW `C/netdev-stats`: second opener beside the running stack (GET_STATS/
  GET_LINK need no ATTACH). One-shot, interval mode (`netdev-stats 2` —
  absolute + Mb/s + pps + per-interval loss deltas), and
  `netdev-stats coalesce <rx_usecs> <rx_frames> <tx_frames>` for LIVE
  SET_COALESCE (STATE_ONLINE only). This is the release-build replacement
  for the DEBUG mib/txh prints.
- `socktest bench rx|tx <host> [streams] [seconds] [bufKB]`: N nonblocking
  sockets, one WaitSelect loop, ReadEClock timing, per-stream + aggregate
  Mb/s. Peer = `scripts/tcp-bench-peer.py` on a PC (5001 sink / 5002
  source; stdlib-only). tx counts bytes accepted into sndbuf (~256 KB/
  stream tail margin). NOT iperf3 — its control protocol buys nothing here.

First live confirmation of the pool-dry mechanism before any counter was
even read (2026-07-15): `socktest bench rx 4` failed stream 3's connect
with errno 53 (ECONNABORTED = lwIP SYN-rexmit exhaustion). The peer's
source blasted on accept, streams 0-2 filled 3×256 KB windows unread
(~537 held buffers > 256 spares), pool dry → stream 3's SYN-ACK eaten
until tcp_abort. Fixed with a start gun: the peer's source now waits for
one byte, socktest fires it after ALL streams connect (peer script and
socktest must be updated TOGETHER — new peer + old client hangs waiting
for the gun).

**Phase 1 protocol:** non-debug build, default knobs;
3 × ≥10 s runs per cell of {rx,tx} × {1,4 streams} against the LAN peer;
record Mb/s + netdev-stats deltas. Classify: pooldry>0 → pool sizing
(Phase 3a); rdmadrop>0 & pooldry=0 → drain rate (Phase 2/4); rbovfl>0 →
IRQ pacing; all zero & flat → CPU-bound (Phase 4 audit).

**Phase 1 RESULTS (2026-07-15, LAN bench):**
- rx: ~278 Mb/s max; across the rx runs pooldry 33102, rdma 20,
  backpressure 0 (drops 33122 total ≈ 3.5% frame loss at 23.8k pps) —
  POOL DRY dominates, and it fires even at 1 STREAM (179 window frames
  < 256 spares!). Mechanism: on one CPU the app releases buffers into
  the recycle ring while the unit task blocks on ns_Core mid-flush —
  AFTER that pass's entry drain — so "held" transiently ≈ queued window
  + stale releases ≈ 2× window > 256 spares.
- tx: ~351 Mb/s, IDENTICAL at 1 and 4 streams, zero loss counters →
  sender is CPU-bound; ~350 Mb/s is the current per-direction CPU
  ceiling to aim rx at.
- FIX (drain-on-dry, genet bcmgenet_netdev_rx): when netdev_rx_pop comes
  up empty mid-walk, netdev_drain_recycle + retry before counting a
  drop. Arrivals imply an open window imply the app consumed & released
  — the buffers are almost always sitting in the recycle ring right
  then. HW-VALIDATED same day: 1-stream pooldry 33k → ZERO.
- RETEST after drain-on-dry: 1 stream 275 Mb/s, pooldry 0, rdma +20
  once → 1-stream rx is CPU-BOUND at ~275 (not loss-bound; eliminating
  3.5% loss didn't move it — rx path is dearer per byte than tx's 351).
  4 streams 240 Mb/s, pooldry ~7400/run, rdma +18 occasionally → the
  GENUINE capacity case: 4×179 = 716 queued-window buffers > 256 spares;
  drain-on-dry can't reclaim what the app hasn't read.

**Phase 3a RESULT (2026-07-15): VALIDATED** — 4-stream pooldry 0, back
to 275 Mb/s (= the 1-stream CPU ceiling). rdma now climbs ~100/run
(~0.04%): the 256-slot ring fills during ns_Core holds (app recv
copy-outs block the unit task mid-flush); chasing it is pointless until
the CPU ceiling moves — faster per-frame processing shortens the holds.

**Phase 4 instrumentation (2026-07-15, built both variants):** DEBUG-tier
timing via get_time() (BCM 1 MHz system timer @ 0xF2003004, single MMIO
read, no traps — timing.h). All fields unconditional so DEBUG and
release share struct layouts (Heisenbug lesson).
- genet rxprof (200 ms deltas in mib_check): `[genet] rxprof: fr+N
  drain=..us flush=..us maxflush=..us us/fr=D.d flush=F.f` — flush =
  lock wait + stack work inside nso_RxInput; drain − flush = driver
  ring mechanics (incl. CachePostDMA + descriptor MMIO).
- netstack lock profile (2 s cadence from netstack_tick): `[netstack]
  lock: N holds, wait ..us, hold ..us, maxhold ..us` — outermost holds
  only (ss_NestCount gate).
Measurement run: DEBUG/pistorm build, `socktest bench rx 1 10` — the
print overhead (~10 lines/s) suppresses absolute Mb/s but the per-frame
SPLIT is the deliverable. Interpretation: flush ≈ drain → stack-side
(lwIP input + lock waits) dominates → tcp_recved batching / csum fold /
copy-out-outside-lock; drain ≫ flush → driver mechanics (CachePostDMA,
recycle CachePreDMA, MMIO per descriptor); big maxflush + maxhold →
lock-hold spikes = the rdma bursts.

**Phase 4 PROFILE RESULTS (2026-07-15 01:55 log, rx bench steady state):**
- rxprof: ~2965 fr/200 ms, us/fr 40-44 TOTAL of which flush 38-41 —
  **flush is ~93% of drain; driver mechanics ≈ 2.5 µs/frame** (cache ops
  + descriptor MMIO are a non-issue). maxflush ~2 ms.
- lock: ~4100 outermost holds/2 s, hold total ≈ 1.67 s **(lock held 83%
  of wall time)**, wait ≈ 0.49 s. Unit-task flush ≈ 1.17 s of that; the
  other ~0.5 s = app recv copy-outs. During the tx bench: maxhold
  15-18 ms (one 64 KB tcp_write + tcp_output under a single hold) —
  the rdma-burst smoking gun.
- Conclusion: the ceiling is serialized stack-side work under ns_Core.

**Phase 4a lock-scope fixes (2026-07-15, built both variants, HW test
pending):**
- sb_tcp_recv: consume restructured — detach a run of whole pbufs under
  the lock, memcpy with the lock RELEASED, re-acquire briefly to
  pbuf_free (ndo_RxRelease contract: foreign calls serialized by
  ns_Core) + ONE batched tcp_recved per run (u16-capped). Lock-held work
  per 64 KB recv drops from ~2 ms (44 copies + 44 tcp_recved) to ~100 µs
  of pointer ops. Partial-head consume stays in place under the lock.
- sb_tcp_send: chunk cap 64 KB → 16 KB + a lock break between chunks
  (tcp_output, unlock, relock — exec semaphores queue FIFO so the pri-10
  unit task takes it immediately). Bounds maxhold; kills the tx-side
  ring starvation.
- Expected: unit-task lock wait shrinks (flush µs/fr down), app copies
  overlap with... nothing (single CPU) but no longer BLOCK injection;
  rdma → ~0; rx should rise toward/past tx's 351. Retest matrix + fresh
  rxprof/lock lines will show the new split; next candidates if lwIP
  work itself dominates: tcp_input path costs, ND_RX_BATCH, recycle
  CachePreDMA elision.

**Phase 4a RETEST + tx-regression fix (2026-07-15):** rx 275 → 309
(recv restructure works), pooldry 0, rdma a few hundred/run (peer
line-rate bursts vs our CPU-bound drain — TCP feedback working, shrinks
as CPU improves). BUT tx 351 → 200: the v1 lock break was (a)
UNCONDITIONAL — a contended FIFO handoff (two context switches) every
16 KB even with nobody waiting — and (b) 16384 is not a multiple of
MSS, and with the OVZ clamp disabling tail top-up every chunk shipped a
324-byte runt segment. v2 (built both variants): break only when
ns_Core.ss_QueueCount > 0 (racy read is safe — a late arrival waits one
chunk), and split chunks round down to whole-MSS (the app buffer's own
tail stays exact). Retest tx expected back at ~351 with rx holding 309.

**Phase 3a (2026-07-15, built both variants, HW test pending):** RX pool
size is now runtime config — `RX_POOL_BUFS` in ENV:genet.prefs, default
1280 (4 windows + ring + slack), clamped 512-4096, ~2.5 MB DMA slab at
default. Fleet of changes: GenetUnit.ndRxPoolBufs latched at UnitOpen
(like budget → needs stack restart to change); recycle ring sized at
ATTACH to next-pow2(pool) with per-unit ndRecycleMask (the old fixed
1024 would have broken SPSC at pool > 1024); caps gained named
ndc_RxPoolBufs (took reserved[0], struct still 40 B) and netdevif_create
sizes wrap storage from it (fallback ring×2 when the field reads 0 —
old-driver compat); ATTACH banner prints the pool size; genet README
documents the key. Expected: 4-stream pooldry → 0, rx ≥ the 1-stream
275; then both directions are CPU-bound → Phase 4 audit
(recycle CachePreDMA / tcp_recved batching / csum fold / batch sizes).

**Phase 2 grid (BUDGET/RXCF/RXCU):** A 64/64/500 base · B 64/64/250 ·
C 64/64/1000 · D 256/64/500 · E 256/128/1000 · F 256/32/100. Coalesce
changes are LIVE via netdev-stats; BUDGET needs a stack restart (config
loads at unit reopen). At 211 Mb/s the 64-frame threshold never fires —
the 500 µs timeout paces IRQs (~9 frames each).

**Phase 3 (code, after diagnosis):** 3a runtime-config RX_POOL_BUFS
(default 1280; recycle ring must grow past pool — fixed 1024 would break
the SPSC invariant; advertise via new caps field so netdev_if wrap count
scales, else the RxRingSlots*2=512 wrap cap re-imposes the limit).
3b TCP_WND 512 KB coupled with pool 2048. Phase 4 (conditional) ranked:
recycle-side CachePreDMA per frame, tcp_recved batching, csum fold outside
ns_Core, ND_RX_BATCH 64.
