# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Breaking changes

- **`KNX_TRANSPORT` is now mandatory.**  There is no default transport: choose
  `knxd`, `ip-tunnel` or `ip-routing`.  An unset or unknown value stops the
  backend at startup with `[ERROR] KNX_TRANSPORT is required …` instead of
  silently falling back to knxd — a wrong `ip-*` setup must not look like an
  outage.
  **Migration:** add `KNX_TRANSPORT=knxd` to keep 1.x behaviour unchanged.
- **The project is renamed** from `cometvisu-knxd-fcgi` to
  `cometvisu-knx-fcgi` (<https://github.com/CometVisu/cometvisu-knx-fcgi>).
  Update `spawn-fcgi`, container and systemd invocations that start the binary
  by path; a `cometvisu-knxd-fcgi` symlink keeps existing setups working.
- **The outage block in `/r` and `/w` uses a transport-specific key**: `"knxd"`
  for the knxd transport (unchanged) and `"bus"` for the KNXnet/IP transports.
  The inner object (`ok`, `since`, `lastOk`) is identical.

### Added

- **Direct KNXnet/IP tunnelling** (`KNX_TRANSPORT=ip-tunnel`): the backend is a
  client of a KNX IP interface or router — no knxd required.  Includes the
  tunnel handshake, acknowledgement and retransmission, rejection of duplicate
  and out-of-order frames, keep-alive, and automatic reconnect after a device
  restart or tunnel loss.
- **Direct KNXnet/IP routing** (`KNX_TRANSPORT=ip-routing`): the backend joins
  the routing multicast group (default `224.0.23.12`) and works with **no knxd
  installed**.
- **`--discover`**: sends a KNXnet/IP SEARCH_REQUEST and lists every answering
  device with its individual and multicast address.

### Changed

- **One bus connection regardless of `FCGI_WORKERS`.**  All workers share the
  single bus link owned by the cache reader, so the worker count no longer
  multiplies knxd client slots (one instead of `FCGI_WORKERS + 1`) or KNXnet/IP
  tunnels (one instead of one per worker — a KNX IP interface usually offers
  only four).
- **A write during a bus outage is reported as failed** (`503`) instead of
  being queued: the client learns immediately that the telegram did not leave,
  rather than receiving `200`.  A telegram that was accepted but cannot be sent
  stays queued and is delivered after the reconnect.
- **knxd is optional at build time.**  `-DKNX_BUILD_KNXD_TRANSPORT=OFF` builds
  a binary without any knxd header or daemon.

### Fixed

- **A `POST`/`PUT` request could terminate a worker** in direct-socket mode: the
  body length was taken from the wrong stream and an overflowing value threw
  out of the request loop.  The worker died (and was restarted), the request was
  lost.
- **The cache reader could stop reconnecting** when writes were in flight, so
  queued telegrams stayed stuck until a restart.
- **A permanently failing send could spin the cache reader at 100 % CPU.**
- **A telegram that failed to send was dropped.**  It now stays queued and is
  sent after the reconnect — a write is never lost silently.
- **A rejected knxd connection leaked a file descriptor** per attempt when
  `KNXD_SOCKET` was misconfigured, eventually exhausting the process.

## [1.2.1] — 2026-11-15

### Fixed

- **Startup log lines were repeated once per worker process.**  When stdout is
  not a terminal — the normal case in a container — the parent's log output is
  buffered.  Each worker forked with those lines still pending, inherited a copy
  of the buffer and printed it again, so `docker logs` showed the startup banner
  up to `FCGI_WORKERS` times.  All forks now flush the buffered output first
  (`src/util/fork_safety.{h,cpp}`, covered by `test_fork_safety`).
- **Log lines of concurrently starting processes could interleave.**  The worker
  and cache-reader warnings are now emitted as one line each, so ten starting
  workers can no longer produce output like `[WARN] [WARN] Worker pid=Cannot
  connect …`.

## [1.2.0] — 2026-11-14

### Added

- **`/r` and `/w` report a knxd outage to the client.**  A dead knxd used to be
  invisible over HTTP: `/r` kept answering `200` with the last known — but
  frozen — values, and only `/w` failed, so a CometVisu client could not tell
  "the bus is quiet" from "the connection is gone".  The cache reader now
  publishes its connection state (available / last successful connection /
  start of the current outage) in the shared memory region, and both handlers
  append a block while knxd is unreachable:

  ```json
  {"d":{...},"i":42,"knxd":{"ok":false,"since":1763000000,"lastOk":1762990000}}
  {"error":"write failed","knxd":{"ok":false,"since":1763000000,"lastOk":1762990000}}
  ```

  The field is absent while knxd is reachable (no overhead in the normal case).
  `since` is the Unix time the current outage started, `lastOk` the Unix time of
  the last successful connection; both are omitted when unknown (knxd never
  connected since startup, or the outage start was not recorded yet).
  A read is not successful while the bus connection is gone, so `/r` answers
  `503` — and it answers immediately instead of waiting for the long-poll
  timeout, because nothing can arrive.  The body still carries the last known
  values, so a client may keep showing them and add a "connection lost"
  marker.  `/w` answers `503` with `{"error":"write failed",...}`.
  `JsonBuilder` gained `add_bool()` for the `ok` literal.

### Fixed

- **The router dropped the `/w` response body.**  `Router::route()` only copied
  `http_status` for `/w`, so error details never reached the client.
- **Configurations with more than 250 group addresses lost all values above
  address ~250** ([CometVisu#1521](https://github.com/CometVisu/CometVisu/issues/1521)).
  `QueryString` capped the number of values per key at 250 (and total key/value
  pairs at 256) and silently dropped the excess.  Since CometVisu subscribes
  every address of a visualization in one request, no `GroupValueRead` was sent
  for the dropped addresses and the long-poll filter never matched them — the
  corresponding tiles stayed empty forever (independent of the order in the
  config).  The limits are now `kMaxValuesPerKey = 4096`,
  `kMaxTotalPairs = 8192` and `kMaxQueryParams = 256`, and they are public
  constants on `QueryString`.

### Changed

- **Query-string limit violations are no longer silent.**  `QueryString`
  reports truncation via `limit_exceeded()`; `/r` and `/w` answer with
  `400 {"error":"too many addresses"}` instead of returning (or writing) an
  incomplete address list.

### Tests

- **A CI job can no longer hang on a leaked test worker.**  The fork-based
  tests (`test_fork_workers`, `test_concurrent_clients`) left forked workers
  behind on early assertion failures and terminated them with `SIGTERM`,
  which a worker blocked in `FCGX_Accept_r()` may ignore.  A surviving worker
  keeps the pipe open that ctest uses to capture test output, so ctest waits
  forever for an EOF that never comes and the job sits at
  `Start 23: test_fork_workers` for hours.  Workers now die with their parent
  (`PR_SET_PDEATHSIG`), are killed with `SIGKILL` and reaped per pid, and are
  tracked so `TearDown()` reaps them even after a failed `ASSERT`.
- `test_fork_workers`'s fork-failure test no longer lowers `RLIMIT_NPROC`
  (which counts the tasks of the whole real UID and therefore behaves
  completely differently on a CI runner than on a developer machine); the
  fork primitive is injected instead, making the failure point deterministic.
  A new test `WorkersDieWithTheirParent` covers the orphan scenario.
- CI runs `ctest --timeout 120` and the job has a `timeout-minutes`
  backstop, so a wedged test fails within seconds instead of blocking the
  workflow.

## [1.1.0] — 2026-11-01

### Fixed

- **Cache reader burned 100% CPU when knxd was missing.**
  `KnxdClient::wait_for_activity()` reported "not connected" and
  `POLLHUP`/`POLLERR`/`POLLNVAL` as a plain timeout, so the cache reader
  loop retried `poll()` in a tight userspace loop and never reconnected —
  `/r` kept returning stale values forever while one CPU core was saturated.
  `WaitResult` now has an `Error` state and the loop (new
  `run_cache_reader()` in `src/knxd/cache_reader.cpp`) reconnects forever
  with an exponential backoff (500 ms … 30 s, rate-limited logging).
- The daemon no longer refuses to start (or exits) when knxd is unavailable:
  parent, cache reader and workers log a warning and retry in the background,
  so knxd may be restarted at any time without restarting the backend.
- `KnxdClient::connect()` now remembers the socket path even when the attempt
  fails, so `reconnect()` can recover once knxd is back.
- **A `/w` request can no longer block forever on the knxd handshake.**
  `open_group_socket()` read the `EIB_OPEN_GROUPCON` reply from a blocking
  socket without a deadline, so a knxd that accepted the connection but never
  answered hung the worker (and with it the request) indefinitely.  The reply
  is now read with a deadline (default 5 s, `set_handshake_timeout_ms()`).
- **Writes and logins are no longer rejected with 503.** The concurrency and
  load-shedding semaphores applied to `/w`/`/l` as well, so a pool full of
  long-polls made switch operations fail.  Both now only guard `/r`.
  Tokens are released via RAII (also when a handler throws — that used to
  terminate the worker process; it now returns HTTP 500), and a worker that
  dies restores its token instead of permanently shrinking the pool.
- **The parent supervisor restarts dead children.** The SIGCHLD handler used
  to `waitpid()` all children itself, so the supervisor never saw an exit
  status: a crashed worker was never replaced and a dead cache reader was
  never restarted (leaving `/r` on stale data forever).  The parent is now the
  only reaper, keeps the listen socket open for replacements, restarts workers
  and the cache reader with a crash-loop backoff, and no longer ignores
  SIGTERM while blocked in `wait()` (`sigaction()` without `SA_RESTART`).
- `FcgiRequest::path()` falls back to the last component of `SCRIPT_NAME` when
  no `REQUEST_URI` is passed (pure-CGI setups such as `cgi-fcgi`).

## [1.0.0] — 2026-07-05

### Added

- Initial release of cometvisu-knxd-fcgi.
- FastCGI backend implementing the CometVisu Protocol.
- `/l` (login) endpoint with session management.
- `/r` (read) endpoint with long-poll support via `poll()`.
- `/w` (write) endpoint for sending KNX telegrams.
- knxd Unix socket client with eibd binary protocol support.
- Address cache integration with knxd's built-in cache.
- Configurable knxd socket path (`KNXD_SOCKET` env var).
- Configurable long-poll timeout (`LONGPOLL_TIMEOUT_SEC` env var).
- Debug logging via `DEBUG_BACKEND` environment variable.
- JSON response builder (no external JSON library dependency).
- Query string parser.
- Comprehensive test suite: unit, integration, and end-to-end tests.
- CMake build system with Google Test integration.
- GPLv3 licensed.
