# Roadmap

Deferred and out-of-scope work.  Everything else that used to be described in
`PLAN.md` and `PLAN-KNXNETIP.md` is implemented in 2.0.0 — the architecture and
design decisions live in [AGENTS.md](AGENTS.md), the client contract and
configuration in [README.md](README.md), and the release history in
[CHANGELOG.md](CHANGELOG.md).

## Deferred

- **`/f` endpoint** — filter management (`POST`/`PUT`/`DELETE`) for batch
  address operations.  Not implemented since 1.0; CometVisu works without it.
  Would need a definition of the request/response shape first.
- **KNX IP Secure** — secure wrapper (Curve25519, AES-128-CCM) in front of the
  KNXnet/IP frame codec.  Explicitly out of scope for 2.0.0: deployments that
  need it stay on the `knxd` transport.  The frame codec in `src/knxnetip/` is
  transport-neutral, so a wrapper can be added without touching any `BusLink`.
- **Interop runs against real hardware** — the unit tests use loopback fakes for
  `ip-tunnel` and `ip-routing`.  A manual checklist with a real interface
  (MDT, Gira, Siemens …) and with `knxd` in `ip:` / `iptn:` mode is still
  missing; `tools/fake-knxnetip-router.py` covers the protocol side only.

## Not planned

- IPv6 / KNXnet/IP v2 — the HPAI codec keeps the protocol byte so it could be
  added later without breaking the frame layout.
- Device management (0x0310) and bus monitoring (0x80 layer).
