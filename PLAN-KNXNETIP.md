# PLAN-KNXNETIP.md — Direct KNXnet/IP transport (no knxd)

**Status:** implemented in 2.0.0 (P0–P8)
**Target:** `cometvisu-knx-fcgi` **2.0.0**
**Audience:** the implementing LLM agent (and the human reviewing its work)

> **Decisions locked 2026-11-15** (see §8): all three transports are equally
> supported and the operator must choose one explicitly (no default); the
> outage key is `"knxd"` for the knxd transport and `"bus"` otherwise; KNX IP
> Secure stays out of scope; the shared TX queue is used by **all** transports,
> so the cache reader is the single bus owner for RX *and* TX.
> Because the startup contract changes (`KNX_TRANSPORT` becomes required) and
> a new JSON key appears, this work ships as **2.0.0**, not 1.3.0.

---

## 0. How to use this document

This is an executable implementation plan, not a design essay.  Work through
the phases **in order**, one phase per pull request (or per branch).  Every
phase is written as a **TDD script**: each numbered step names the test to
write *first*, states what it must assert, and only then describes the
implementation.

Hard rules (they come from [AGENTS.md](AGENTS.md) and are not negotiable):

1. **No implementation before a failing test exists.**  Run the test, watch it
   fail for the expected reason, then implement.
2. **Never weaken or delete an existing test** to make new code pass.  If an
   existing test is genuinely wrong, say so in the PR description and explain
   why — do not silently edit it.
3. **`ctest --test-dir build --output-on-failure` must be green at every commit.**
4. **`clang-format -i` on every touched file**; C++20, Google style, 100 columns,
   2-space indents, never tabs.
5. **GPLv3 header on every new `.cpp`/`.h`/`CMakeLists.txt`** (see AGENTS.md).
6. **No new third-party dependencies.**  No `boost::asio`, no `libknx`, no
   vendored protocol libraries.  Plain POSIX sockets.
7. **Do not change the CometVisu-facing JSON contract** (`/l`, `/r`, `/w`
   bodies and status codes) unless a phase says so explicitly.
8. Update [AGENTS.md](AGENTS.md) (file table, design decisions), [README.md](README.md)
   (env vars) and [CHANGELOG.md](CHANGELOG.md) in the same PR as the change.

Phase gates are listed per phase under **Acceptance**.  A phase is not done
until its gate passes on both gcc and clang (CI matrix).

---

## 1. Goal and non-goals

### Goal

Let the backend talk to the KNX bus through **KNXnet/IP** directly, so that a
CometVisu installation needs **no knxd daemon** and no Unix socket:

- **IP tunneling** — client of a KNX IP interface / router (point-to-point
  UDP, port 3671).
- **IP routing** — multicast member (224.0.23.12:3671), fire-and-forget.

knxd stays supported as a third transport (existing deployments, and users who
need the features knxd adds).

### Non-goals (explicitly out of scope for this plan)

- **KNX IP Secure** (secure wrapper, Curve25519 session, AES-128-CCM).  This is
  a separate project of comparable size; see §8.  Design the code so a
  `SecureWrapper` can wrap the frame codec later, but do not implement it.
- KNX IP **device management** (0x0310) and **bus monitoring** (0x80 layer).
- IPv6 (KNXnet/IP v2).  IPv4 only; the HPAI codec keeps a protocol byte so v2
  can be added later.
- Replacing the CometVisu session/auth model, the FCGI layer, or the cache.

---

## 2. What already exists and must be reused

| Item | Location | Reuse |
|---|---|---|
| `KnxdClientInterface` (transport seam) | `src/knxd/knxd_client.h` | Rename → `BusLink` (P1). Handlers use only this. |
| APDU encode/decode (`[0x00, APCI\|value]`) | `src/knxd/knxd_protocol.{h,cpp}` | **Reused unchanged** — cEMI carries the identical APDU. |
| Group address mapping `X/Y/Z → (X<<11)\|(Y<<8)\|Z` | `knxd_protocol.{h,cpp}` | Reused unchanged. |
| `SharedGroupCache` + position counter + condvar | `src/state/shared_group_cache.{h,cpp}` | Unchanged. |
| `run_cache_reader()` resilient loop | `src/knxd/cache_reader.{h,cpp}` | Extended (P4) to also drain a TX queue; the reconnect/backoff/no-busy-loop semantics stay. |
| Outage reporting (`knxd_status_json`) | `src/handlers/knxd_status_json.{h,cpp}` | Same shape, **transport-dependent key** `"knxd"` / `"bus"` (§8 Q2); rename to `add_bus_outage()` (P1). |
| Fork-based worker pool + supervisor | `src/main.cpp`, `src/fcgi/` | Unchanged. |
| `MockKnxdClient` | `mocks/mock_knxd_socket.{h,cpp}` | Rename → `MockBusLink` (P1). |

Nothing in `src/fcgi/`, `src/router/`, `src/handlers/`, `src/state/` (except the
new TX queue) and `src/util/` needs to know which transport is in use.

---

## 3. Target architecture

