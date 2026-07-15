# bsdsocket.library — LVO interface coverage

A per-LVO map of the `bsdsocket.library` interface: what the API surface is,
what **lwip-amiga** implements today, and the decision on each stub. Sourced
from the NDK 3.2R4 SFD (`sfd/bsdsocket_lib.sfd`) and autodoc
(`SANA+RoadshowTCP-IP/doc/bsdsocket.doc`), cross-checked against our LVO vector
table (`src/bsdsocket/vectors.c`).

## How the interface is organized

The library has two tiers:

- **Mandatory core** — the AmiTCP v4 baseline. Every `bsdsocket.library` must
  provide these; they are the sockets API applications actually link against.
- **Roadshow extensions** — added by Roadshow *after* the AmiTCP baseline
  (the SFD marks the boundary: *"Berkeley Packet Filter (Roadshow extensions
  start here)"*, LVO −366). These are grouped into **capability features**, and
  an application probes for each feature at runtime by reading a capability tag
  through `SocketBaseTagList()` (e.g. `SocketBaseTags(SBTM_GETREF(SBTC_HAVE_ROUTING_API), &flag, TAG_END)`).
  If the tag comes back set, every LVO in that feature is present. The tables
  below are grouped by that feature tag.

> **Capability probe.** Our `SocketBaseTagList` answers every `SBTC_HAVE_*`
> feature tag (`sb_misc.c`): the implemented groups (DNS, local-database,
> address-conversion, reentrant/getaddrinfo, **interface**) report `TRUE`, the
> rest `FALSE`, and `SBTC_NUM_PACKET_FILTER_CHANNELS` reports `0`. The
> interface tag reports `TRUE` for its read-only query subset; the config LVOs
> in that group refuse gracefully with `EINVAL` (configuration is
> prefs-file-only). Feature-probing apps get a definitive answer instead of an
> "unknown tag" error.

## Summary

| | Count |
|---|---|
| Named LVOs total | **121** |
| Implemented | **75** |
| Stubbed (`LibStub` / `LibStubNull`) | **46** |
| Reserved slots (not counted above) | 18 |

Legend — **Impl.**: ✅ implemented · 🟡 implemented with a documented limitation · ⛔ stub.
**Decision**: ✅ done · 🔜 planned/later · 🟡 partial / on-demand · ❌ not planned / out of scope.

Varargs siblings (`…Tags`, `syslog`) share the LVO of their `…TagList` / `…A`
base and do not consume a separate vector; they are noted in parentheses.

---

## Mandatory core — AmiTCP v4 baseline (46 LVOs)

