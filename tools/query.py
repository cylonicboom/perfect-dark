#!/usr/bin/env python3

# simple client for server status queries
#
# usage: query [--details] <address>[:<port>]
#
# Sends a connectionless PDQM query and prints the server's status. With
# --details it requests the live scoreboard (query type 1) as well. The wire
# format matches netmsgQuerySummaryWrite / netmsgQueryDetailsWrite in
# port/src/net/netmsg.c (see docs/PORT_MASTER_SERVER.md).

import sys, os, socket, struct, selectors

DEFAULT_PORT = 27100
QUERY_MAGIC = b"PDQM\x01"
MAX_WAIT = 5.0

def checksum(data):
  crc = 0xFFFF
  for b in data[:-2]:
    x = crc >> 8 ^ b
    x ^= x >> 4
    crc += (crc << 8) ^ (x << 12) ^ (x << 5) ^ x
    crc &= 0xFFFF
  return crc

class Reader:
  def __init__(self, data):
    self.d = data
    self.o = 0
  def u8(self):
    v = self.d[self.o]; self.o += 1; return v
  def u16(self):
    v = struct.unpack_from("<H", self.d, self.o)[0]; self.o += 2; return v
  def s16(self):
    v = struct.unpack_from("<h", self.d, self.o)[0]; self.o += 2; return v
  def u32(self):
    v = struct.unpack_from("<L", self.d, self.o)[0]; self.o += 4; return v
  def string(self):
    n = struct.unpack_from("<H", self.d, self.o)[0]
    s = self.d[self.o + 2:self.o + 2 + n]
    self.o += 2 + n
    return s[:-1].decode(encoding='utf-8', errors='replace')
  def left(self):
    return len(self.d) - self.o

argv = [a for a in sys.argv[1:]]
details = "--details" in argv
if details:
  argv.remove("--details")

if len(argv) < 1:
  print("usage: query [--details] <address>[:<port>]")
  sys.exit(1)

addrstr = argv[0].strip()
host = addrstr
port = None

if addrstr.startswith('[') and ("]:" in addrstr):
  # [ipv6]:port
  host, sep, port = addrstr.rpartition(':')
elif addrstr.count(':') == 1:
  # possibly ipv4:port or hostname:port
  host, sep, port = addrstr.rpartition(':')

host = host.strip('[]')

if port != None:
  port = int(port)
else:
  port = DEFAULT_PORT

sockfam = socket.AF_INET
if ':' in host:
  sockfam = socket.AF_INET6

sel = selectors.DefaultSelector()
sock = socket.socket(sockfam, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, True)
sock.settimeout(MAX_WAIT)
sel.register(sock, selectors.EVENT_READ, None)

# send query magic to server(s); append a query-type byte (1 = details)
payload = QUERY_MAGIC + (b"\x01" if details else b"")
sock.sendto(payload, (host, port))

while True:
  # wait for response
  events = sel.select(1.0)  # timeout in seconds
  if not events:
    break

  for (key, mask) in events:
    data, from_addr = sock.recvfrom(2048)

    # check magic
    if data[:5] != QUERY_MAGIC:
      print("invalid magic: expected", QUERY_MAGIC, "got", data[:5])
      sys.exit(1)

    # check size
    datalen = struct.unpack("<H", data[5:7])[0]
    if datalen > len(data):
      print("invalid size: expected", len(data), "got", datalen)
      sys.exit(1)

    # check checksum
    chkremote = struct.unpack("<H", data[datalen - 2:datalen])[0]
    chklocal = checksum(data[:datalen])
    if chkremote != chklocal:
      print("invalid checksum: expected", chklocal, "got", chkremote)
      sys.exit(1)

    # payload is everything between the size field and the checksum
    r = Reader(data[7:datalen - 2])

    proto = r.u32()
    flags = r.u8()
    num_clients = r.u8()
    max_clients = r.u8()
    num_sims = r.u8()
    stagenum = r.u8()
    scenario = r.u8()
    servername = r.string()
    romname = r.string()
    moddir = r.string()

    print("address:", from_addr)
    print("protocol ver:", proto)
    print("flags: 0x{:02x} (in_progress={} passworded={} dedicated={} challenge={})".format(
        flags, bool(flags & 1), bool(flags & 2), bool(flags & 4), bool(flags & 8)))
    print("clients: {0}/{1}".format(num_clients, max_clients))
    print("sims:", num_sims)
    print("stage num:", hex(stagenum))
    print("scenario:", scenario)
    print("server name:", servername)
    print("rom name:", romname)
    print("mod dir:", moddir)

    if details and r.left() > 0:
      scorelimit = r.u8()
      timelimit = r.u8()
      teamscorelimit = r.u16()
      print("limits: score={} time={} teamscore={}".format(scorelimit, timelimit, teamscorelimit))
      np = r.u8()
      print("players:", np)
      for i in range(np):
        name = r.string(); ping = r.u16(); team = r.u8(); score = r.s16(); deaths = r.s16()
        print("  {:<16} ping={:<5} team={} score={} deaths={}".format(name, ping, team, score, deaths))
      nb = r.u8()
      print("bots:", nb)
      for i in range(nb):
        name = r.string(); team = r.u8(); diff = r.u8(); score = r.s16()
        print("  {:<16} team={} diff={} score={}".format(name, team, diff, score))

    print("-" * 40)