```
                 ┌──────────────────────────────────────────────┐
   CometVisu ───►│  FCGI  →  Router  →  /l   /r   /w             │
                 │                          │        │            │
                 │                     SharedGroupCache  BusLink (write only)
                 │                          ▲        │            │
                 └──────────────────────────┼────────┼────────────┘
                                            │        │
        ┌───────────────────────────────────┼────────┼──────────────────────┐
        │ Forked worker processes (N)       │        │                      │
        └───────────────────────────────────┼────────┼──────────────────────┘
                                            │        │  (knxd: own write socket
                                            │        │   ip-routing: own send
                                            │        │   ip-tunnel: shared TX queue)
        ┌───────────────────────────────────┼────────┼──────────────────────┐
        │ Cache-reader process (1)          │        │                      │
        │  run_cache_reader()  ─────────────┘        │                      │
        │      │                                     │                      │
        │      ▼                                     ▼                      │
        │  BusLink (RX + TX owner)  ── knxd socket / UDP 3671 / multicast    │
        └────────────────────────────────────────────────────────────────────┘
```

### 3.1 The transport seam

`BusLink` (renamed `KnxdClientInterface`) keeps its current shape:

```cpp
class BusLink {
public:
  virtual ~BusLink() = default;
  virtual bool connect() = 0;                       // P1: no socket_path argument
  virtual void disconnect() = 0;
  virtual bool reconnect() = 0;
  virtual bool is_connected() const = 0;
  virtual bool send_group_packet(uint16_t group_addr,
                                 const std::vector<uint8_t>& apdu) = 0;
  virtual bool poll_group_telegram(uint16_t& out_addr,
                                   std::vector<uint8_t>& out_apdu) = 0;
  virtual int get_fd() const = 0;                   // poll()-able fd, -1 if none
  virtual uint64_t get_telegram_count() const = 0;
  virtual void set_nonblocking(bool enable) = 0;
  enum class WaitResult : uint8_t { Timeout, GroupData, Error };
  virtual WaitResult wait_for_activity(int timeout_ms) = 0;
};
```

Connection parameters move into the constructor (from `BusConfig`), so the
handlers/loop never see a socket path or an IP address.  `open_group_socket()`
disappears: for knxd it becomes an internal detail of `KnxdBusLink::connect()`;
for KNXnet/IP it is meaningless.

**Rule:** one `BusLink` implementation per transport, and *nothing else* in the
codebase may know which one is in use.  If an `if (transport == …)` appears
outside `main.cpp` / `BusFactory`, the abstraction is wrong — fix the seam
instead of adding the branch.  This is what keeps the three transports at
maximal shared code and minimal total size.

### 3.2 Process model impact — the one real constraint

A KNX IP interface offers only **~4 concurrent tunnels** (MDT, Gira, Siemens,
…), while the pool runs `FCGI_WORKERS` (default 20) processes.  Therefore:

| Transport | Receiving | Sending | Bus connections needed |
|---|---|---|---|
| `knxd` | cache reader (RW group socket) | **shared TX queue → cache reader** | **1** |
| `ip-routing` | cache reader (joined multicast group) | **shared TX queue → cache reader** | **1** (stateless) |
| `ip-tunnel` | cache reader (tunnel owner) | **shared TX queue → cache reader** | **1** |

All three transports therefore have the **same process shape**: exactly one
process (the cache reader) owns the bus link for RX *and* TX, and every worker
only enqueues into the shared TX queue (§3.3, §8 Q4).  That is why
`FCGI_WORKERS` never appears in the right-hand column: a tunnel limit of ~4 is
irrelevant, and knxd needs a single client slot instead of `FCGI_WORKERS + 1`.
It also removes `open_group_socket()` and per-worker socket bookkeeping from the
worker path — less code, one fewer class of bug.

### 3.3 Shared TX queue (all transports)

- `SharedTxQueue` — fixed-size ring (e.g. 256 slots) in
  `mmap(MAP_SHARED|MAP_ANONYMOUS)`, one slot =
  `{uint16_t group_addr; uint8_t len; uint8_t data[14];}`, protected by the same
  `PTHREAD_PROCESS_SHARED` + `PTHREAD_MUTEX_ROBUST` pattern as
  `SharedGroupCache`.
- An `eventfd(0, EFD_NONBLOCK)` created by the parent **before** forking is
  inherited by all children; a worker `write()`s 8 bytes after enqueueing.
- The cache reader polls `{link_fd, tx_eventfd}`; on the event it drains the
  queue and sends through its bus link — for `ip-tunnel` including the ACK /
  retransmission handshake, for `knxd` and `ip-routing` a plain send.
- A full queue makes `send_group_packet()` return `false` → `/w` answers
  `503`, exactly like a failed write today.
- While the bus link is unavailable, `send_group_packet()` must return `false`
  **without** enqueueing, so a `/w` during an outage fails (and reports the
  outage) instead of silently succeeding.

---

## 4. Configuration surface (final state)

