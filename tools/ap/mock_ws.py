#!/usr/bin/env python3
"""Throwaway mock Archipelago WebSocket server for validating the PD transport
bridge (src/game/luaai_ap.c) in isolation -- no real AP server or apworld needed.

Speaks just enough AP:
  - completes the WebSocket handshake,
  - sends RoomInfo on connect,
  - on receiving Connect -> replies Connected + a scripted ReceivedItems that
    grants item 1003 (maps to stage 3 / Villa in scripts/ap/client.lua),
  - logs any LocationChecks the client sends,
  - answers ping with pong and handles close.

Pass criteria when driven from the game:
  - pd.ap_status() reaches "connected",
  - Villa appears in the solo mission list (the granted item unlocked it),
  - completing the Defection/Agent objective shows up here as a LocationChecks.

Usage:
  ws://   python3 tools/ap/mock_ws.py [--host 127.0.0.1] [--port 38281]
  wss://  python3 tools/ap/mock_ws.py --tls --cert cert.pem --key key.pem
          (generate a throwaway self-signed pair with:
             openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem \\
               -out cert.pem -days 1 -subj /CN=localhost)
Stdlib only.
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import ssl
import struct
import sys

WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# DataPackage loaded from datapackage.json (run gen_datapackage.py first). When
# present the mock serves it on GetDataPackage and grants items by name; when
# absent it still works but grants nothing.
DP = None
# Scripted starting grant, by item NAME (resolved to ids via the DataPackage).
GRANT_NAMES = [
    "Stage: Defection", "Stage: Villa",
    "Difficulty: Special Agent", "Device: Night Vision",
]


def log(*a):
    print("[mock]", *a, flush=True)


def grant_item_ids():
    if not DP:
        return []
    name_to_id = DP["games"]["Perfect Dark"]["item_name_to_id"]
    return [name_to_id[n] for n in GRANT_NAMES if n in name_to_id]


def http_handshake(conn):
    req = b""
    while b"\r\n\r\n" not in req:
        chunk = conn.recv(4096)
        if not chunk:
            return False
        req += chunk
    key = None
    for line in req.split(b"\r\n"):
        if line.lower().startswith(b"sec-websocket-key:"):
            key = line.split(b":", 1)[1].strip()
    if key is None:
        log("no Sec-WebSocket-Key in request")
        return False
    accept = base64.b64encode(
        hashlib.sha1(key + WS_MAGIC.encode()).digest()
    ).decode()
    resp = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n"
    )
    conn.sendall(resp.encode())
    log("handshake complete")
    return True


def send_frame(conn, payload, opcode=0x1):
    if isinstance(payload, str):
        payload = payload.encode()
    n = len(payload)
    hdr = bytearray([0x80 | opcode])
    if n < 126:
        hdr.append(n)
    elif n <= 0xFFFF:
        hdr.append(126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(127)
        hdr += struct.pack(">Q", n)
    conn.sendall(bytes(hdr) + payload)


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def recv_frame(conn):
    """Return (opcode, payload_bytes) or None on close/EOF."""
    h = recv_exact(conn, 2)
    if not h:
        return None
    op = h[0] & 0x0F
    masked = h[1] & 0x80
    ln = h[1] & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", recv_exact(conn, 2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", recv_exact(conn, 8))[0]
    mask = recv_exact(conn, 4) if masked else b"\0\0\0\0"
    data = recv_exact(conn, ln) if ln else b""
    if data is None:
        return None
    if masked:
        data = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
    return op, data


def send_cmds(conn, cmds):
    send_frame(conn, json.dumps(cmds))


def handle(conn):
    if not http_handshake(conn):
        return

    send_cmds(conn, [{
        "cmd": "RoomInfo",
        "version": {"major": 0, "minor": 5, "build": 0, "class": "Version"},
        "generator_version": {"major": 0, "minor": 5, "build": 0, "class": "Version"},
        "tags": [], "password": False, "permissions": {},
        "hint_cost": 0, "location_check_points": 0,
        "games": ["Perfect Dark"],
        "datapackage_checksums": {"Perfect Dark": "mock"} if DP else {},
        "seed_name": "mock", "time": 0.0,
    }])
    log("sent RoomInfo")

    while True:
        fr = recv_frame(conn)
        if fr is None:
            log("client closed")
            return
        op, data = fr
        if op == 0x8:
            log("recv close")
            return
        if op == 0x9:  # ping -> pong
            send_frame(conn, data, opcode=0xA)
            continue
        if op not in (0x1, 0x2):
            continue
        try:
            msgs = json.loads(data.decode("utf-8"))
        except Exception as e:
            log("bad json from client:", e)
            continue
        for m in msgs:
            cmd = m.get("cmd")
            log("recv", cmd, m)
            if cmd == "GetDataPackage":
                games = m.get("games") or (list(DP["games"]) if DP else [])
                pkg = {g: DP["games"][g] for g in games
                       if DP and g in DP["games"]}
                send_cmds(conn, [{"cmd": "DataPackage", "data": {"games": pkg}}])
                log("sent DataPackage for", list(pkg))
            elif cmd == "Connect":
                send_cmds(conn, [{
                    "cmd": "Connected", "team": 0, "slot": 1,
                    "players": [{"team": 0, "slot": 1,
                                 "alias": m.get("name", "Player1"),
                                 "name": m.get("name", "Player1")}],
                    "missing_locations": [], "checked_locations": [],
                    "slot_data": {}, "slot_info": {}, "hint_points": 0,
                }])
                log("sent Connected")
                grant = grant_item_ids()
                send_cmds(conn, [{
                    "cmd": "ReceivedItems", "index": 0,
                    "items": [{"item": i, "location": 0, "player": 0, "flags": 0}
                              for i in grant],
                }])
                log("sent ReceivedItems " + str(grant) + " " + str(GRANT_NAMES))
            elif cmd == "LocationChecks":
                log("CHECK reported:", m.get("locations"))
                # Echo a PrintJSON so the client logs something visible.
                send_cmds(conn, [{
                    "cmd": "PrintJSON",
                    "data": [{"text": "mock received check(s): " +
                              ",".join(str(x) for x in m.get("locations", []))}],
                }])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=38281)
    ap.add_argument("--tls", action="store_true", help="serve wss:// (needs --cert/--key)")
    ap.add_argument("--cert", default="cert.pem")
    ap.add_argument("--key", default="key.pem")
    args = ap.parse_args()

    global DP
    dp_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "datapackage.json")
    if os.path.exists(dp_path):
        with open(dp_path) as f:
            DP = json.load(f)
        g = DP["games"]["Perfect Dark"]
        log(f"loaded DataPackage: {len(g['item_name_to_id'])} items, "
            f"{len(g['location_name_to_id'])} locations")
    else:
        log("no datapackage.json (run gen_datapackage.py) -> no items granted")

    tlsctx = None
    if args.tls:
        tlsctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tlsctx.load_cert_chain(certfile=args.cert, keyfile=args.key)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    log(f"listening on {'wss' if tlsctx else 'ws'}://{args.host}:{args.port}")
    try:
        while True:
            raw, addr = srv.accept()
            log("connection from", addr)
            conn = raw
            try:
                if tlsctx:
                    conn = tlsctx.wrap_socket(raw, server_side=True)
                handle(conn)
            except (ConnectionResetError, BrokenPipeError, ssl.SSLError) as e:
                log("connection error:", e)
            finally:
                conn.close()
    except KeyboardInterrupt:
        log("bye")
        return 0


if __name__ == "__main__":
    sys.exit(main())
