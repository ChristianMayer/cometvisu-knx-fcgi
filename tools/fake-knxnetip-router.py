#!/usr/bin/env python3
# Copyright (C) 2026 Christian Mayer and the CometVisu contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Minimal KNXnet/IP tunnelling "router" for manual testing without hardware.

It answers CONNECT_REQUEST, ACKs every TUNNELLING_REQUEST (echoing the
telegrams it receives) and answers CONNECTIONSTATE_REQUEST.  Point the backend
at it:

    python3 tools/fake-knxnetip-router.py --port 3671 &
    KNX_TRANSPORT=ip-tunnel KNX_IP_HOST=127.0.0.1 FCGI_SOCKET=:9000 \
        ./build/src/cometvisu-knx-fcgi

Not a KNX stack: it never talks to a bus.  Unit/integration tests use their own
in-process fakes (tests/unit/test_knxnetip_tunnel.cpp).
"""

import argparse
import socket
import struct

SEARCH_REQUEST = 0x0201
SEARCH_RESPONSE = 0x0202
CONNECT_REQUEST = 0x0205
CONNECT_RESPONSE = 0x0206
CONNECTIONSTATE_REQUEST = 0x0207
CONNECTIONSTATE_RESPONSE = 0x0208
DISCONNECT_REQUEST = 0x0209
DISCONNECT_RESPONSE = 0x020A
TUNNELLING_REQUEST = 0x0420
TUNNELLING_ACK = 0x0421
ROUTING_INDICATION = 0x0530

CHANNEL = 0x21


def frame(service, body):
    return struct.pack(">BBHH", 0x06, 0x10, service, 6 + len(body)) + body


def hpai(address, port):
    return struct.pack(">BB4sH", 0x08, 0x01, socket.inet_aton(address), port)


def handle(data, peer):
    if len(data) < 6:
        return []
    _, _, service, _ = struct.unpack(">BBHH", data[:6])
    body = data[6:]

    if service == CONNECT_REQUEST:
        print(f"CONNECT_REQUEST from {peer[0]}:{peer[1]}")
        # channel, status, HPAI data endpoint, CRD (tunnel, link layer, IA)
        response = bytes([CHANNEL, 0x00]) + hpai(peer[0], 3671) + bytes([0x04, 0x04, 0x11, 0x01])
        return [frame(CONNECT_RESPONSE, response)]

    if service == CONNECTIONSTATE_REQUEST:
        return [frame(CONNECTIONSTATE_RESPONSE, bytes([CHANNEL, 0x00]))]

    if service == DISCONNECT_REQUEST:
        print("DISCONNECT_REQUEST")
        return [frame(DISCONNECT_RESPONSE, bytes([CHANNEL, 0x00]))]

    if service == TUNNELLING_REQUEST:
        # 0x04 | channel | sequence | 0x00 | cEMI
        sequence = body[2]
        cemi = body[4:]
        print(f"TUNNELLING_REQUEST seq={sequence} cemi={cemi.hex()}")
        ack = bytes([0x04, CHANNEL, sequence, 0x00])
        return [frame(TUNNELLING_ACK, ack)]

    if service == SEARCH_REQUEST:
        print(f"SEARCH_REQUEST from {peer[0]}:{peer[1]}")
        return []

    print(f"ignoring service 0x{service:04x}")
    return []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3671)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.address, args.port))
    print(f"fake KNXnet/IP router listening on {args.address}:{args.port}")

    while True:
        data, peer = sock.recvfrom(2048)
        for reply in handle(data, peer):
            sock.sendto(reply, peer)


if __name__ == "__main__":
    main()