All 46 are implemented.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `socket` | −30 | ✅ | ✅ done | |
| `bind` | −36 | ✅ | ✅ done | |
| `listen` | −42 | ✅ | ✅ done | |
| `accept` | −48 | ✅ | ✅ done | |
| `connect` | −54 | ✅ | ✅ done | |
| `sendto` | −60 | ✅ | ✅ done | |
| `send` | −66 | ✅ | ✅ done | |
| `recvfrom` | −72 | ✅ | ✅ done | |
| `recv` | −78 | ✅ | ✅ done | |
| `shutdown` | −84 | ✅ | ✅ done | |
| `setsockopt` | −90 | ✅ | ✅ done | |
| `getsockopt` | −96 | ✅ | ✅ done | |
| `getsockname` | −102 | ✅ | ✅ done | |
| `getpeername` | −108 | ✅ | ✅ done | |
| `IoctlSocket` | −114 | ✅ | ✅ done | |
| `CloseSocket` | −120 | ✅ | ✅ done | |
| `WaitSelect` | −126 | ✅ | ✅ done | |
| `SetSocketSignals` | −132 | ✅ | ✅ done | |
| `getdtablesize` | −138 | ✅ | ✅ done | Fixed table size (`SB_FD_COUNT`). |
| `ObtainSocket` | −144 | ✅ | ✅ done | |
| `ReleaseSocket` | −150 | ✅ | ✅ done | |
| `ReleaseCopyOfSocket` | −156 | ✅ | ✅ done | Shared copies wake **all** holders (per-base owner refcounting in `SbSocket.owners`). |
| `Errno` | −162 | ✅ | ✅ done | |
| `SetErrnoPtr` | −168 | ✅ | ✅ done | |
| `Inet_NtoA` | −174 | ✅ | ✅ done | |
| `inet_addr` | −180 | ✅ | ✅ done | |
| `Inet_LnaOf` | −186 | ✅ | ✅ done | |
| `Inet_NetOf` | −192 | ✅ | ✅ done | |
| `Inet_MakeAddr` | −198 | ✅ | ✅ done | |
| `inet_network` | −204 | ✅ | ✅ done | |
| `gethostbyname` | −210 | ✅ | ✅ done | |
| `gethostbyaddr` | −216 | ✅ | ✅ done | Reverse DNS via PTR query (`sb_rdns.c`); `NULL`/`HOST_NOT_FOUND` when no PTR. |
| `getnetbyname` | −222 | ✅ | ✅ done | |
| `getnetbyaddr` | −228 | ✅ | ✅ done | |
| `getservbyname` | −234 | ✅ | ✅ done | |
| `getservbyport` | −240 | ✅ | ✅ done | |
| `getprotobyname` | −246 | ✅ | ✅ done | |
| `getprotobynumber` | −252 | ✅ | ✅ done | |
| `vsyslog` (+ `syslog`) | −258 | ✅ | ✅ done | Honours `SBTC_LOGMASK` priority filter and prefixes `SBTC_LOGTAGPTR` ident. |
| `Dup2Socket` | −264 | ✅ | ✅ done | |
| `sendmsg` | −270 | ✅ | ✅ done | |
| `recvmsg` | −276 | ✅ | ✅ done | |
| `gethostname` | −282 | ✅ | ✅ done | |
| `gethostid` | −288 | ✅ | ✅ done | |
| `SocketBaseTagList` (+ `SocketBaseTags`) | −294 | ✅ | ✅ done | errno/h_errno wiring, signal masks, syslog config (`SBTC_LOG*`), and `SBTC_HAVE_*` capability probes (see note above). The C runtimes set `SBTC_LOGTAGPTR` at socket-init — declining it aborts init. |
| `GetSocketEvents` | −300 | ✅ | ✅ done | |

*(LVOs −306…−360 are 10 reserved slots.)*

---

## Roadshow extensions — grouped by capability feature (75 LVOs)

### `SBTC_NUM_PACKET_FILTER_CHANNELS` — Berkeley Packet Filter (8 LVOs)

Packet capture (tcpdump-class). Would map to a promiscuous RAW netif tap.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `bpf_open` | −366 | ⛔ | 🔜 future | |
| `bpf_close` | −372 | ⛔ | 🔜 future | |
| `bpf_read` | −378 | ⛔ | 🔜 future | |
| `bpf_write` | −384 | ⛔ | 🔜 future | |
| `bpf_set_notify_mask` | −390 | ⛔ | 🔜 future | |
| `bpf_set_interrupt_mask` | −396 | ⛔ | 🔜 future | |
| `bpf_ioctl` | −402 | ⛔ | 🔜 future | |
| `bpf_data_waiting` | −408 | ⛔ | 🔜 future | |

### `SBTC_HAVE_ROUTING_API` — route management (5 LVOs)

lwIP has no route table beyond netif + gateway, so there is nothing to expose.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `AddRouteTagList` (+ `AddRouteTags`) | −414 | ⛔ | ❌ no | |
| `DeleteRouteTagList` (+ `DeleteRouteTags`) | −420 | ⛔ | ❌ no | |
| `ChangeRouteTagList` (+ `ChangeRouteTags`) | −426 | ⛔ | ❌ no | Private / unimplemented in Roadshow itself. |
| `FreeRouteInfo` | −432 | ⛔ | ❌ no | |
| `GetRouteInfo` | −438 | ⛔ | 🟡 maybe | Only member with plausible value (report netif + gateway) if a tool needs it. |

### `SBTC_HAVE_INTERFACE_API` — interface management (10 LVOs)

