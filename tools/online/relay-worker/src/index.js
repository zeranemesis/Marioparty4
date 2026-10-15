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
//   GET  /rooms?build=<hex>&disc=<hex>  the public rooms this player can join
//   GET  /rooms/<id>                    one public room's invitation
//
// A host makes its room public by sending {"type":"publish",...} on its host
// socket, and private again with {"type":"unpublish"}; closing that socket
// takes it off the list too. A public room's invitation must carry no address
// (the Worker checks), so its guests come through the relay and nobody who
// reads the list learns where the host lives.
//
// Secrets (wrangler secret put): TURN_KEY_ID, TURN_KEY_API_TOKEN.

const ROOM = /^[0-9a-f]{32}$/;
const CONN = /^[0-9a-f]{32}$/;
const MAX_GUESTS = 8;
const HEX = /^[0-9a-f]{16}$/;
const LANGS = new Set(["en", "de", "fr", "es", "it"]);
// More than the directory ever needs, and a bound on what one script can fill.
const MAX_PUBLIC = 300;
const LIST_SIZE = 60;
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

// The PB4 invitation as the lobby encodes it (Invitation.Encode): 49 bytes,
// address 4, port 2, expiry 4, fingerprint 16, token 16, local address 4,
// local port 2, players 1.
function decodeInvite(text) {
  if (typeof text !== "string" || text.length !== 72 || !text.startsWith("PB4.")) return null;
  let raw;
  try {
    raw = atob(text.slice(4).replaceAll("-", "+").replaceAll("_", "/"));
  } catch {
    return null;
  }
  if (raw.length !== 49) return null;
  const b = Uint8Array.from(raw, (c) => c.charCodeAt(0));
  const v = new DataView(b.buffer);
  return {
    address: b.slice(0, 4),
    port: v.getUint16(4, true),
    expires: v.getUint32(6, true),
    token: b.slice(26, 42),
    local: b.slice(42, 46),
    localPort: v.getUint16(46, true),
    players: b[48],
  };
}

// The room a token names: the first 16 bytes of SHA-256("PBRELAY1" + token).
async function roomOf(token) {
  const data = new Uint8Array(8 + token.length);
  data.set(new TextEncoder().encode("PBRELAY1"));
  data.set(token, 8);
  const hash = new Uint8Array(await crypto.subtle.digest("SHA-256", data));
  return [...hash.slice(0, 16)].map((x) => x.toString(16).padStart(2, "0")).join("");
}

