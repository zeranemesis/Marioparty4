// PartyBoard relay: a rendezvous for online lobbies whose host cannot open its
// box (CGNAT, no UPnP), and short-lived TURN credentials for their game traffic.
//
// It never sees anything readable. The lobby's TLS, with the certificate pinned
// by the invitation, runs end to end inside the WebSockets this pairs; the room
// is named by a hash of the invitation token, never the token itself; and the
// game datagrams that go through TURN are sealed by the lobby before they leave.
//
//   POST /turn                          -> { urls, username, credential }
//   WS   /room/<id>/host?udp=<ip:port>  the host, waiting for guests
//   WS   /room/<id>/guest?ip=<ipv4>     a guest; gets "hello" then "ready"
//   WS   /room/<id>/accept/<conn>       the host taking one guest connection
//
// Secrets (wrangler secret put): TURN_KEY_ID, TURN_KEY_API_TOKEN.

const ROOM = /^[0-9a-f]{32}$/;
const CONN = /^[0-9a-f]{32}$/;
const MAX_GUESTS = 8;
// Long enough for a twenty-turn party; the lobby asks again for a new salon.
const TURN_TTL_SECONDS = 4 * 3600;

function ip(request) {
  return request.headers.get("CF-Connecting-IP") || "";
}

async function limited(binding, key) {
  if (!binding) return false;
  try {
    const { success } = await binding.limit({ key });
    return !success;
  } catch {
    return false;
  }
}

async function turnCredentials(env) {
  const response = await fetch(
    `https://rtc.live.cloudflare.com/v1/turn/keys/${env.TURN_KEY_ID}/credentials/generate-ice-servers`,
    {
      method: "POST",
      headers: { Authorization: `Bearer ${env.TURN_KEY_API_TOKEN}`, "Content-Type": "application/json" },
      body: JSON.stringify({ ttl: TURN_TTL_SECONDS }),
    },
  );
  if (!response.ok) throw new Error(`turn ${response.status}`);
  const body = await response.json();
  // The lobby wants one set: the entry that carries a username, with all its URLs.
  const servers = Array.isArray(body.iceServers) ? body.iceServers : [body.iceServers];
  const turn = servers.find((s) => s && s.username && s.credential);
  if (!turn) throw new Error("turn: no credentials");
  const urls = Array.isArray(turn.urls) ? turn.urls : [turn.urls];
  return { urls, username: turn.username, credential: turn.credential };
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const parts = url.pathname.split("/").filter(Boolean);

    if (parts.length === 1 && parts[0] === "turn") {
      if (request.method !== "POST") return new Response("POST only", { status: 405 });
      if (await limited(env.TURN_LIMIT, ip(request))) return new Response("slow down", { status: 429 });
      try {
        return Response.json(await turnCredentials(env), { headers: { "Cache-Control": "no-store" } });
      } catch {
        return new Response("turn unavailable", { status: 503 });
      }
    }

    if (parts[0] === "room" && parts.length >= 3 && ROOM.test(parts[1])) {
      if (request.headers.get("Upgrade") !== "websocket") return new Response("websocket only", { status: 426 });
      if (parts[2] === "host" && (await limited(env.ROOM_LIMIT, ip(request)))) {
        return new Response("slow down", { status: 429 });
      }
      const room = env.ROOMS.get(env.ROOMS.idFromName(parts[1]));
      return room.fetch(request);
    }

    return new Response("PartyBoard relay", { status: 404 });
  },
};

// One room per salon. Hibernatable WebSockets: an idle salon costs nothing while
// it waits, and the roles live in the sockets' tags and attachments, not in
// memory that hibernation would drop.
export class RelayRoom {
  constructor(state) {
    this.state = state;
  }

  async fetch(request) {
    const url = new URL(request.url);
    const parts = url.pathname.split("/").filter(Boolean);
    const role = parts[2];
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);

    if (role === "host") {
      // A new host for the same room replaces the old one (a lobby that
      // reconnected); the guests already paired keep their own sockets.
      for (const old of this.state.getWebSockets("host")) old.close(4000, "replaced");
      const udp = url.searchParams.get("udp") || "";
      this.state.acceptWebSocket(server, ["host"]);
      server.serializeAttachment({ role: "host", udp });
      return new Response(null, { status: 101, webSocket: client });
    }

    if (role === "guest") {
      const host = this.state.getWebSockets("host")[0];
      const full = this.state.getWebSockets("guest").length >= MAX_GUESTS;
      const conn = crypto.randomUUID().replaceAll("-", "");
      // Accepted once, already tagged, so the accept below can find it.
      this.state.acceptWebSocket(server, ["guest", `g:${conn}`]);
      if (!host || full) {
        server.close(host ? 4429 : 4404, host ? "room full" : "no host");
        return new Response(null, { status: 101, webSocket: client });
      }
      // The guest's own IPv4 as STUN saw it, else the address this request
      // came from: the host must permit the address TURN will see.
      const said = url.searchParams.get("ip") || "";
      const address = /^\d{1,3}(\.\d{1,3}){3}$/.test(said) ? said : ip(request);
      server.serializeAttachment({ role: "guest", conn });
      host.send(JSON.stringify({ type: "incoming", conn, ip: address }));
      server.send(JSON.stringify({ type: "hello", udp: host.deserializeAttachment().udp }));
      return new Response(null, { status: 101, webSocket: client });
    }

    if (role === "accept" && CONN.test(parts[3] || "")) {
      const conn = parts[3];
      const guest = this.state.getWebSockets(`g:${conn}`)[0];
      this.state.acceptWebSocket(server, ["accept", `a:${conn}`]);
      if (!guest) {
        server.close(4404, "no guest");
        return new Response(null, { status: 101, webSocket: client });
      }
      server.serializeAttachment({ role: "accept", conn });
      // Nothing is buffered here: the guest starts its TLS only on "ready".
      guest.send(JSON.stringify({ type: "ready" }));
      return new Response(null, { status: 101, webSocket: client });
    }

    return new Response("unknown role", { status: 404 });
  }

  peer(ws) {
    const a = ws.deserializeAttachment();
    if (!a) return null;
    if (a.role === "guest") return this.state.getWebSockets(`a:${a.conn}`)[0] || null;
    if (a.role === "accept") return this.state.getWebSockets(`g:${a.conn}`)[0] || null;
    return null;
  }

  webSocketMessage(ws, message) {
    // Only ciphertext is piped, as binary; text from a client is not ours to relay.
    if (typeof message === "string") return;
    const peer = this.peer(ws);
    if (peer) peer.send(message);
  }

  webSocketClose(ws, code) {
    const peer = this.peer(ws);
    if (peer) peer.close(1000, "peer left");
    try {
      ws.close(code === 1005 ? 1000 : code, "closed");
    } catch {}
  }

  webSocketError(ws) {
    const peer = this.peer(ws);
    if (peer) peer.close(1011, "peer error");
  }
}