The stack self-configures from `ENVARC:netstack.prefs`, so **configuration is
declined**: the config/create family refuses gracefully with `EINVAL` (a shared
`bsd_InterfaceConfigUnsupported` stub, `sb_ifquery.c`). The **read-only query
subset is implemented** (`sb_ifquery.c`) and backs the `netinfo` CLI; the
capability tag is advertised so third-party status apps can use it too.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `AddInterfaceTagList` (+ `AddInterfaceTags`) | −444 | 🟡 | ❌ no | Refuses with `EINVAL`; config is prefs-file-only. |
| `ConfigureInterfaceTagList` (+ `ConfigureInterfaceTags`) | −450 | 🟡 | ❌ no | Refuses with `EINVAL`. |
| `ReleaseInterfaceList` | −456 | ✅ | ✅ done | |
| `ObtainInterfaceList` | −462 | ✅ | ✅ done | Names of the live (non-loopback) interfaces. |
| `QueryInterfaceTagList` (+ `QueryInterfaceTags`) | −468 | ✅ | ✅ done | Address/mask/broadcast/MTU/MAC/state/bind-type/DNS `IFQ_*` tags; counters out of scope (use `netdev-stats`). |
| `CreateAddrAllocMessageA` (+ `CreateAddrAllocMessage`) | −474 | ⛔ | ❌ no | Config; `NULL` stub. |
| `DeleteAddrAllocMessage` | −480 | ⛔ | ❌ no | |
| `BeginInterfaceConfig` | −486 | ⛔ | ❌ no | |
| `AbortInterfaceConfig` | −492 | ⛔ | ❌ no | |
| `RemoveInterface` | −732 | 🟡 | ❌ no | Refuses with `EINVAL`. (SFD lists it later, same feature.) |

### `SBTC_HAVE_MONITORING_API` — monitor management (2 LVOs)

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `AddNetMonitorHookTagList` (+ `AddNetMonitorHookTags`) | −498 | ⛔ | ❌ never | |
| `RemoveNetMonitorHook` | −504 | ⛔ | ❌ never | |

### `SBTC_HAVE_STATUS_API` — status query (1 LVO)

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `GetNetworkStatistics` | −510 | ⛔ | 🔜 later | Worth faking from lwIP's own stats counters. |

### `SBTC_HAVE_DNS_API` — DNS server & default-domain management (6 LVOs)

Fully implemented.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `AddDomainNameServer` | −516 | ✅ | ✅ done | |
| `RemoveDomainNameServer` | −522 | ✅ | ✅ done | |
| `ReleaseDomainNameServerList` | −528 | ✅ | ✅ done | |
| `ObtainDomainNameServerList` | −534 | ✅ | ✅ done | |
| `GetDefaultDomainName` | −702 | ✅ | ✅ done | (SFD "Default domain name" group, same feature tag.) |
| `SetDefaultDomainName` | −708 | ✅ | ✅ done | |

### `SBTC_HAVE_LOCAL_DATABASE_API` — local database access (9 LVOs)

Fully implemented.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `setnetent` | −540 | ✅ | ✅ done | |
| `endnetent` | −546 | ✅ | ✅ done | |
| `getnetent` | −552 | ✅ | ✅ done | |
| `setprotoent` | −558 | ✅ | ✅ done | |
| `endprotoent` | −564 | ✅ | ✅ done | |
| `getprotoent` | −570 | ✅ | ✅ done | |
| `setservent` | −576 | ✅ | ✅ done | |
| `endservent` | −582 | ✅ | ✅ done | |
| `getservent` | −588 | ✅ | ✅ done | |

### `SBTC_HAVE_ADDRESS_CONVERSION_API` — address conversion (5 LVOs)

Fully implemented.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `inet_aton` | −594 | ✅ | ✅ done | |
| `inet_ntop` | −600 | ✅ | ✅ done | IPv4 only. |
| `inet_pton` | −606 | ✅ | ✅ done | IPv4 only. |
| `In_LocalAddr` | −612 | ✅ | ✅ done | |
| `In_CanForward` | −618 | ✅ | ✅ done | |

### `SBTC_HAVE_KERNEL_MEMORY_API` — kernel memory / mbuf (11 LVOs)

Not general mbuf compatibility for apps: every `mbuf_*` autodoc entry states
*"expected to be called on the context of the kernel from within the IP
filter hook. Do not call it from user code or you will crash the stack."*
This is the memory API for writing an `ipf_*` filter hook — dead weight
without `ipf_*`, which is itself `❌ never` (private, out of scope).

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `mbuf_copym` | −624 | ⛔ | ❌ never | |
| `mbuf_copyback` | −630 | ⛔ | ❌ never | |
| `mbuf_copydata` | −636 | ⛔ | ❌ never | |
| `mbuf_free` | −642 | ⛔ | ❌ never | |
| `mbuf_freem` | −648 | ⛔ | ❌ never | |
| `mbuf_get` | −654 | ⛔ | ❌ never | |
| `mbuf_gethdr` | −660 | ⛔ | ❌ never | |
| `mbuf_prepend` | −666 | ⛔ | ❌ never | |
| `mbuf_cat` | −672 | ⛔ | ❌ never | |
| `mbuf_adj` | −678 | ⛔ | ❌ never | |
| `mbuf_pullup` | −684 | ⛔ | ❌ never | Only relevant if `ipf_*` is ever implemented. |

