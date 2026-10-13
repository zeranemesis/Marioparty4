# Degraded-network UDP relay for local netplay tests (SCRIPTED level only).
# The client sends to LISTEN; the relay forwards to the host at TARGET and
# relays replies back, each direction with its own delay, jitter, loss and
# duplication. Seeded, so a profile is reproducible.
#   python -I netem_relay.py LISTEN TARGET DELAY_MS JITTER_MS LOSS_PCT DUP_PCT SEED STATS_FILE
import heapq, json, random, select, socket, sys, time

listen, target = int(sys.argv[1]), int(sys.argv[2])
delay, jitter = float(sys.argv[3]) / 1000.0, float(sys.argv[4]) / 1000.0
loss, dup = float(sys.argv[5]) / 100.0, float(sys.argv[6]) / 100.0
rng = random.Random(int(sys.argv[7]))
stats_path = sys.argv[8]

front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
front.bind(("127.0.0.1", listen))
back = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
back.bind(("127.0.0.1", 0))
client = None
queue = []  # (due, seq, socket, data, address)
seq = 0
stats = {"c2h": {"in": 0, "lost": 0, "dup": 0, "out": 0}, "h2c": {"in": 0, "lost": 0, "dup": 0, "out": 0}}
last_dump = time.monotonic()


def schedule(sock, data, address, direction):
    global seq
    s = stats[direction]
    s["in"] += 1
    if rng.random() < loss:
        s["lost"] += 1
        return
    copies = 2 if rng.random() < dup else 1
    s["dup"] += copies - 1
    for _ in range(copies):
        due = time.monotonic() + max(0.0, delay + rng.uniform(-jitter, jitter))
        seq += 1
        heapq.heappush(queue, (due, seq, sock, data, address))


while True:
    timeout = 0.5
    if queue:
        timeout = max(0.0, min(timeout, queue[0][0] - time.monotonic()))
    readable, _, _ = select.select([front, back], [], [], timeout)
    for sock in readable:
        try:
            data, address = sock.recvfrom(4096)
        except ConnectionResetError:
            continue
        if sock is front:
            client = address
            schedule(back, data, ("127.0.0.1", target), "c2h")
        elif client is not None:
            schedule(front, data, client, "h2c")
    now = time.monotonic()
    while queue and queue[0][0] <= now:
        _, _, sock, data, address = heapq.heappop(queue)
        try:
            sock.sendto(data, address)
            stats["c2h" if sock is back else "h2c"]["out"] += 1
        except OSError:
            pass
    if now - last_dump > 2:
        with open(stats_path, "w") as handle:
            json.dump(stats, handle)
        last_dump = now
