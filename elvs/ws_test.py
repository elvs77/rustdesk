"""elvs: end-to-end test of device registration over WebSocket.

Starts the freshly built hbbs in a temp dir and plays both sides of a session
the way the RustDesk client does when it may only reach port 443 (WebSocket):

  B  = customer PC, registers over WebSocket (RegisterPk), stays connected
  B2 = second PC behind the same public IP (same X-Real-IP)
  A  = technician, asks for B (PunchHoleRequest) over WebSocket
then checks that B gets the PunchHole through its WebSocket, answers with a
RelayResponse on a new connection, and A receives it. Also checks heartbeats,
online state, RequestRelay routing, offline after disconnect and that plain UDP
registration still works as before.
"""
import os, socket, subprocess, sys, tempfile, time, shutil
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "gen"))
import rendezvous_pb2 as pb
from websockets.sync.client import connect

HBBS = os.path.abspath(sys.argv[1])
PORT = 21216  # not the default, so nothing else on the runner gets in the way
WS = f"ws://127.0.0.1:{PORT + 2}"
FAIL = []


def check(cond, what):
    print(("OK   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAIL.append(what)


def msg(**kw):
    m = pb.RendezvousMessage()
    for k, v in kw.items():
        getattr(m, k).CopyFrom(v)
    return m.SerializeToString()


def parse(b):
    m = pb.RendezvousMessage()
    m.ParseFromString(b)
    return m


def ws(real_ip=None):
    h = {"X-Real-IP": real_ip} if real_ip else None
    return connect(WS, additional_headers=h, open_timeout=5, max_size=None)


def recv_msg(c, timeout=5, skip_empty=True):
    end = time.time() + timeout
    while time.time() < end:
        try:
            b = c.recv(timeout=max(0.1, end - time.time()))
        except TimeoutError:
            break
        if isinstance(b, str):
            continue
        if not b and skip_empty:
            continue
        return parse(b) if b else None
    return "timeout"


def local_ip():
    # hbbs treats loopback on the NAT-test port as console, so ask from a real address
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.connect(("192.0.2.1", 9))  # nothing is sent
    ip = u.getsockname()[0]
    u.close()
    return ip


def online(ids):
    s = socket.create_connection((local_ip(), PORT - 1), timeout=5)
    body = msg(online_request=pb.OnlineRequest(id="x", peers=ids))
    s.sendall(frame(body))
    data = read_frame(s)
    s.close()
    st = parse(data).online_response.states
    return [bool(st[i // 8] & (0x80 >> (i % 8))) for i in range(len(ids))]


def frame(b):  # hbb_common BytesCodec
    n = len(b)
    if n <= 0x3F:
        h = (n << 2).to_bytes(1, "little")
    elif n <= 0x3FFF:
        h = ((n << 2) | 1).to_bytes(2, "little")
    elif n <= 0x3FFFFF:
        h = ((n << 2) | 2).to_bytes(3, "little")
    else:
        h = ((n << 2) | 3).to_bytes(4, "little")
    return h + b


def read_frame(s):
    first = s.recv(1)
    hl = (first[0] & 3) + 1
    rest = b""
    while len(rest) < hl - 1:
        rest += s.recv(hl - 1 - len(rest))
    n = int.from_bytes(first + rest, "little") >> 2
    data = b""
    while len(data) < n:
        data += s.recv(n - len(data))
    return data


def register(c, pid, uuid):
    c.send(msg(register_pk=pb.RegisterPk(id=pid, uuid=uuid, pk=os.urandom(32))))
    return recv_msg(c)


work = tempfile.mkdtemp()
log = open(os.path.join(work, "hbbs.log"), "w")
env = dict(os.environ, RUST_LOG="info")
proc = subprocess.Popen([HBBS, "-p", str(PORT)], cwd=work, stdout=log, stderr=subprocess.STDOUT, env=env)
try:
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", PORT + 2), timeout=1).close()
            break
        except OSError:
            time.sleep(0.2)
    key = open(os.path.join(work, "id_ed25519.pub")).read().strip()

    # B and B2: two PCs behind one public IP, registered over WebSocket
    b = ws("203.0.113.5")
    r = register(b, "elvs100001", b"uuid-b")
    check(r != "timeout" and r.HasField("register_pk_response") and r.register_pk_response.result == 0,
          "B registers over WebSocket (RegisterPkResponse OK)")
    b2 = ws("203.0.113.5")
    r = register(b2, "elvs100002", b"uuid-b2")
    check(r != "timeout" and r.register_pk_response.result == 0, "B2 (same public IP) registers too")
    check(online(["elvs100001", "elvs100002", "nobody9"]) == [True, True, False], "both online, unknown id offline")

    # A asks for B -> B must get the PunchHole over its WebSocket
    a = ws("198.51.100.7")
    a.send(msg(punch_hole_request=pb.PunchHoleRequest(id="elvs100001", licence_key=key, version="1.4.0")))
    r = recv_msg(b)
    check(r != "timeout" and r.HasField("punch_hole"), "B receives PunchHole through WebSocket")
    r2 = recv_msg(b2, timeout=1)
    check(r2 == "timeout", "B2 does not get B's PunchHole")
    if r != "timeout" and r.HasField("punch_hole"):
        # B answers like the client in relay mode: new connection, RelayResponse
        b_rel = ws("203.0.113.5")
        b_rel.send(msg(relay_response=pb.RelayResponse(socket_addr=r.punch_hole.socket_addr,
                                                        uuid="sess-1", relay_server="relay.example",
                                                        id="elvs100001", version="1.4.0")))
        r = recv_msg(a)
        check(r != "timeout" and r.HasField("relay_response") and r.relay_response.uuid == "sess-1",
              "A receives B's RelayResponse")
        b_rel.close()
    a.close()

    # RequestRelay from A goes straight to B's WebSocket
    a = ws("198.51.100.8")
    a.send(msg(request_relay=pb.RequestRelay(id="elvs100002", uuid="sess-2", relay_server="relay.example",
                                             licence_key=key)))
    r = recv_msg(b2)
    check(r != "timeout" and r.HasField("request_relay") and r.request_relay.uuid == "sess-2",
          "B2 receives RequestRelay through WebSocket")
    a.close()

    # heartbeats keep B online well past REG_TIMEOUT (30 s) and the 30 s read timeout
    beats = 0
    end = time.time() + 40
    while time.time() < end:
        try:
            x = b.recv(timeout=1)
        except TimeoutError:
            continue
        if x == b"":
            beats += 1
            b.send(b"")  # the client answers every empty frame with one
    check(beats >= 2, f"B gets heartbeats ({beats} in 40 s)")
    # B2 sends nothing back -> must still be online after 40 s? No: B2 did not answer,
    # so it has to drop out of the online list (REG_TIMEOUT 30 s)
    check(online(["elvs100001", "elvs100002"]) == [True, False],
          "after 40 s: B (answers heartbeats) online, B2 (silent) offline")

    a = ws("198.51.100.9")
    a.send(msg(punch_hole_request=pb.PunchHoleRequest(id="elvs100001", licence_key=key, version="1.4.0")))
    r = recv_msg(b)
    check(r != "timeout" and r.HasField("punch_hole"), "B still reachable after 40 s")
    a.close()

    # B goes away -> offline after REG_TIMEOUT
    b.close()
    time.sleep(32)
    check(online(["elvs100001"]) == [False], "B offline 32 s after closing its connection")

    # plain UDP registration unchanged
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(3)
    u.sendto(msg(register_peer=pb.RegisterPeer(id="elvs100003", serial=0)), ("127.0.0.1", PORT))
    data, _ = u.recvfrom(4096)
    r = parse(data)
    check(r.HasField("register_peer_response") and r.register_peer_response.request_pk, "UDP RegisterPeer as before")
    u.sendto(msg(register_pk=pb.RegisterPk(id="elvs100003", uuid=b"uuid-u", pk=os.urandom(32))), ("127.0.0.1", PORT))
    data, _ = u.recvfrom(4096)
    check(parse(data).register_pk_response.result == 0, "UDP RegisterPk as before")
    check(online(["elvs100003"]) == [True], "UDP device online")
    a = ws("198.51.100.10")
    a.send(msg(punch_hole_request=pb.PunchHoleRequest(id="elvs100003", licence_key=key, version="1.4.0")))
    data, _ = u.recvfrom(4096)
    check(parse(data).HasField("punch_hole"), "UDP device still gets PunchHole over UDP")
    a.close()
except Exception:
    import traceback
    traceback.print_exc()
    FAIL.append("exception")
finally:
    proc.kill()
    proc.wait(5)
    log.close()
    print("---- hbbs log (tail) ----")
    print("".join(open(os.path.join(work, "hbbs.log")).readlines()[-40:]))
    shutil.rmtree(work, ignore_errors=True)

print(f"\n{len(FAIL)} failures")
sys.stdout.flush()
os._exit(1 if FAIL else 0)  # websockets threads must not keep the process alive