### `SBTC_HAVE_SERVER_API` — internet-server (inetd) support (2 LVOs)

The `ObtainSocket`/`ReleaseSocket` family already covers the socket handoff apps
actually need.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `ProcessIsServer` | −690 | ⛔ | ❌ no | |
| `ObtainServerSocket` | −696 | ⛔ | ❌ no | |

### `SBTC_HAVE_ROADSHOWDATA_API` — global data access (3 LVOs)

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `ObtainRoadshowData` | −714 | ⛔ | ❌ never | Roadshow-specific global config store; no equivalent here. |
| `ReleaseRoadshowData` | −720 | ⛔ | ❌ never | |
| `ChangeRoadshowData` | −726 | ⛔ | ❌ never | |

### `SBTC_HAVE_GETHOSTADDR_R_API` — reentrant lookups & RFC 3493 name/service translation (6 LVOs)

Fully implemented. The reentrant `_r` lookups and the `getaddrinfo` family share
this one capability tag.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `gethostbyname_r` | −738 | ✅ | ✅ done | |
| `gethostbyaddr_r` | −744 | ✅ | ✅ done | Reverse DNS via `gethostbyaddr`. |
| `freeaddrinfo` | −804 | ✅ | ✅ done | |
| `getaddrinfo` | −810 | ✅ | ✅ done | RFC 2553 subset. |
| `gai_strerror` | −816 | ✅ | ✅ done | |
| `getnameinfo` | −822 | ✅ | ✅ done | Reverse DNS; falls back to numeric unless `NI_NAMEREQD` (→ `EAI_NONAME`). |

### `SBTC_IPF_API_VERSION` / `SBTC_NUM_PACKET_FILTER_CHANNELS` — IP filter (private, 7 LVOs)

Roadshow's private, undocumented IP-filter/logging interface. Marked `==private`
in the SFD; the doc warns the interface is subject to change.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `ipf_open` | −762 | ⛔ | ❌ never | Private, out of scope. |
| `ipf_close` | −768 | ⛔ | ❌ never | |
| `ipf_ioctl` | −774 | ⛔ | ❌ never | |
| `ipf_log_read` | −780 | ⛔ | ❌ never | |
| `ipf_log_data_waiting` | −786 | ⛔ | ❌ never | |
| `ipf_set_notify_mask` | −792 | ⛔ | ❌ never | |
| `ipf_set_interrupt_mask` | −798 | ⛔ | ❌ never | |

*(LVOs −750/−756 are 2 reserved; −828…−858 are 6 reserved.)*

---

## Feature-group roll-up

| Feature tag | LVOs | Implemented | Decision |
|---|---|---|---|
| *(mandatory core)* | 46 | 46 | ✅ done |
| `SBTC_HAVE_DNS_API` | 6 | 6 | ✅ done |
| `SBTC_HAVE_LOCAL_DATABASE_API` | 9 | 9 | ✅ done |
| `SBTC_HAVE_ADDRESS_CONVERSION_API` | 5 | 5 | ✅ done |
| `SBTC_HAVE_GETHOSTADDR_R_API` | 6 | 6 | ✅ done |
| `SBTC_HAVE_STATUS_API` | 1 | 0 | 🔜 later (fake from lwIP stats) |
| `SBTC_HAVE_INTERFACE_API` | 10 | 3 | ✅ query subset; config ❌ (graceful `EINVAL`) |
| `SBTC_HAVE_ROUTING_API` | 5 | 0 | ❌ (GetRouteInfo maybe) |
| `SBTC_HAVE_KERNEL_MEMORY_API` | 11 | 0 | ❌ never (only useful to an `ipf_*` hook) |
| `SBTC_NUM_PACKET_FILTER_CHANNELS` (BPF) | 8 | 0 | 🔜 future |
| `SBTC_HAVE_MONITORING_API` | 2 | 0 | ❌ never |
| `SBTC_HAVE_SERVER_API` | 2 | 0 | ❌ no |
| `SBTC_HAVE_ROADSHOWDATA_API` | 3 | 0 | ❌ never |
| IP filter (`ipf_*`, private) | 7 | 0 | ❌ never |
| **Total** | **121** | **75** | |

