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
> address-conversion, reentrant/getaddrinfo) report `TRUE`, the rest `FALSE`,
> and `SBTC_NUM_PACKET_FILTER_CHANNELS` reports `0`. Feature-probing apps get a
> definitive answer instead of an "unknown tag" error.

## Summary

| | Count |
|---|---|
| Named LVOs total | **121** |
| Implemented | **72** |
| Stubbed (`LibStub` / `LibStubNull`) | **49** |
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
| `vsyslog` (+ `syslog`) | −258 | ✅ | ✅ done | |
| `Dup2Socket` | −264 | ✅ | ✅ done | |
| `sendmsg` | −270 | ✅ | ✅ done | |
| `recvmsg` | −276 | ✅ | ✅ done | |
| `gethostname` | −282 | ✅ | ✅ done | |
| `gethostid` | −288 | ✅ | ✅ done | |
| `SocketBaseTagList` (+ `SocketBaseTags`) | −294 | ✅ | ✅ done | Answers `SBTC_HAVE_*` capability probes (see note above). |
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

The stack self-configures via DHCP. A `Query*` subset would still serve status
tools; the config/create family stays deferred pending the config-UX decision.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `AddInterfaceTagList` (+ `AddInterfaceTags`) | −444 | ⛔ | ❌ no | DHCP self-config supersedes. |
| `ConfigureInterfaceTagList` (+ `ConfigureInterfaceTags`) | −450 | ⛔ | ❌ no | |
| `ReleaseInterfaceList` | −456 | ⛔ | 🟡 maybe | Pairs with `ObtainInterfaceList`. |
| `ObtainInterfaceList` | −462 | ⛔ | 🟡 maybe | Status-tool candidate. |
| `QueryInterfaceTagList` (+ `QueryInterfaceTags`) | −468 | ⛔ | 🟡 maybe | Status-tool candidate (Query subset). |
| `CreateAddrAllocMessageA` (+ `CreateAddrAllocMessage`) | −474 | ⛔ | ❌ no | |
| `DeleteAddrAllocMessage` | −480 | ⛔ | ❌ no | |
| `BeginInterfaceConfig` | −486 | ⛔ | ❌ no | |
| `AbortInterfaceConfig` | −492 | ⛔ | ❌ no | |
| `RemoveInterface` | −732 | ⛔ | ❌ no | (SFD lists it later, same feature.) |

### `SBTC_HAVE_MONITORING_API` — monitor management (2 LVOs)

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `AddNetMonitorHookTagList` (+ `AddNetMonitorHookTags`) | −498 | ⛔ | ❌ no | |
| `RemoveNetMonitorHook` | −504 | ⛔ | ❌ no | |

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

AmiTCP mbuf compatibility over lwIP pbufs; near-zero modern-app value.

| LVO | Off. | Impl. | Decision | Notes |
|---|---|---|---|---|
| `mbuf_copym` | −624 | ⛔ | 🟡 on-demand | |
| `mbuf_copyback` | −630 | ⛔ | 🟡 on-demand | |
| `mbuf_copydata` | −636 | ⛔ | 🟡 on-demand | |
| `mbuf_free` | −642 | ⛔ | 🟡 on-demand | |
| `mbuf_freem` | −648 | ⛔ | 🟡 on-demand | |
| `mbuf_get` | −654 | ⛔ | 🟡 on-demand | |
| `mbuf_gethdr` | −660 | ⛔ | 🟡 on-demand | |
| `mbuf_prepend` | −666 | ⛔ | 🟡 on-demand | |
| `mbuf_cat` | −672 | ⛔ | 🟡 on-demand | |
| `mbuf_adj` | −678 | ⛔ | 🟡 on-demand | |
| `mbuf_pullup` | −684 | ⛔ | 🟡 on-demand | Implement the whole group only if a real app demands it. |

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
| `ObtainRoadshowData` | −714 | ⛔ | ❌ no | Roadshow-specific global config store; no equivalent here. |
| `ReleaseRoadshowData` | −720 | ⛔ | ❌ no | |
| `ChangeRoadshowData` | −726 | ⛔ | ❌ no | |

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
| `SBTC_HAVE_INTERFACE_API` | 10 | 0 | 🟡 Query subset later; rest ❌ |
| `SBTC_HAVE_ROUTING_API` | 5 | 0 | ❌ (GetRouteInfo maybe) |
| `SBTC_HAVE_KERNEL_MEMORY_API` | 11 | 0 | 🟡 on-demand only |
| `SBTC_NUM_PACKET_FILTER_CHANNELS` (BPF) | 8 | 0 | 🔜 future |
| `SBTC_HAVE_MONITORING_API` | 2 | 0 | ❌ no |
| `SBTC_HAVE_SERVER_API` | 2 | 0 | ❌ no |
| `SBTC_HAVE_ROADSHOWDATA_API` | 3 | 0 | ❌ no |
| IP filter (`ipf_*`, private) | 7 | 0 | ❌ never |
| **Total** | **121** | **72** | |
