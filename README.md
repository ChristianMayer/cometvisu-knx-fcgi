# cometvisu-knx-fcgi

FastCGI backend in modern C++ that implements the
[CometVisu Protocol](https://github.com/CometVisu/CometVisu/wiki/Protocol)
and connects to the KNX bus.

Today bus access goes through one of three transports, all equally supported:

| `KNX_TRANSPORT` | Transport                          | Needs                      |
|-----------------|------------------------------------|----------------------------|
| `knxd`          | knxd daemon over a Unix socket     | knxd running locally       |
| `ip-tunnel`     | KNXnet/IP tunnelling client        | a KNX IP interface/router  |
| `ip-routing`    | KNXnet/IP multicast routing member | a KNX IP router            |

`KNX_TRANSPORT` is **required** — there is no default, so a misconfiguration
can never be mistaken for a bus outage.  `ip-routing` and `ip-tunnel` need no
knxd at all; `-DKNX_BUILD_KNXD_TRANSPORT=OFF` builds a binary without any knxd
dependency.

> **Migrating from 1.x**: add `KNX_TRANSPORT=knxd` to the environment. The
> outage block in `/r`/`/w` is still `"knxd": {...}` for that transport and
> `"bus": {...}` for the KNXnet/IP transports.

> **Renamed**: this project was called `cometvisu-knxd-fcgi` up to and including
> v1.2.1.  The repository now lives at
> <https://github.com/CometVisu/cometvisu-knx-fcgi> and the binary is named
> `cometvisu-knx-fcgi`.  Deployments that start the binary by path (`spawn-fcgi`,
> container entrypoints) must use the new name; a symlink
> `cometvisu-knxd-fcgi -> cometvisu-knx-fcgi` keeps old setups working.

## Quick Start

```bash
# Prerequisites (knxd-dev is only needed for the knxd transport)
sudo apt install libfcgi-dev cmake g++
sudo apt install knxd-dev        # only for KNX_TRANSPORT=knxd

# Build (using system-installed knxd headers)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

### Building without the knxd-dev system package

If you build knxd from source, point CMake at your checkout:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DKNXD_SOURCE_DIR=/path/to/knxd
cmake --build build -j$(nproc)
```

The build expects to find `eibtypes.h` in `<KNXD_SOURCE_DIR>/include/`.

### Development container (recommended)

The repository ships a VS Code dev container with the complete toolchain, so no
local package installation is needed:

```bash
# In VS Code: "Dev Containers: Reopen in Container"
# or from a terminal (needs the devcontainer CLI):
devcontainer up --workspace-folder .
```

`.devcontainer/Dockerfile` installs g++/clang/cmake/ninja, clang-format,
clang-tidy, gdb, `libfcgi-dev`, `knxd` + `knxd-dev` (for the knxd transport) and
`tcpdump`/`socat`/`iproute2` for KNXnet/IP debugging.  The project is configured
and built automatically after the container is created; the existing
`.vscode` tasks and launch configurations work unchanged inside the container.

> For testing against a real KNX IP router, UDP 3671 and the routing multicast
> group `224.0.23.12` must be reachable from the container — on Linux add
> `"runArgs": ["--network=host"]` to `.devcontainer/devcontainer.json`.

### Run tests / Run the server

```bash
# Run tests
ctest --test-dir build --output-on-failure

# Only the black-box interoperability suites (needs knxd + knxtool)
ctest --test-dir build -L blackbox --output-on-failure

# Run via spawn-fcgi (traditional FastCGI mode)
KNX_TRANSPORT=knxd spawn-fcgi -p 9000 -n ./build/src/cometvisu-knx-fcgi

# Run standalone (direct socket — no spawn-fcgi needed)
KNX_TRANSPORT=knxd FCGI_SOCKET=:9000 ./build/src/cometvisu-knx-fcgi

# Direct KNXnet/IP connection, no knxd needed
KNX_TRANSPORT=ip-tunnel KNX_IP_HOST=192.168.1.10 FCGI_SOCKET=:9000 \
    ./build/src/cometvisu-knx-fcgi
KNX_TRANSPORT=ip-routing KNX_IP_IFACE=eth0 FCGI_SOCKET=:9000 \
    ./build/src/cometvisu-knx-fcgi

# Which devices answer?
./build/src/cometvisu-knx-fcgi --discover
```

### Code coverage

```bash
tools/coverage.sh                  # instrumented build + tests + gate
tools/coverage.sh --strict         # require 100 % lines and branches everywhere
tools/coverage.sh --list-uncovered # show what is still missing
```

CI measures coverage on **every pull request** (`.github/workflows/ci.yml`,
job `coverage`), publishes the per-file table in the workflow summary and fails
the PR on a regression or on new code that is not fully covered.

The gate is `tools/check_coverage.py`: it reads the gcov counters of every
translation unit and requires that each line and each branch arm of `src/` is
reached.  Files listed in `tools/coverage-baseline.json` may keep their
recorded (legacy) coverage but must never regress; any file that is not listed
must be fully covered.  Use a separate build directory — `tools/coverage.sh`
does that for you.

### Environment variables

| Variable              | Default    | Description                              |
|-----------------------|------------|------------------------------------------|
| `KNX_TRANSPORT`       | **required** | `knxd`, `ip-tunnel` or `ip-routing`. Unset or unknown → startup error. |
| `KNXD_SOCKET`         | `/run/knx` | knxd transport: path to the knxd Unix socket |
| `KNX_IP_HOST`         | *(required for `ip-tunnel`)* | Address of the KNX IP interface/router |
| `KNX_IP_PORT`         | `3671`     | KNXnet/IP UDP port                       |
| `KNX_IP_LOCAL_ADDR`   | *(OS choice)* | `ip-tunnel`: local address advertised in the HPAI (NAT friendly when unset) |
| `KNX_IP_MULTICAST`    | `224.0.23.12` | `ip-routing`: multicast group          |
| `KNX_IP_INDIVIDUAL_ADDR` | `15.15.250` | `ip-routing`: individual address used as the cEMI source of our routing telegrams (`A.L.D`). A router rejects `0.0.0`, so this must be a free address of the installation. |
| `KNX_IP_IFACE`        | *(default)* | `ip-routing`: interface for the group join / `IP_MULTICAST_IF` |
| `KNX_IP_NAT`          | `0`        | `ip-tunnel`: `1` = NAT mode              |
| `KNX_IP_HEARTBEAT_SEC`| `60`       | `ip-tunnel`: `CONNECTIONSTATE_REQUEST` interval (10–86400) |
| `KNX_IP_ACK_TIMEOUT_MS`| `1000`    | `ip-tunnel`: ACK timeout before retransmitting (10–60000) |
| `KNX_IP_RETRIES`      | `3`        | `ip-tunnel`: retransmissions per telegram (1–10) |
| `FCGI_SOCKET`         | *(unset)*  | Direct FCGI socket (`:port` for TCP, or filesystem path for Unix socket). When set, the server runs standalone without `spawn-fcgi`. |
| `FCGI_WORKERS`        | `20`       | Number of worker processes in direct socket mode (1–256). Each worker handles one concurrent client using `fork()`-based pre-forking. Idle workers cost ~few MB resident RAM each. Workers never open a bus connection of their own — they enqueue writes into the shared TX queue. |
| `LONGPOLL_TIMEOUT_SEC`| `300`      | Max seconds to wait in long-poll `/r`    |
| `BASE_URL`            | *(unset)*  | When set, the login (`/l`) response includes a config object `"c"` with `"baseURL"` set to this value. Used by the web server to advertise the URL prefix under which the backend is reachable (e.g. `/proxy/visu`). |
| `DEBUG_BACKEND`       | *(unset)*  | Set to `1` to enable debug logging to stderr |

At startup, the server logs the effective values of all configuration
environment variables to stdout:

```
[INFO] cometvisu-knx-fcgi 2.0.0 starting
[INFO] Configuration:
[INFO]   KNX_TRANSPORT        knxd
[INFO]   BUS                  transport=knxd socket=/run/knx
[INFO]   FCGI_SOCKET           (not set, spawn-fcgi mode)
[INFO]   FCGI_WORKERS          10
[INFO]   LONGPOLL_TIMEOUT_SEC  300
[INFO]   ADDRESS_PREFIX        (not set)
[INFO]   BASE_URL              /proxy/visu
[INFO]   DEBUG_BACKEND         1
```

### FastCGI environment variables

These are standard
[CGI variables](https://fastcgi-archives.github.io/FastCGI_Specification.html)
provided by the web server. The application reads them automatically — you do
not need to set them yourself.

| Variable          | Description                                        |
|-------------------|----------------------------------------------------|
| `REQUEST_METHOD`  | HTTP method: `GET`, `POST`, or `PUT`               |
| `REQUEST_URI`     | Full request URI, e.g. `/cgi-bin/l?u=USER&p=PASS`  |
| `QUERY_STRING`    | Query string portion, e.g. `u=USER&p=PASS&d=DEVICE`|
| `SCRIPT_NAME`     | Virtual path to the script, e.g. `/cgi-bin/l`     |
| `PATH_INFO`       | Extra path after the script name, e.g. `/l`       |
| `CONTENT_TYPE`    | MIME type of the request body (POST/PUT)           |
| `CONTENT_LENGTH`  | Size of the request body in bytes (POST/PUT)       |
| `SERVER_PROTOCOL` | Protocol string, e.g. `HTTP/1.1`                   |

> **Routing note**: The application determines which handler (`/l`, `/r`,
> `/w`) to invoke by inspecting `PATH_INFO` first. If `PATH_INFO` is empty,
> it strips `SCRIPT_NAME` from `REQUEST_URI` to derive the path. When
> `SCRIPT_NAME` itself ends with the endpoint (e.g. `/cgi-bin/l`), the last
> path component is used as the handler key. This means all common web-server
> configurations — `ScriptAlias`, `fastcgi_pass` with `SCRIPT_NAME` pointing
> to a directory or directly to the binary — work out of the box.

### Debug mode

Set `DEBUG_BACKEND=1` (or `true`/`yes`/`on`) to enable detailed debug output to
stderr. This prints every HTTP request/response and all knxd communication with
millisecond timestamps, making it easy to trace the full communication flow:

```
[2026-07-04 12:34:56.789] → HTTP REQUEST: GET /r?a=KNX:1/2/3&t=5
[2026-07-04 12:34:56.790]   → KNXD SEND: cache_read addr=1/2/3 nowait=true
[2026-07-04 12:34:56.791]   ← KNXD RECV: cache_read addr=1/2/3 data=42
[2026-07-04 12:34:56.792] ← HTTP RESPONSE: 200 body={"d":{"KNX:1/2/3":"42"},"i":"1"}
```

KNXD operations are indented under their parent HTTP request. Long URIs (>500
chars) and large response bodies (>1000 chars) are automatically truncated with
a `... (truncated, NNNN chars total)` marker.

## Architecture

See [AGENTS.md](AGENTS.md) for the architecture, the design decisions (worker
pool, shared cache, transport seam, TX queue) and the coding rules.  Deferred
work is listed in [ROADMAP.md](ROADMAP.md).

## License

See [LICENSE](LICENSE).