---

## `SocketBaseTagList` config & per-opener tags (non-feature)

`SocketBaseTagList()` (LVO −294) carries two kinds of tag. The `SBTC_HAVE_*`
capability tags — one per Roadshow feature group — are covered by the
**Feature-group roll-up** above. Everything else is a *config or per-opener
setting* (errno/h_errno plumbing, task signal masks, syslog, stack tuning): the
full set is listed here, sourced from `netinclude/libraries/bsdsocket.h`
(codes) and `bsdsocket.doc`, cross-checked against `sb_misc.c`.

Each tag is a `SBTM_{GET,SET}{VAL,REF}(code)` request. A tag this library does
not handle falls through to `default:` and `SocketBaseTagList` returns that
tag's **1-based position** — Roadshow's "could not process this item"
convention. **This is not free to decline:** a C runtime that bundles an
unsupported tag into its startup taglist aborts with *"bsdsocket.library could
not be initialized"* — exactly what `SBTC_LOGTAGPTR` (11) did to every clib2
program before it was handled. Config tags declined below are safe only because
no CRT sets them at init (`SBTC_CAN_SHARE_LIBRARY_BASES`/`SBTC_ERROR_HOOK`
arrive in their own non-fatal taglist, *after* init has already succeeded).

Legend as above — **Impl.**: ✅ handled · 🟡 handled, one direction/limitation ·
⛔ falls through to `default:` (reported as an unprocessed tag).