// A pseudo for a list everyone reads: printable, short, never empty.
function cleanName(text) {
  const name = String(text || "")
    .replace(/[\u0000-\u001f\u007f-\u009f\u200b-\u200f\u202a-\u202e\u2066-\u2069]/g, "")
    .trim()
    .slice(0, 24);
  return name || "Joueur";
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

    if (parts[0] === "rooms" && parts.length <= 2) {
      if (request.method !== "GET") return new Response("GET only", { status: 405 });
      if (parts.length === 2 && !ROOM.test(parts[1])) return new Response("no such room", { status: 404 });
      if (await limited(env.LIST_LIMIT, ip(request))) return new Response("slow down", { status: 429 });
      return env.DIRECTORY.get(env.DIRECTORY.idFromName("public")).fetch(request);
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
  constructor(state, env) {
    this.state = state;
    this.env = env;
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
      // A host that gave a key holds the room: only the same key replaces it.
      // Guests learn the room from the invitation, and a public room's
      // invitation is on the list, so without this anyone could take it over.
      const key = url.searchParams.get("key") || "";
      const held = await this.state.storage.get("key");
      if (held && key !== held) {
        this.state.acceptWebSocket(server, ["refused"]);
        server.close(4403, "room taken");
        return new Response(null, { status: 101, webSocket: client });
      }
      if (!held && /^[0-9a-f]{32}$/.test(key)) await this.state.storage.put("key", key);
      for (const old of this.state.getWebSockets("host")) old.close(4000, "replaced");
      const udp = url.searchParams.get("udp") || "";
      this.state.acceptWebSocket(server, ["host"]);
      server.serializeAttachment({ role: "host", udp, room: parts[1] });
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

  directory() {
    return this.env.DIRECTORY.get(this.env.DIRECTORY.idFromName("public"));
  }

  // The host's own requests: publish this room on the public list, or not.
  async hostMessage(ws, text) {
    const a = ws.deserializeAttachment();
    let m;
    try {
      m = JSON.parse(text);
    } catch {
      return;
    }
    if (m && m.type === "unpublish") {
      await this.directory().fetch("https://directory/remove", { method: "POST", body: JSON.stringify({ room: a.room }) });
      ws.send(JSON.stringify({ type: "unpublished" }));
      return;
    }
    if (!m || m.type !== "publish") return;
    const refuse = (reason) => ws.send(JSON.stringify({ type: "publish-refused", reason }));
    const invite = decodeInvite(m.invite);
    if (!invite) return refuse("invite");
    // No address at all: the list must never say where a host is.
    if (invite.address.some((x) => x) || invite.port || invite.local.some((x) => x) || invite.localPort) return refuse("address");
    // Two players: the relay carries one guest.
    if (invite.players !== 2) return refuse("players");
    if ((await roomOf(invite.token)) !== a.room) return refuse("room");
    const now = Math.floor(Date.now() / 1000);
    if (invite.expires <= now || invite.expires > now + 31 * 60) return refuse("expired");
    if (!HEX.test(m.build || "") || !HEX.test(m.disc || "")) return refuse("fields");
    const entry = {
      room: a.room,
      name: cleanName(m.name),
      lang: LANGS.has(m.lang) ? m.lang : "en",
      region: m.region === "eu" ? "eu" : "us",
      build: m.build,
      disc: m.disc,
      invite: m.invite,
      expires: invite.expires,
    };
    const r = await this.directory().fetch("https://directory/publish", { method: "POST", body: JSON.stringify(entry) });
    ws.send(JSON.stringify(r.ok ? { type: "published" } : { type: "publish-refused", reason: "full" }));
  }

  async webSocketMessage(ws, message) {
    // Only ciphertext is piped, as binary; text from a guest is not ours to relay.
    if (typeof message === "string") {
      const a = ws.deserializeAttachment();
      if (a && a.role === "host") await this.hostMessage(ws, message);
      return;
    }
    const peer = this.peer(ws);
    if (peer) peer.send(message);
  }

  async webSocketClose(ws, code) {
    const a = ws.deserializeAttachment();
    // The last host socket gone: the room leaves the public list.
    if (a && a.role === "host" && a.room && !this.state.getWebSockets("host").some((h) => h !== ws)) {
      try {
        await this.directory().fetch("https://directory/remove", { method: "POST", body: JSON.stringify({ room: a.room }) });
      } catch {}
      // Its key goes too: a room is one salon, and nothing should outlive it.
      await this.state.storage.delete("key");
    }
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

// The public list: one instance, in SQLite. An entry lives while its host's
// socket is open and its invitation has not expired, whichever ends first.
export class RoomDirectory {
  constructor(state) {
    this.sql = state.storage.sql;
    this.sql.exec(`CREATE TABLE IF NOT EXISTS rooms (
      room TEXT PRIMARY KEY, name TEXT, lang TEXT, region TEXT, build TEXT, disc TEXT,
      invite TEXT, expires INTEGER, created INTEGER)`);
  }

  async fetch(request) {
    const url = new URL(request.url);
    const parts = url.pathname.split("/").filter(Boolean);
    const now = Math.floor(Date.now() / 1000);
    this.sql.exec("DELETE FROM rooms WHERE expires <= ?", now);

    if (request.method === "POST" && parts[0] === "publish") {
      const e = await request.json();
      const count = this.sql.exec("SELECT COUNT(*) AS n FROM rooms WHERE room != ?", e.room).one().n;
      if (count >= MAX_PUBLIC) return new Response("full", { status: 507 });
      this.sql.exec(
        `INSERT INTO rooms (room, name, lang, region, build, disc, invite, expires, created)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT(room) DO UPDATE SET name = excluded.name, lang = excluded.lang, region = excluded.region,
           build = excluded.build, disc = excluded.disc, invite = excluded.invite, expires = excluded.expires`,
        e.room, e.name, e.lang, e.region, e.build, e.disc, e.invite, e.expires, now,
      );
      return new Response("ok");
    }
    if (request.method === "POST" && parts[0] === "remove") {
      const e = await request.json();
      this.sql.exec("DELETE FROM rooms WHERE room = ?", String(e.room || ""));
      return new Response("ok");
    }

    const headers = { "Cache-Control": "no-store" };
    if (parts[0] === "rooms" && parts.length === 2) {
      const row = this.sql.exec("SELECT invite FROM rooms WHERE room = ?", parts[1]).toArray()[0];
      if (!row) return new Response("no such room", { status: 404, headers });
      return Response.json({ invite: row.invite }, { headers });
    }
    if (parts[0] === "rooms") {
      // Only the rooms this player could start: same build, same disc.
      const build = url.searchParams.get("build") || "";
      const disc = url.searchParams.get("disc") || "";
      if (!HEX.test(build) || !HEX.test(disc)) return Response.json({ rooms: [] }, { headers });
      const rooms = this.sql
        .exec(
          "SELECT room, name, lang, region, created FROM rooms WHERE build = ? AND disc = ? ORDER BY created DESC LIMIT ?",
          build, disc, LIST_SIZE,
        )
        .toArray()
        .map((r) => ({ room: r.room, name: r.name, lang: r.lang, region: r.region, age: Math.max(0, now - r.created) }));
      return Response.json({ rooms }, { headers });
    }
    return new Response("unknown", { status: 404 });
  }
}