| Variable | Default | Applies to | Meaning |
|---|---|---|---|
| `KNX_TRANSPORT` | *(required)* | all | `knxd` \| `ip-tunnel` \| `ip-routing`; unset or unknown is a startup error |
| `KNXD_SOCKET` | `/run/knx` | knxd | Unix socket path |
| `KNX_IP_HOST` | *(unset)* | ip-tunnel | IP interface / router address |
| `KNX_IP_PORT` | `3671` | ip-tunnel, ip-routing | UDP port |
| `KNX_IP_LOCAL_ADDR` | *(auto)* | ip-tunnel | local address advertised in the HPAI (`0.0.0.0` = let the OS choose, NAT-friendly) |
| `KNX_IP_MULTICAST` | `224.0.23.12` | ip-routing | routing multicast group |
| `KNX_IP_IFACE` | *(default route)* | ip-routing | interface name for `IP_MULTICAST_IF` / group join |
| `KNX_IP_NAT` | `0` | ip-tunnel | `1` = NAT mode (accept NAT'd HPAIs, keep-alive) |
| `KNX_IP_HEARTBEAT_SEC` | `60` | ip-tunnel | `CONNECTIONSTATE_REQUEST` interval (device timeout is 120 s) |
| `KNX_IP_ACK_TIMEOUT_MS` | `1000` | ip-tunnel | wait for `TUNNELING_ACK` before retransmitting |
| `KNX_IP_RETRIES` | `3` | ip-tunnel | retransmissions per telegram before giving up |

**Breaking change (2.0.0):** `KNX_TRANSPORT` has **no default**.  A process
started without it (or with an unknown value) logs `[ERROR]` naming the three
valid values and exits 1.  This is deliberate — silently falling back to `knxd`
would turn an `ip-*` misconfiguration into what looks like a bus outage.  The
release notes must show the one-line migration: `KNX_TRANSPORT=knxd` restores
1.x behaviour exactly.  All other variables keep their current defaults.

New variables are printed in the existing startup configuration dump and in
`--help` (including the fact that `KNX_TRANSPORT` is mandatory).

---

## 5. Protocol reference (byte level)

Everything below is what the implementation must produce/accept.  Put it in
`src/knxnetip/DESIGN.md`-style comments at the top of the codec headers, and
assert it with the hex vectors in §7.

### 5.1 KNXnet/IP header (6 bytes)

| Offset | Size | Value |
|---|---|---|
| 0 | 1 | `0x06` header length |
| 1 | 1 | `0x10` protocol version 1.0 |
| 2 | 2 | service type (big endian) |
| 4 | 2 | total length incl. header (big endian) |

Service types:

| Value | Name | Direction |
|---|---|---|
| `0x0201` | `SEARCH_REQUEST` | out |
| `0x0202` | `SEARCH_RESPONSE` | in |
| `0x0203` | `DESCRIPTION_REQUEST` | out |
| `0x0204` | `DESCRIPTION_RESPONSE` | in |
| `0x0205` | `CONNECT_REQUEST` | out |
| `0x0206` | `CONNECT_RESPONSE` | in |
| `0x0207` | `CONNECTIONSTATE_REQUEST` | out |
| `0x0208` | `CONNECTIONSTATE_RESPONSE` | in |
| `0x0209` | `DISCONNECT_REQUEST` | out |
| `0x020A` | `DISCONNECT_RESPONSE` | in |
| `0x0320` | `TUNNELLING_REQUEST` | both |
| `0x0321` | `TUNNELLING_ACK` | both |
| `0x0330` | `ROUTING_INDICATION` | in |
| `0x0331` | `ROUTING_LOST_MESSAGE` | in |
| `0x0332` | `ROUTING_BUSY` | in |

Unknown service types are **ignored**, not errors (routers emit extras).

### 5.2 HPAI (Host Protocol Address Information, IPv4/UDP = 8 bytes)

| Offset | Size | Value |
|---|---|---|
| 0 | 1 | structure length `0x08` |
| 1 | 1 | host protocol `0x01` = IPv4/UDP (`0x02` = IPv4/TCP) |
| 2 | 4 | IPv4 address (network order) |
| 6 | 2 | port (big endian) |

`0.0.0.0:0` is valid in a request ("let the device pick / I am behind NAT").

### 5.3 cEMI frames

```
[0] message code          0x11 L_Data.req | 0x29 L_Data.ind | 0x2E L_Data.con
[1] additional info length 0x00 (standard frame)
[2] control field 1
[3] control field 2
[4..5] source address (individual address)
[6..7] destination address (group or individual)
[8] length = (number of APDU octets) - 1
[9..] APDU (TPCI/APCI + data)
```

Control field 1 bits: `b7` frame type (1 = standard), `b6` reserved 0,
`b5` repeat (1 = do not repeat), `b4` system broadcast, `b3..b2` priority
(00 system, 01 normal, 10 urgent, 11 low), `b1` ack requested, `b0` confirm/error.
Control field 2 bits: `b7` destination type (1 = group), `b6..b4` hop count,
`b3..b0` extended frame format.

Canonical values used when sending: control 1 = `0xBC`, control 2 = `0xE0`
(standard frame, repeat not allowed, system broadcast, low priority, group
destination, hop count 6).  Received frames must be parsed generically — never
compare against `0xBC`/`0xE0` for validity.

**Only `L_Data.ind` (0x29) is fed into the cache.**  `L_Data.con` (0x2E) is a
transport acknowledgement of *our own* write and must be dropped, otherwise
every write would be delivered twice (see AGENTS.md rule 1).  This is a
mandatory test in P2.

Group-value APDU (identical to the current `parse_apdu()` input):

| APDU | Meaning |
|---|---|
| `00 00` | GroupValueRead |
| `00 80 <v>` | GroupValueWrite, 1-byte value `v` (6-bit) |
| `00 80 <b1> <b2>` | GroupValueWrite, 2-byte value |
| `00 40 <v>` | GroupValueResponse |

Note: for cEMI the length byte is `apdu.size() - 1`.

### 5.4 Tunneling handshake

`CONNECT_REQUEST` (total 25 bytes) body:

```
HPAI control (8) | HPAI data (8) | connection type 0x04 (tunnel)
                                  | KNX layer 0x02 (link layer)
                                  | reserved 0x00
```

`CONNECT_RESPONSE` (total 20 bytes) body:

```
channel id (1) | status (1) | HPAI data (8) | 0x04 0x04 <IA hi> <IA lo>
```

The individual address in the CRD is the address the client must use as
source address in `L_Data.req`; `0x00 0x00` means "let the device fill it in".

Status codes: `0x00` no error, `0x01` host protocol type, `0x02` version,
`0x03` sequence number, `0x04` connection id, `0x05` connection type,
`0x06` connection option, `0x07` **no more connections**, `0x08` no more unique
connections, `0x09` data connection, `0x0A` KNX connection, `0x0B` tunneling
layer.  `0x07`/`0x08` must produce a clear `[ERROR]` log line naming the
tunnel limit (this is the case the operator will hit).

`CONNECTIONSTATE_REQUEST` (total 16): `channel | 0x00 | HPAI control`.
`CONNECTIONSTATE_RESPONSE` (total 8): `channel | status`.

`DISCONNECT_REQUEST` (total 16): `channel | 0x00 | HPAI control`.
`DISCONNECT_RESPONSE` (total 8): `channel | status`.

### 5.5 Tunneling data + sequence numbers

`TUNNELLING_REQUEST` body: `0x04 | channel | sequence | 0x00 | cEMI…`
(total = 10 + cEMI length).
`TUNNELLING_ACK` body: `0x04 | channel | sequence | 0x00 | status` (total 11).

Rules:

- One sequence counter for *all* frames sent on the channel (`0..255`, then 0).
  The tunnel owner is the only sender (hence §3.3), which makes this simple.
- Every received `TUNNELLING_REQUEST` is ACKed with its **own** sequence number
  before/while it is processed.
- **A frame whose sequence number is not the expected one is ACKed and
  discarded.**  Duplicate delivery of a toggle/scene/step command is a
  correctness bug (AGENTS.md rule 1) — this must be tested explicitly.
- A missing ACK leads to retransmission with the **same** sequence number up to
  `KNX_IP_RETRIES` times, then the telegram is dropped and logged.
- `L_Data.con` for our own write is dropped (§5.3).

### 5.6 Heartbeat and reconnect

- Send `CONNECTIONSTATE_REQUEST` every `KNX_IP_HEARTBEAT_SEC` (default 60 s;
  devices drop the tunnel after 120 s without traffic).
- Any `CONNECTIONSTATE_RESPONSE` with status `0x04` (unknown channel id) or a
  send error means the tunnel is gone → reconnect with the existing
  exponential backoff (500 ms … 30 s) from `run_cache_reader()`.
- The reconnect must reuse the reader loop: `wait_for_activity()` returns
  `Error`, the loop reconnects, exactly as with knxd.  No new spin loop.
- Publish availability through `SharedGroupCache::publish_bus_status()`
  so `/r` and `/w` report the outage (unchanged JSON).

### 5.7 Routing

- Join `KNX_IP_MULTICAST` (`224.0.23.12`) on `KNX_IP_IFACE` (default: any).
- `ROUTING_INDICATION` body = bare cEMI frame (no channel, no sequence, no ACK).
- Sending: one `sendto()` per telegram with `IP_MULTICAST_TTL` = 16 and loop
  enabled for testability.
- No connection state.  Availability is derived from `sendto()` errors
  (`ENETUNREACH`, `ENODEV`, …) and `ROUTING_LOST_MESSAGE`; document the
  pragmatic behaviour in AGENTS.md rather than inventing a heartbeat.
- Routing is **fire-and-forget**: `/w` returns 200 when `sendto()` succeeded.

### 5.8 Discovery (P7)

- `SEARCH_REQUEST` to `224.0.23.12:3671` from the control socket; collect
  `SEARCH_RESPONSE`s for a bounded window (e.g. 1 s); parse the device hardware
  DIB (name, individual address, multicast address) for logging and for
  `--help`-style diagnostics.
- Used only for diagnostics and for "no `KNX_IP_HOST` configured, pick the
  single device found".  Never required for operation.

---

## 6. Phases

Estimates assume one experienced C++ developer working TDD-first with this
repo's test standard.  "LOC" is production code only; tests are roughly the
same size again.

### P0 — Rename leftovers (0.5–1 day)

Cleanup that the project rename (`cometvisu-knxd-fcgi` → `cometvisu-knx-fcgi`)
did not cover, so later phases start from consistent naming.

1. **Test first:** no behaviour test — this is a compile-and-grep gate.  Add a
   CI step (`grep -rn "cvknxd\|COMETVISU_KNXD_FCGI"` must return nothing)…
   or, simpler and less brittle: do the rename and rely on the existing suite.
2. Rename namespace `cvknxd` → `cvknx`, CMake targets `cvknxd_*` → `cvknx_*`,
   compile definition `CVKNXD_BINARY` → `CVKNX_BINARY`, include guards
   `COMETVISU_KNXD_FCGI_*` → `COMETVISU_KNX_FCGI_*`.
3. Convert the include guards to `#pragma once` (AGENTS.md already mandates
   `#pragma once`; the code contradicts the rule).
4. Make `knxd-dev` / `eibtypes.h` **optional**: a `KNX_BUILD_KNXD_TRANSPORT`
   option (default `ON`) guards `find_path(eibtypes.h)` and the
   `cvknx_knxd` library.  When `OFF`, the build must succeed with no knxd
   headers present.  **Test:** configure+build in a directory with
   `-DKNX_BUILD_KNXD_TRANSPORT=OFF` in CI.

**Acceptance:** all 27+ tests green; a build with `-DKNX_BUILD_KNXD_TRANSPORT=OFF`
succeeds; no `cvknxd` string left in `src/`, `tests/`, `mocks/`.

### P1 — Transport seam and configuration (1–2 days)

1. **Tests:** `tests/unit/test_bus_config.cpp`
   - `KNX_TRANSPORT` unset → error naming the three valid values (exit 1, no default)
   - `KNX_TRANSPORT=knxd` → `BusConfig::transport == Knxd`
   - `ip-tunnel` / `ip-routing` parsed; unknown value → error + `400`-style
     startup failure (log `[ERROR]` and exit 1)
   - `BusConfig::outage_key()` → `"knxd"` for knxd, `"bus"` for both ip transports (§8 Q2)
   - numeric parsing: port range, heartbeat ≥ 10, retries ≥ 1; invalid value →
     error, never a silent default
   - `KNX_IP_HOST` required for `ip-tunnel`, ignored otherwise
2. Rename `KnxdClientInterface` → `BusLink` (`src/bus/bus_link.h`),
   `MockKnxdClient` → `MockBusLink` (`mocks/mock_bus_link.h`), member `knxd_` →
   `bus_` in the handlers, `open_group_socket()` out of the interface.
   Existing tests must keep passing **unchanged in intent** (mechanical renames
   only).  `KnxdBusLink` = today's `KnxdClient`, constructed with a
   `BusConfig`.
3. `BusFactory` (`src/bus/bus_factory.{h,cpp}`): transport → `BusLink`.
4. `main.cpp` / `run_cache_reader()`: take `BusConfig` instead of
   `socket_path`; log the effective configuration.
5. `knxd_status_json`: rename `add_knxd_outage()` → `add_bus_outage()` and
   `publish_knxd_status()` / `knxd_status()` → `publish_bus_status()` /
   `bus_status()` (all transports use them now), the JSON key coming from
   `BusConfig::outage_key()`.  **Tests:** `tests/unit/test_knxd_status_json.cpp`
   covers both keys with the same body shape.
6. Update AGENTS.md (file table: `src/bus/`, `src/knxnetip/`), README env table
   (mark `KNX_TRANSPORT` as required, add a migration note), CHANGELOG 2.0.0.

**Acceptance:** `KNX_TRANSPORT=knxd` reproduces today's behaviour exactly and
**all existing behaviour tests pass unchanged**; unset or unknown values fail at
startup; `--help` lists the new variables and marks `KNX_TRANSPORT` mandatory.

### P2 — cEMI codec (2–3 days)

Pure functions, no sockets.  `src/knxnetip/cemi.{h,cpp}`.

1. **Tests:** `tests/unit/test_cemi.cpp`, one test per rule:
   - `EncodeGroupWriteOneByte` → `11 00 BC E0 <src hi> <src lo> <dst hi> <dst lo> 02 00 80 42`
   - `EncodeGroupWriteTwoBytes` (DPT 9.001, `0c6f`) → data length `03`, APDU `00 80 0C 6F`
   - `EncodeGroupValueRead` → `… 01 00 00`
   - `EncodeEmptyApduRejected` (length byte would underflow)
   - `ParseLDataIndGroupWrite` → address + APDU + `is_group == true`
   - `ParseLDataIndIgnoresIndividualDestination` (returns "not for us")
   - `ParseTruncatedFrameRejected` (every prefix length 0…n-1)
   - `ParseWrongMessageCodeRejected` (0x2E `L_Data.con`, 0x2F busmonitor)
   - `SourceAddressUsesCrd` and `SourceAddressZeroWhenCrdAvailable` semantics
2. Implement `encode_group_apdu(...)`, `parse_cemi(...)` returning
   `std::optional<CemiFrame>`; reject rather than throw.
3. Document the bit layouts in the header (§5.3).

**Acceptance:** the codec is total (never crashes, never reads out of bounds)
and covered by the vectors above; no allocation in `parse_cemi` beyond the
returned vector.

### P3 — KNXnet/IP framing (1–2 days)

`src/knxnetip/knxnetip_frame.{h,cpp}` — header, HPAI, service-specific builders
and parsers.

1. **Tests:** `tests/unit/test_knxnetip_frame.cpp`
   - `HeaderRoundTrip` for every service type
   - `RejectsWrongHeaderLength` / `RejectsUnknownVersion` / `RejectsLengthMismatch`
   - `HpaiEncodeDecode` (incl. `0.0.0.0:0`)
   - `ConnectRequestBytes` → exact 25-byte vector
   - `ParseConnectResponseOk` / `…ErrorNoMoreConnections` (`0x07`)
   - `ConnectionStateRequestBytes` (16) / `ParseConnectionStateResponse` (8)
   - `TunnelingRequestBytes` (10 + cEMI) / `ParseTunnelingAck` (11)
   - `ParseRoutingIndication` → cEMI slice
   - `IgnoresUnknownServiceType`
2. Implement build/parse helpers; all lengths derived from `data.size()`, never
   hardcoded per call site.

**Acceptance:** every builder produces bytes matching the vectors; every parser
validates lengths before reading.

### P4 — Shared TX queue and single-owner refactor (3–4 days)

Prerequisite for **every** transport (§8 Q4).  Refactor only — no new protocol.
After this phase no worker process owns a bus connection of any kind: workers
enqueue, the cache reader sends.

1. **Tests:** `tests/unit/test_shared_tx_queue.cpp`
   - `PushPopFifo`, `FifoAcrossAttachedInstances` (two `attach()` views)
   - `FullQueueRejectsPush` (returns `false`, queue content unchanged)
   - `PayloadTooLongRejected` (> 14 bytes)
   - `SignalEventfdIncrements` (read of the eventfd yields the pushed count)
   - Robust-mutex recovery: owner dies while holding the lock (`EOWNERDEAD`)
2. `SharedTxQueue` in `src/state/shared_tx_queue.{h,cpp}`; created in
   `main.cpp` before the forks, passed to workers and to the cache reader.
3. `SharedTxBusLink : BusLink` for workers (enqueue + eventfd signal; never
   opens a socket).  `send_group_packet()` returns `false` when the queue is
   full **or** the published bus status is unavailable.
4. `run_cache_reader()`: poll `{link_fd, tx_eventfd}`, drain the queue first,
   then receive.  **Tests:** `tests/unit/test_cache_reader.cpp` gains
   - `DrainsTxQueueBeforePolling`
   - `TxQueueDrainedWhenLinkIdle` (eventfd wakes the loop, no 1 s latency)
   - `TxDuringOutageIsRejectedNotQueued`
5. **Switch knxd onto the queue** (§8 Q4): the cache reader opens the single
   read-write group socket (`open_group_socket()` becomes internal to
   `KnxdBusLink::connect()`); workers get `SharedTxBusLink` and never open a
   socket again.  `/w` failure semantics are unchanged: full queue or an
   unavailable link → `false` → `503`.  If the write-only mode of
   `open_group_socket()` is now dead code, delete it.
   **Tests:** `tests/integration/test_write_handler.cpp` gains
   `KnxdWriteGoesThroughTxQueue` (the worker owns no fd) and
   `KnxdWriteFailsWhenQueueFull`; `test_knxd_client.cpp` asserts exactly one
   knxd connection per process after startup.
6. Rewrite AGENTS.md design decisions 2, 4 and 8 in this PR — they describe the
   old per-worker socket model.

**Acceptance:** `/w` latency budget unchanged (< 100 ms observed in
`test_concurrent_clients`); the cache reader still never busy-loops; a running
knxd sees exactly **one** client connection regardless of `FCGI_WORKERS`.

### P5 — IP routing transport (2–3 days)

Smallest end-to-end win on the new model: no connection, no sequence numbers,
no heartbeat.  Workers already use `SharedTxBusLink` (P4), so only the cache
reader has to learn the transport.

1. **Tests:** `tests/unit/test_knxnetip_routing.cpp` with a loopback UDP peer on
   an ephemeral port (never 3671 — CI must not depend on a fixed port):
   - `SendsRoutingIndicationWithCemi` (peer receives exactly one datagram, bytes match P2 vector)
   - `ReceivesRoutingIndication` → `poll_group_telegram()` yields address + APDU
   - `IgnoresOwnEcho` (documented policy: echoes are delivered; dedupe is the
     handler's job — assert the current contract explicitly)
   - `SendFailsWhenSocketBroken` → `false`, no exception
   - `DoesNotSpinWhenSocketSilent` (measure CPU: `wait_for_activity(200)` returns
     `Timeout` after ≈200 ms, using `steady_clock`)
2. `RoutingBusLink : BusLink` — **one** UDP socket owned by the cache reader:
   joined to `KNX_IP_MULTICAST` (`SO_REUSEADDR`) for RX, and used for `sendto()`
   with `IP_MULTICAST_TTL` = 16; `get_fd()` = that socket.
3. Config plumbing (`KNX_IP_MULTICAST`, `KNX_IP_IFACE`, `KNX_IP_PORT`).
4. E2E test (`tests/e2e/test_routing_e2e.cpp`): start the binary with
   `KNX_TRANSPORT=ip-routing` against a fake router on loopback, do `/w`, assert
   the datagram, inject a `ROUTING_INDICATION`, assert `/r` returns it with a
   monotonically increasing `i` and that the outage key is `"bus"`.

**Acceptance:** an installation can run `/l`, `/r`, `/w` with
`KNX_TRANSPORT=ip-routing` and **no knxd installed**; interop smoke test against
`knxd` in `ip:` mode documented in `tools/`.

### P6 — KNXnet/IP tunneling client (4–6 days)

The main work item.  `src/knxnetip/tunneling_bus_link.{h,cpp}`.

1. **Tests:** `tests/unit/test_knxnetip_tunnel.cpp` against a fake router
   (`tests/fakes/fake_knxnetip_router.{h,cpp}`, UDP on loopback, scriptable):
   - `ConnectSendsConnectRequestAndParsesChannel` (asserts exact bytes, channel id, IA)
   - `ConnectMapsNoMoreConnectionsToError` (status `0x07` → clear error)
   - `ConnectTimesOutAfterDeadline` (no reply → fails within the deadline, no hang)
   - `SendIncrementsSequenceAndWaitsForAck`
   - `RetransmitsSameSequenceOnMissingAck` (`KNX_IP_RETRIES`, same seq)
   - `AcksAndDiscardsDuplicateInbound` (same seq twice → cache sees it once)
   - `AcksOutOfOrderInboundAndDiscards`
   - `SequenceWraps255To0`
   - `DropsLDataCon` (own write confirmation must not reach the cache)
   - `SendsHeartbeatAfterInterval` (fake router with 10 ms heartbeat, asserts
     `CONNECTIONSTATE_REQUEST`, answers, asserts connection kept)
   - `ReconnectsAfterConnectionStateError` (`0x04` → reconnect with backoff)
   - `DisconnectSendsDisconnectRequest` (best effort, ignores failure)
   - `NoBusyLoopWhenSocketSilent` (CPU guard as in P5)
2. Implement the session state machine: connect → run (heartbeat timer, RX
   dispatch, ACK) → disconnect; `wait_for_activity()` handles the heartbeat
   deadline internally (never blocks past the next heartbeat).
3. Wire into `BusFactory` and `main.cpp` (`KNX_TRANSPORT=ip-tunnel`, single
   tunnel in the cache reader, workers use `SharedTxBusLink`).
4. E2E test: full FCGI cycle with `KNX_TRANSPORT=ip-tunnel` against the fake
   router, including one simulated router restart.
5. Interop: document a manual/CI-optional test against a real device and
   against `knxd` used as tunnel **client** counterpart (`iptn:`).

**Acceptance:** `/l`, `/r` (long-poll), `/w` work over a tunnel; a router
restart is survived without restarting the backend; the tunnel limit is
respected (`FCGI_WORKERS` has **no** influence on the number of tunnels).

### P7 — Discovery and diagnostics (1–2 days)

`SEARCH_REQUEST`/`SEARCH_RESPONSE`, `DESCRIPTION_*`, logging of device name,
individual address and multicast address.  Optional auto-selection when
exactly one device answers and `KNX_IP_HOST` is unset.  Tests: fake router
answers with a fixed DIB; assert parse + log line; two answers and no
`KNX_IP_HOST` → error asking for explicit configuration.

### P8 — Docs, interop, release (2–3 days)

- README: three equally supported transports, a "no knxd needed" quick start,
  the **required** `KNX_TRANSPORT` with the migration line
  (`KNX_TRANSPORT=knxd` restores 1.x behaviour), container hints (UDP 3671 must
  be reachable; multicast usually needs host networking).
- AGENTS.md: transport abstraction and the no-branching rule, shared TX queue /
  single-owner model, cEMI rules (the `L_Data.con` trap), tunnel-limit rule,
  outage-key semantics (`knxd` vs `bus`).
- CHANGELOG: **`2.0.0`** entry with an explicit *Breaking changes* section
  (required `KNX_TRANSPORT`, `"bus"` outage key for the ip transports); bump
  `VERSION`.
- `tools/` scripts: start a fake router, run a routing interop test against
  `knxd`.
- Optional CI job that builds with `-DKNX_BUILD_KNXD_TRANSPORT=OFF`.

---

## 7. Test strategy

- **All unit tests use loopback UDP on an ephemeral port.**  Never bind 3671 in
  a test, never require multicast-capable networking in CI.
  Routing keepalive tests use a configurable "multicast" address that is
  actually `127.0.0.1` where the kernel permits, otherwise skip with a clear
  `GTEST_SKIP()` message.
- **Hex vectors are the contract.**  Every builder test asserts the complete
  byte string, not individual offsets, so a wrong length field cannot slip
  through.
- **Deterministic timing.**  Interval-based behaviour (heartbeat, ACK timeout,
  backoff) must be configurable down to milliseconds through the options
  struct (as `CacheReaderOptions` already does) — never `sleep()` in a test.
- **CPU guard.**  Every "wait" primitive gets a test that asserts
  `steady_clock` elapsed time and (where practical) that the process is not
  spinning.
- **Fake router** (`tests/fakes/`) is a scriptable UDP peer:
  `expect(service_type)`, `reply(bytes)`, `send(bytes)`, `drop_next(n)`.
  It must never be able to hang a test: every wait is bounded.

---

## 8. Decisions (locked 2026-11-15)

These were open questions; they are now decided and binding for every phase.
Do not re-open them during implementation — if a phase cannot satisfy one, stop
and raise it instead of silently diverging.

| # | Question | Decision |
|---|---|---|
| Q1 | Relative importance / default transport | **All three transports are equally supported and none is a default.** `knxd`, `ip-tunnel` and `ip-routing` are first-class; the operator selects one via `KNX_TRANSPORT`. Unset or unknown → `[ERROR]` + exit 1, never a silent fallback. |
| Q2 | Client-facing outage key | Transport-dependent: `"knxd"` when `KNX_TRANSPORT=knxd` (keeps existing CometVisu deployments unchanged), `"bus"` for `ip-tunnel` and `ip-routing`. Inner shape identical (`ok` / `since` / `lastOk`); produced by one `add_bus_outage(json, status, key)` from `BusConfig::outage_key()`. |
| Q3 | KNX IP Secure | Out of scope. Keep the frame-codec seam so a `SecureWrapper` can wrap it later without touching any transport. |
| Q4 | Shared TX queue for all transports? | **Yes — all three.** Workers never own a bus connection; the cache reader is the single owner of RX *and* TX. Removes the `FCGI_WORKERS + 1` knxd slot requirement (→ 1), per-worker `open_group_socket()`, and the per-worker send sockets routing would otherwise need. `/w` failure semantics are unchanged (`false` → `503`). |
| Q5 | Fewer forked workers for tunneling? | No. One tunnel serves all workers through the TX queue. |
| Q6 | Routing availability detection | `sendto()` errors + `ROUTING_LOST_MESSAGE`; do not fabricate a heartbeat. |

Consequences baked into the phases above: Q1 + Q2 are **breaking** (required
variable, new JSON key) → release **2.0.0**; Q4 moves the shared TX queue to
**P4**, before the first new transport (P5).

---

## 9. Definition of done (every phase)

- [ ] New tests fail before the implementation and pass after it.
- [ ] `ctest --test-dir build --output-on-failure` green (gcc **and** clang).
- [ ] Release build green (`-DCMAKE_BUILD_TYPE=Release`).
- [ ] `clang-format -i` applied; no new `clang-tidy` findings in touched files.
- [ ] No busy loop anywhere (test or reasoning documented in the PR).
- [ ] AGENTS.md / README.md / CHANGELOG.md updated in the same PR.
- [ ] No new external dependency.
- [ ] Handlers and FCGI layer untouched (or the deviation justified).

---

## 10. Effort summary

| Phase | Deliverable | Production LOC | Days |
|---|---|---|---|
| P0 | naming cleanup, optional knxd dep | ~50 | 0.5–1 |
| P1 | `BusLink` seam, config, factory | ~150 | 1–2 |
| P2 | cEMI codec | 300–400 | 2–3 |
| P3 | KNXnet/IP framing | 200–250 | 1–2 |
| P4 | shared TX queue + single owner (+ knxd switch) | 250–350 | 3–4 |
| P5 | IP routing transport | 150–250 | 2–3 |
| P6 | tunneling session | 250–350 | 4–6 |
| P7 | discovery/diagnostics | 200 | 1–2 |
| P8 | docs, interop, release | — | 2–3 |
| **Σ** | | **~1,500–2,000** + tests | **15–25** |

Deleting nothing: the knxd transport stays.  The net code volume grows by
roughly the size of `src/knxd/` (~1,400 LOC) — the gain is architectural
(one transport interface, no daemon, no client-slot limit) and operational,
not a line-count win.  The one genuinely *simpler* mode is `ip-routing`.

---

## 11. References

- KNXnet/IP specification (KNX Association, chapter 3/8): header, HPAI,
  service types, tunneling sequence rules.
- [knxd](https://github.com/knxd/knxd) — reference implementation, including
  its `ip:` (routing) and `iptn:` (tunneling) backends.
- [calimero-core](https://github.com/calimero-project/calimero-core) —
  readable Java implementation of the same frames.
- [xknx](https://github.com/XKNX/xknx) (Python) — good for cross-checking hex
  vectors.
- [CometVisu protocol](https://github.com/CometVisu/CometVisu/wiki/Protocol) —
  the client contract that must not change.