| Tag | Code | Purpose | Impl. | Decision | Notes |
|---|---|---|---|---|---|
| `SBTC_BREAKMASK` | 1 | Signal mask that aborts blocking calls (^C) | ✅ | ✅ done | Per-opener; defaults to `SIGBREAKF_CTRL_C`. |
| `SBTC_SIGIOMASK` | 2 | Signal delivered on async socket readiness (SIGIO) | ✅ | ✅ done | |
| `SBTC_SIGURGMASK` | 3 | Signal delivered on out-of-band data (SIGURG) | ✅ | ✅ done | |
| `SBTC_SIGEVENTMASK` | 4 | Signal delivered on `FD_*` socket events | ✅ | ✅ done | |
| `SBTC_ERRNO` | 6 | Current `errno` value | ✅ | ✅ done | |
| `SBTC_HERRNO` | 7 | Current `h_errno` value | ✅ | ✅ done | |
| `SBTC_DTABLESIZE` | 8 | Socket descriptor-table size | 🟡 | ✅ done | GET returns `SB_FD_COUNT`; SET refused (fixed table). |
| `SBTC_FDCALLBACK` | 9 | Link-library fd alloc/free callback | ⛔ | ❌ no | Legacy; the header itself says *"don't use in new code"*. |
| `SBTC_LOGSTAT` | 10 | `openlog()` options (`LOG_PID`, …) | ✅ | ✅ done | Stored per-opener (advisory). |
| `SBTC_LOGTAGPTR` | 11 | `syslog` ident string pointer | ✅ | ✅ done | Prefixed to each `vsyslog` line. Set by clib2/newlib at init. |
| `SBTC_LOGFACILITY` | 12 | Default `syslog` facility | ✅ | ✅ done | Stored per-opener (advisory). |
| `SBTC_LOGMASK` | 13 | `setlogmask()` priority bitmask | ✅ | ✅ done | Honoured by `vsyslog`; defaults to all priorities. |
| `SBTC_ERRNOSTRPTR` | 14 | Pointer to a string describing current `errno` | ⛔ | ❌ no | `strerror` lives in the C runtime; no string tables here. |
| `SBTC_HERRNOSTRPTR` | 15 | String describing current `h_errno` | ⛔ | ❌ no | As above. |
| `SBTC_IOERRNOSTRPTR` | 16 | String describing the last `IoErr()` | ⛔ | ❌ no | As above. |
| `SBTC_S2ERRNOSTRPTR` | 17 | String for the primary I/O error code | ⛔ | ❌ no | As above. |
| `SBTC_S2WERRNOSTRPTR` | 18 | String for the secondary/wire I/O error code | ⛔ | ❌ no | As above. |
| `SBTC_ERRNOBYTEPTR` | 21 | Wire `errno` to a caller `BYTE` | ✅ | ✅ done | SET-only (as designed). |
| `SBTC_ERRNOWORDPTR` | 22 | Wire `errno` to a caller `WORD` | ✅ | ✅ done | SET-only. |
| `SBTC_ERRNOLONGPTR` | 24 | Wire `errno` to a caller `LONG` | ✅ | ✅ done | SET-only; `NULL` restores the internal errno. |
| `SBTC_HERRNOLONGPTR` | 25 | Wire `h_errno` to a caller `LONG` | ✅ | ✅ done | SET-only; `NULL` restores the internal h_errno. |
| `SBTC_RELEASESTRPTR` | 29 | Pointer to the stack's release/version string | ⛔ | 🔜 later | Trivial to return the `$VER` string; low demand. |
| `SBTC_UDP_CHECKSUM` | 42 | Enable/disable UDP checksums | ⛔ | ❌ no | lwIP always checksums UDP; not runtime-toggleable. |
| `SBTC_IP_FORWARDING` | 43 | Enable/disable IP forwarding | ⛔ | ❌ no | Single-homed host, not a router. |
| `SBTC_IP_DEFAULT_TTL` | 44 | Get/set default IP TTL | ⛔ | ❌ no | Fixed at the lwIP default. |
| `SBTC_ICMP_MASK_REPLY` | 45 | Reply to ICMP address-mask requests | ⛔ | ❌ no | ICMP behaviour is lwIP's; no per-opener control. |
| `SBTC_ICMP_SEND_REDIRECTS` | 46 | Send ICMP redirects | ⛔ | ❌ no | As above. |
| `SBTC_ICMP_PROCESS_ECHO` | 48 | How to process ICMP echo requests | ⛔ | ❌ no | As above. |
| `SBTC_ICMP_PROCESS_TSTAMP` | 49 | How to process ICMP timestamp requests | ⛔ | ❌ no | As above. |
| `SBTC_CAN_SHARE_LIBRARY_BASES` | 51 | Opt in to sharing one base across callers | ⛔ | ❌ no | **Deliberately declined** — per-opener state (`task`, `errnoPtr`, `sigBit`) lives in the child base; callers keep their own base. |
| `SBTC_LOG_FILE_NAME` | 52 | Get/set the log output file name | ⛔ | ❌ no | Logging goes to the debug backend (`Kprintf`), not a file. |
| `SBTC_LOG_HOOK` | 55 | Get/set the installed log hook | ⛔ | ❌ no | As above. |
| `SBTC_SYSTEM_STATUS` | 56 | Query `SBSYSSTAT_*` (interfaces/resolver/routes up) | ⛔ | 🔜 later | Synthesizable from netif up/down + configured DNS servers. |
| `SBTC_SIG_ADDRESS_CHANGE_MASK` | 57 | Signal on interface-address change | ⛔ | 🔜 later | Plausible once link/DHCP-renew events are surfaced. |
| `SBTC_IP_FILTER_HOOK` | 62 | Get/set the IP filter (`ipf_*`) hook | ⛔ | ❌ never | Private IP-filter interface; out of scope (see `ipf_*`). |
| `SBTC_GET_BYTES_RECEIVED` | 64 | Query total bytes received | ⛔ | 🔜 later | `netdev-stats` already tracks byte counters. |
| `SBTC_GET_BYTES_SENT` | 65 | Query total bytes sent | ⛔ | 🔜 later | As above. |
| `SBTC_IDN_DEFAULT_CHARACTER_SET` | 66 | IDN charset for resolver name translation | ⛔ | ❌ no | No IDN/punycode translation in the resolver. |
| `SBTC_ERROR_HOOK` | 68 | Install the shared-base error-routing hook | ⛔ | ❌ no | **Deliberately declined** — only meaningful with `SBTC_CAN_SHARE_LIBRARY_BASES`, which is declined. |

*(Codes 5, 19–20, 23, 26–28 are unassigned in the NDK 3.2 header. `SBTC_ERRNOPTR(size)`
is a macro that resolves to the byte/word/long errno-pointer tag, not a distinct code.)*
