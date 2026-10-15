using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace PartyBoardOnline {

// What Bridge needs from the socket that faces the Internet: a plain UDP socket
// when the box could be opened, or a TURN allocation when it could not. Receive
// never blocks; it throws SocketException(WouldBlock) when nothing is queued,
// which the pump already treats as "nothing to read this turn".
interface IDatagramSocket : IDisposable {
    void Send(byte[] data,int length,IPEndPoint to);
    byte[] Receive(ref IPEndPoint from);
    void NonBlocking();
}

sealed class UdpDatagramSocket : IDatagramSocket {
    readonly UdpClient udp;
    public UdpDatagramSocket(UdpClient client){udp=client;}
    public void Send(byte[] data,int length,IPEndPoint to){udp.Send(data,length,to);}
    public byte[] Receive(ref IPEndPoint from){return udp.Receive(ref from);}
    public void NonBlocking(){udp.Client.Blocking=false;}
    public void Dispose(){udp.Close();}
}

// STUN (RFC 5389) and the TURN additions (RFC 8656) this lobby uses: Allocate,
// Refresh, CreatePermission, ChannelBind, Send and Data, with the long-term
// credential. IPv4 only, like the rest of the lobby.
static class Stun {
    public const uint Cookie=0x2112A442;
    public const ushort Binding=0x001,Allocate=0x003,Refresh=0x004,SendMethod=0x006,DataMethod=0x007,CreatePermission=0x008,ChannelBind=0x009;
    public const ushort Request=0x000,Indication=0x010,Success=0x100,Error=0x110;
    public const ushort MappedAddress=0x0001,Username=0x0006,MessageIntegrity=0x0008,ErrorCode=0x0009,ChannelNumber=0x000C,Lifetime=0x000D,
        XorPeerAddress=0x0012,Data=0x0013,Realm=0x0014,Nonce=0x0015,XorRelayedAddress=0x0016,RequestedTransport=0x0019,XorMappedAddress=0x0020,
        Software=0x8022,Fingerprint=0x8028;

    public sealed class Message {
        public ushort Type;public byte[] Transaction=new byte[12];
        public readonly List<KeyValuePair<ushort,byte[]>> Attributes=new List<KeyValuePair<ushort,byte[]>>();
        public ushort Method {get{return (ushort)(Type & 0x000F | (Type & 0x00E0)>>1 | (Type & 0x3E00)>>2);}}
        public ushort Class {get{return (ushort)(Type & 0x0110);}}
        public byte[] Get(ushort type){foreach(var a in Attributes)if(a.Key==type)return a.Value;return null;}
        public Message Add(ushort type,byte[] value){Attributes.Add(new KeyValuePair<ushort,byte[]>(type,value));return this;}
        // Offset of MESSAGE-INTEGRITY within the encoded bytes, for Verify.
        internal int IntegrityOffset=-1;internal byte[] Raw;
    }

    public static ushort TypeOf(ushort method,ushort cls) {
        return (ushort)((method & 0x000F) | (method & 0x0070)<<1 | (method & 0x0F80)<<2 | cls);
    }
    public static Message New(ushort method,ushort cls) {
        var m=new Message{Type=TypeOf(method,cls)};
        using(var rng=RandomNumberGenerator.Create())rng.GetBytes(m.Transaction);
        return m;
    }

    static void Put16(byte[] b,int p,int v){b[p]=(byte)(v>>8);b[p+1]=(byte)v;}
    static void Put32(byte[] b,int p,uint v){b[p]=(byte)(v>>24);b[p+1]=(byte)(v>>16);b[p+2]=(byte)(v>>8);b[p+3]=(byte)v;}
    static int Get16(byte[] b,int p){return b[p]<<8|b[p+1];}
    static uint Get32(byte[] b,int p){return (uint)b[p]<<24|(uint)b[p+1]<<16|(uint)b[p+2]<<8|b[p+3];}

    // Encodes, and signs with MESSAGE-INTEGRITY when a key is given, then adds a
    // FINGERPRINT when asked. Both are computed the way RFC 5389 15.4 and 15.5
    // say: over the message so far, with the length field already counting the
    // attribute being added.
    public static byte[] Encode(Message m,byte[] key=null,bool fingerprint=false) {
        var body=new MemoryStream();
        foreach(var a in m.Attributes) {
            var head=new byte[4];Put16(head,0,a.Key);Put16(head,2,a.Value.Length);body.Write(head,0,4);body.Write(a.Value,0,a.Value.Length);
            int pad=(4-a.Value.Length%4)%4;body.Write(new byte[pad],0,pad);
        }
        var bytes=new List<byte>(Header(m,(int)body.Length));bytes.AddRange(body.ToArray());
        if(key!=null) {
            var withLength=bytes.ToArray();Put16(withLength,2,(int)body.Length+24);
            byte[] mac;using(var h=new HMACSHA1(key))mac=h.ComputeHash(withLength);
            var head=new byte[4];Put16(head,0,MessageIntegrity);Put16(head,2,20);
            bytes=new List<byte>(withLength);bytes.AddRange(head);bytes.AddRange(mac);
        }
        if(fingerprint) {
            var withLength=bytes.ToArray();Put16(withLength,2,withLength.Length-20+8);
            var crc=new byte[4];Put32(crc,0,Crc32(withLength)^0x5354554E);
            var head=new byte[4];Put16(head,0,Fingerprint);Put16(head,2,4);
            bytes=new List<byte>(withLength);bytes.AddRange(head);bytes.AddRange(crc);
        }
        return bytes.ToArray();
    }
    static byte[] Header(Message m,int length) {
        var h=new byte[20];Put16(h,0,m.Type);Put16(h,2,length);Put32(h,4,Cookie);Buffer.BlockCopy(m.Transaction,0,h,8,12);return h;
    }

    // Null when the bytes are not a STUN message with the magic cookie.
    public static Message Decode(byte[] b,int length) {
        if(length<20 || (b[0]&0xC0)!=0 || Get32(b,4)!=Cookie)return null;
        int size=Get16(b,2);if(size%4!=0 || 20+size>length)return null;
        var m=new Message{Type=(ushort)Get16(b,0)};Buffer.BlockCopy(b,8,m.Transaction,0,12);
        m.Raw=new byte[20+size];Buffer.BlockCopy(b,0,m.Raw,0,20+size);
        int p=20;
        while(p+4<=20+size) {
            int type=Get16(b,p),len=Get16(b,p+2);if(p+4+len>20+size)return null;
            if(type==MessageIntegrity)m.IntegrityOffset=p;
            var value=new byte[len];Buffer.BlockCopy(b,p+4,value,0,len);m.Add((ushort)type,value);
            p+=4+len+(4-len%4)%4;
        }
        return m;
    }

    // MESSAGE-INTEGRITY check: the HMAC over everything before the attribute, with
    // the length field rewritten to end right after it.
    public static bool Verify(Message m,byte[] key) {
        if(m.IntegrityOffset<0 || m.Raw==null)return false;
        var signed=new byte[m.IntegrityOffset];Buffer.BlockCopy(m.Raw,0,signed,0,signed.Length);Put16(signed,2,m.IntegrityOffset-20+24);
        byte[] mac;using(var h=new HMACSHA1(key))mac=h.ComputeHash(signed);
        var stated=new byte[20];Buffer.BlockCopy(m.Raw,m.IntegrityOffset+4,stated,0,20);
        return Wire.Equal(mac,stated);
    }
    public static bool VerifyFingerprint(Message m) {
        var stated=m.Get(Fingerprint);if(stated==null || stated.Length!=4 || m.Raw.Length<8)return false;
        var covered=new byte[m.Raw.Length-8];Buffer.BlockCopy(m.Raw,0,covered,0,covered.Length);
        return (Crc32(covered)^0x5354554E)==Get32(stated,0);
    }

    // The long-term credential key: MD5(username ":" realm ":" password).
    public static byte[] LongTermKey(string username,string realm,string password) {
        using(var md5=MD5.Create())return md5.ComputeHash(Encoding.UTF8.GetBytes(username+":"+realm+":"+password));
    }

    public static byte[] XorAddress(IPEndPoint e) {
        var a=e.Address.GetAddressBytes();if(a.Length!=4)throw new IOException("IPv4 seulement.");
        var v=new byte[8];v[1]=1;Put16(v,2,e.Port^(int)(Cookie>>16));
        var c=new byte[4];Put32(c,0,Cookie);for(int i=0;i<4;i++)v[4+i]=(byte)(a[i]^c[i]);return v;
    }
    public static IPEndPoint ReadXorAddress(byte[] v) {
        if(v==null || v.Length<8 || v[1]!=1)return null;
        var c=new byte[4];Put32(c,0,Cookie);var a=new byte[4];for(int i=0;i<4;i++)a[i]=(byte)(v[4+i]^c[i]);
        return new IPEndPoint(new IPAddress(a),Get16(v,2)^(int)(Cookie>>16));
    }
    public static int ReadErrorCode(byte[] v) {return v==null || v.Length<4?0:(v[2]&7)*100+v[3];}
    public static byte[] U32(uint value){var b=new byte[4];Put32(b,0,value);return b;}
    public static uint ReadU32(byte[] v){return v==null || v.Length<4?0:Get32(v,0);}

    static readonly uint[] crcTable=Enumerable.Range(0,256).Select(n=>{uint c=(uint)n;for(int k=0;k<8;k++)c=(c&1)!=0?0xEDB88320u^(c>>1):c>>1;return c;}).ToArray();
    public static uint Crc32(byte[] data) {uint c=0xFFFFFFFFu;foreach(var x in data)c=crcTable[(c^x)&0xFF]^(c>>8);return c^0xFFFFFFFFu;}
}

// A TURN allocation used as this host's Internet-facing UDP socket. Guests send
// to Relayed exactly as they would to a public address; their datagrams arrive
// here as Data indications or ChannelData, and replies leave the same way. One
// thread reads the server; Bridge's pump only ever touches the inbox.
sealed class TurnClient : IDatagramSocket {
    readonly UdpClient server;readonly string username,password;
    string realm;byte[] nonce,key;
    public IPEndPoint Relayed {get;private set;}
    public IPEndPoint Mapped {get;private set;}
    readonly ConcurrentQueue<Tuple<byte[],IPEndPoint>> inbox=new ConcurrentQueue<Tuple<byte[],IPEndPoint>>();
    readonly ConcurrentDictionary<string,TaskCompletionSource<Stun.Message>> pending=new ConcurrentDictionary<string,TaskCompletionSource<Stun.Message>>();
    readonly ConcurrentDictionary<string,ushort> channelOf=new ConcurrentDictionary<string,ushort>();
    readonly ConcurrentDictionary<ushort,IPEndPoint> peerOf=new ConcurrentDictionary<ushort,IPEndPoint>();
    readonly ConcurrentDictionary<string,byte> binding=new ConcurrentDictionary<string,byte>();
    readonly HashSet<IPAddress> permitted=new HashSet<IPAddress>();
    int nextChannel=0x4000;volatile bool closed;Thread reader;Timer refresher;uint lifetime=600;
    // How the periodic refresh of allocation, permissions and channels went.
    int refreshes,refreshFailures;
    public int Refreshes {get{return Volatile.Read(ref refreshes);}}
    public int RefreshFailures {get{return Volatile.Read(ref refreshFailures);}}

    public TurnClient(IPEndPoint turnServer,string user,string credential) {
        username=user;password=credential;server=new UdpClient(new IPEndPoint(IPAddress.Any,0));server.Connect(turnServer);
        reader=new Thread(Read){IsBackground=true,Name="turn_read"};reader.Start();
    }

    static string Key(byte[] transaction){return Convert.ToBase64String(transaction);}
    static string Key(IPEndPoint e){return e.ToString();}

    // Sends a request until it is answered, as RFC 5389 7.2.1 asks over UDP.
    Stun.Message Transact(Stun.Message request,bool sign) {
        var done=new TaskCompletionSource<Stun.Message>();pending[Key(request.Transaction)]=done;
        try {
            var bytes=Stun.Encode(request,sign?key:null,true);int wait=500;
            for(int attempt=0;attempt<6 && !closed;attempt++,wait=Math.Min(wait*2,4000)) {
                server.Send(bytes,bytes.Length);
                if(done.Task.Wait(wait))return done.Task.Result;
            }
            throw new IOException("Le relais ne répond pas.");
        } finally {TaskCompletionSource<Stun.Message> ignored;pending.TryRemove(Key(request.Transaction),out ignored);}
    }

    // An authenticated request: signed with the long-term credential, retried
    // once with the server's new nonce when it says the old one went stale (438).
    Stun.Message Authenticated(ushort method,Action<Stun.Message> fill) {
        for(int attempt=0;attempt<2;attempt++) {
            var request=Stun.New(method,Stun.Request);fill(request);
            request.Add(Stun.Username,Encoding.UTF8.GetBytes(username)).Add(Stun.Realm,Encoding.UTF8.GetBytes(realm)).Add(Stun.Nonce,nonce);
            var reply=Transact(request,true);
            if(reply.Class==Stun.Success)return reply;
            int code=Stun.ReadErrorCode(reply.Get(Stun.ErrorCode));
            if(code==438 && reply.Get(Stun.Nonce)!=null){nonce=reply.Get(Stun.Nonce);continue;}
            throw new IOException("Le relais a refusé la demande (code "+code+").");
        }
        throw new IOException("Le relais a refusé la demande.");
    }

    public void Allocate() {
        // The first Allocate is unauthenticated on purpose: the 401 carries the
        // realm and nonce the signed one needs.
        var probe=Stun.New(Stun.Allocate,Stun.Request).Add(Stun.RequestedTransport,new byte[]{17,0,0,0});
        var challenge=Transact(probe,false);
        if(challenge.Class!=Stun.Error || Stun.ReadErrorCode(challenge.Get(Stun.ErrorCode))!=401 || challenge.Get(Stun.Realm)==null || challenge.Get(Stun.Nonce)==null)
            throw new IOException("Le relais ne propose pas d'authentification.");
        realm=Encoding.UTF8.GetString(challenge.Get(Stun.Realm));nonce=challenge.Get(Stun.Nonce);key=Stun.LongTermKey(username,realm,password);
        var reply=Authenticated(Stun.Allocate,r=>r.Add(Stun.RequestedTransport,new byte[]{17,0,0,0}));
        Relayed=Stun.ReadXorAddress(reply.Get(Stun.XorRelayedAddress));Mapped=Stun.ReadXorAddress(reply.Get(Stun.XorMappedAddress));
        if(Relayed==null)throw new IOException("Le relais n'a pas donné d'adresse.");
        var granted=Stun.ReadU32(reply.Get(Stun.Lifetime));if(granted>=60)lifetime=granted;
        // An allocation lives `lifetime` seconds, a permission five minutes and a
        // channel binding ten: one timer refreshes all three well before.
        refresher=new Timer(_=>Refresh(),null,TimeSpan.FromSeconds(Math.Min(120,lifetime/3)),TimeSpan.FromSeconds(Math.Min(120,lifetime/3)));
    }

    void Refresh() {
        if(closed)return;
        try {
            Authenticated(Stun.Refresh,r=>r.Add(Stun.Lifetime,Stun.U32(lifetime)));
            IPAddress[] addresses;lock(permitted)addresses=permitted.ToArray();
            foreach(var a in addresses)CreatePermissionNow(a);
            foreach(var c in peerOf.ToArray())BindNow(c.Value,c.Key);
            Interlocked.Increment(ref refreshes);
        } catch {if(!closed)Interlocked.Increment(ref refreshFailures);}
    }

    // Lets datagrams from this address reach us. TURN permissions are per
    // address, not per port, so a guest behind a NAT that changes ports still
    // gets through once the host has named its address.
    public void Permit(IPAddress address) {
        lock(permitted){if(!permitted.Add(address))return;}
        CreatePermissionNow(address);
    }
    void CreatePermissionNow(IPAddress address) {
        Authenticated(Stun.CreatePermission,r=>r.Add(Stun.XorPeerAddress,Stun.XorAddress(new IPEndPoint(address,0))));
    }
    void BindNow(IPEndPoint peer,ushort channel) {
        Authenticated(Stun.ChannelBind,r=>r.Add(Stun.ChannelNumber,new byte[]{(byte)(channel>>8),(byte)channel,0,0}).Add(Stun.XorPeerAddress,Stun.XorAddress(peer)));
    }

    public void Send(byte[] data,int length,IPEndPoint to) {
        if(closed)throw new ObjectDisposedException("TurnClient");
        ushort channel;
        if(channelOf.TryGetValue(Key(to),out channel)) {
            // ChannelData: four bytes of header instead of a 36-byte indication.
            var frame=new byte[4+length];frame[0]=(byte)(channel>>8);frame[1]=(byte)channel;frame[2]=(byte)(length>>8);frame[3]=(byte)length;
            Buffer.BlockCopy(data,0,frame,4,length);server.Send(frame,frame.Length);return;
        }
        var payload=new byte[length];Buffer.BlockCopy(data,0,payload,0,length);
        var indication=Stun.New(Stun.SendMethod,Stun.Indication).Add(Stun.XorPeerAddress,Stun.XorAddress(to)).Add(Stun.Data,payload);
        var bytes=Stun.Encode(indication);server.Send(bytes,bytes.Length);
        // Bind a channel for this peer in the background; until it answers, the
        // indications above keep the traffic moving.
        if(binding.TryAdd(Key(to),0))Task.Run(()=>{
            try {
                ushort next=(ushort)Interlocked.Increment(ref nextChannel);if(next>0x7FFF)return;
                BindNow(to,next);peerOf[next]=to;channelOf[Key(to)]=next;
            } catch {byte ignored;binding.TryRemove(Key(to),out ignored);}
        });
    }

    public byte[] Receive(ref IPEndPoint from) {
        Tuple<byte[],IPEndPoint> next;
        if(!inbox.TryDequeue(out next))throw new SocketException((int)SocketError.WouldBlock);
        from=next.Item2;return next.Item1;
    }
    public void NonBlocking(){}

    void Read() {
        while(!closed) {
            byte[] data;IPEndPoint from=null;
            try{data=server.Receive(ref from);}catch(SocketException){if(closed)return;continue;}catch(ObjectDisposedException){return;}
            if(data.Length>=4 && data[0]>=0x40 && data[0]<=0x7F) {
                int channel=data[0]<<8|data[1],length=data[2]<<8|data[3];IPEndPoint peer;
                if(4+length<=data.Length && peerOf.TryGetValue((ushort)channel,out peer)){var payload=new byte[length];Buffer.BlockCopy(data,4,payload,0,length);Enqueue(payload,peer);}
                continue;
            }
            var m=Stun.Decode(data,data.Length);if(m==null)continue;
            if(m.Method==Stun.DataMethod && m.Class==Stun.Indication) {
                var peer=Stun.ReadXorAddress(m.Get(Stun.XorPeerAddress));var payload=m.Get(Stun.Data);
                if(peer!=null && payload!=null)Enqueue(payload,peer);
                continue;
            }
            TaskCompletionSource<Stun.Message> waiting;
            if(pending.TryGetValue(Key(m.Transaction),out waiting)) {
                // A success to a signed request must carry a valid signature;
                // challenges (401/438) are unsigned by nature.
                if(m.Class==Stun.Success && key!=null && !Stun.Verify(m,key))continue;
                waiting.TrySetResult(m);
            }
        }
    }
    void Enqueue(byte[] payload,IPEndPoint peer){if(inbox.Count<4096)inbox.Enqueue(Tuple.Create(payload,peer));}

    public void Dispose() {
        if(closed)return;closed=true;
        if(refresher!=null)refresher.Dispose();
        // Lifetime zero releases the allocation now rather than in ten minutes.
        try{var bye=Stun.New(Stun.Refresh,Stun.Request).Add(Stun.Lifetime,Stun.U32(0)).Add(Stun.Username,Encoding.UTF8.GetBytes(username)).Add(Stun.Realm,Encoding.UTF8.GetBytes(realm??"")).Add(Stun.Nonce,nonce??new byte[0]);
            var bytes=Stun.Encode(bye,key,true);server.Send(bytes,bytes.Length);}catch{}
        server.Close();
    }
}

// A WebSocket seen as a Stream, so the lobby's TLS (SslStream) runs through the
// relay unchanged: the relay only ever carries ciphertext. Text frames are the
// relay's own notices and go to Notice; binary frames are the stream.
sealed class WebSocketStream : Stream {
    readonly WebSocket socket;byte[] buffered=new byte[0];int offset;
    public Action<string> Notice {get;set;}
    public WebSocketStream(WebSocket ws){socket=ws;ReadTimeout=Timeout.Infinite;WriteTimeout=Timeout.Infinite;}
    public override bool CanRead {get{return true;}}
    public override bool CanWrite {get{return true;}}
    public override bool CanSeek {get{return false;}}
    public override bool CanTimeout {get{return true;}}
    public override int ReadTimeout {get;set;}
    public override int WriteTimeout {get;set;}
    public override long Length {get{throw new NotSupportedException();}}
    public override long Position {get{throw new NotSupportedException();}set{throw new NotSupportedException();}}
    public override void Flush(){}
    public override long Seek(long o,SeekOrigin s){throw new NotSupportedException();}
    public override void SetLength(long v){throw new NotSupportedException();}
    static CancellationTokenSource Deadline(int ms){return ms==Timeout.Infinite?new CancellationTokenSource():new CancellationTokenSource(ms);}

    // Returns the next text notice, or null when a binary frame came first and
    // is now buffered for Read.
    public string ReadMessage(out bool binary) {
        var chunk=new byte[16384];var message=new MemoryStream();
        using(var deadline=Deadline(ReadTimeout)) {
            while(true) {
                WebSocketReceiveResult r;
                try{r=socket.ReceiveAsync(new ArraySegment<byte>(chunk),deadline.Token).GetAwaiter().GetResult();}
                catch(OperationCanceledException){throw new IOException("Le relais ne répond plus.");}
                catch(WebSocketException e){throw new IOException("Le relais a fermé la connexion.",e);}
                if(r.MessageType==WebSocketMessageType.Close){Closed=true;CloseCode=(int?)r.CloseStatus??0;binary=false;return null;}
                message.Write(chunk,0,r.Count);
                if(!r.EndOfMessage)continue;
                binary=r.MessageType==WebSocketMessageType.Binary;
                if(binary){buffered=message.ToArray();offset=0;return null;}
                return Encoding.UTF8.GetString(message.ToArray());
            }
        }
    }
    // Set once the relay has closed this socket, with the code it gave:
    // 4429 a room that is taken, 4404 a room with no host.
    public bool Closed {get;private set;}
    public int CloseCode {get;private set;}
    public override int Read(byte[] destination,int at,int count) {
        while(offset>=buffered.Length) {
            if(Closed)return 0;
            bool binary;var text=ReadMessage(out binary);
            if(Closed)return 0;
            if(!binary && text!=null && Notice!=null)Notice(text);
        }
        int n=Math.Min(count,buffered.Length-offset);Buffer.BlockCopy(buffered,offset,destination,at,n);offset+=n;return n;
    }
    // A request of our own to the relay, as text: the room's host socket is
    // never a pipe, so nothing else writes on it.
    readonly object sendLock=new object();
    public void SendText(string text) {
        var bytes=Encoding.UTF8.GetBytes(text);
        lock(sendLock) using(var deadline=Deadline(WriteTimeout==Timeout.Infinite?10000:WriteTimeout)) {
            try{socket.SendAsync(new ArraySegment<byte>(bytes),WebSocketMessageType.Text,true,deadline.Token).GetAwaiter().GetResult();}
            catch(OperationCanceledException){throw new IOException("Le relais ne répond plus.");}
            catch(WebSocketException e){throw new IOException("Le relais a fermé la connexion.",e);}
        }
    }
    public override void Write(byte[] source,int at,int count) {
        using(var deadline=Deadline(WriteTimeout)) {
            try{socket.SendAsync(new ArraySegment<byte>(source,at,count),WebSocketMessageType.Binary,true,deadline.Token).GetAwaiter().GetResult();}
            catch(OperationCanceledException){throw new IOException("Le relais ne répond plus.");}
            catch(WebSocketException e){throw new IOException("Le relais a fermé la connexion.",e);}
        }
    }
    protected override void Dispose(bool disposing) {if(disposing){try{socket.Abort();}catch{}socket.Dispose();}base.Dispose(disposing);}
}

// The rendezvous: a Cloudflare Worker that pairs WebSockets opened outward by the
// host and its guests, for when the host's box cannot be opened. It hands out
// short-lived TURN credentials too. Nothing secret is in this program; the room
// is named by a hash of the invitation token, never the token itself, and the
// lobby's TLS with its pinned certificate runs end to end inside.
static class Relay {
    // The deployed Worker. PARTYBOARD_RELAY overrides it (tests, a private relay);
    // an empty value turns the relay off.
    const string DefaultUrl="https://partyboard-relay.zeranemesis.workers.dev";
    public static string BaseUrl {get{var v=Environment.GetEnvironmentVariable("PARTYBOARD_RELAY");return (v??DefaultUrl).TrimEnd('/');}}
    public static bool Available {get{return BaseUrl.Length>0;}}
    // Tests against a local relay (wrangler dev has no TURN keys): a loopback
    // TURN and STUN server instead of Cloudflare's. Set by Tests.cs only.
    internal static Tuple<string,string,IPEndPoint> TestTurn;internal static IPEndPoint TestStun;
    public static string RoomId(byte[] token) {
        var hash=Wire.Hash(Encoding.ASCII.GetBytes("PBRELAY1").Concat(token).ToArray());
        return BitConverter.ToString(hash,0,16).Replace("-","").ToLowerInvariant();
    }
    static Uri Socket(string path){var b=BaseUrl;return new Uri((b.StartsWith("https:")?"wss:"+b.Substring(6):b.StartsWith("http:")?"ws:"+b.Substring(5):b)+path);}

    public static WebSocketStream Connect(string path,int timeoutMs=10000) {
        var ws=new ClientWebSocket();ws.Options.KeepAliveInterval=TimeSpan.FromSeconds(20);
        using(var deadline=new CancellationTokenSource(timeoutMs)) {
            try{ws.ConnectAsync(Socket(path),deadline.Token).GetAwaiter().GetResult();}
            catch(Exception e){ws.Dispose();throw new IOException("Le relais en ligne n'est pas joignable.",e);}
        }
        return new WebSocketStream(ws);
    }

    // username, credential and the TURN server's UDP endpoint.
    public static Tuple<string,string,IPEndPoint> TurnCredentials() {
        string json;
        using(var client=new WebClient()) {client.Headers[HttpRequestHeader.UserAgent]="PartyBoard-Online";json=client.UploadString(BaseUrl+"/turn","POST","{}");}
        var reply=new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(json);
        var username=reply["username"] as string;var credential=reply["credential"] as string;
        var urls=((System.Collections.ArrayList)reply["urls"]).Cast<object>().Select(u=>(string)u).ToArray();
        // turn:host:port?transport=udp -- the UDP one on 3478, the port TURN is for.
        var url=urls.FirstOrDefault(u=>u.StartsWith("turn:") && u.Contains("transport=udp") && u.Contains(":3478"))??urls.FirstOrDefault(u=>u.StartsWith("turn:") && u.Contains("transport=udp"));
        if(username==null || credential==null || url==null)throw new IOException("Le relais n'a pas donné d'accès.");
        var hostPort=url.Substring(5).Split('?')[0];var colon=hostPort.LastIndexOf(':');
        var host=hostPort.Substring(0,colon);int port=int.Parse(hostPort.Substring(colon+1));
        var address=Dns.GetHostAddresses(host).First(a=>a.AddressFamily==AddressFamily.InterNetwork);
        return Tuple.Create(username,credential,new IPEndPoint(address,port));
    }

    // Host side. The room tells us when a guest arrives, with the address TURN
    // will see: that address is permitted, then the guest's connection is opened
    // and handed to offer, which takes it like a TCP one.
    public static void HostNotices(string room,WebSocketStream control,TurnClient allocation,Func<bool> stop,Action<WebSocketStream> offer,Action<string> report,Action<Dictionary<string,object>> other=null,Func<bool> accepting=null) {
        try {
            while(!stop()) {
                bool binary;var notice=control.ReadMessage(out binary);
                if(control.Closed)break;if(notice==null)continue;
                Dictionary<string,object> n;try{n=Parse(notice);}catch{continue;}
                object type,conn,ip;
                if(!n.TryGetValue("type",out type))continue;
                if((type as string)!="incoming"){if(other!=null)other(n);continue;}
                if(!n.TryGetValue("conn",out conn) || !(conn is string))continue;
                // A room that has its player keeps its socket only to stay on the
                // list; nobody else is let in.
                if(accepting!=null && !accepting()){try{control.SendText("{\"type\":\"refuse\",\"conn\":\""+(string)conn+"\"}");}catch{}continue;}
                IPAddress address;
                if(n.TryGetValue("ip",out ip) && IPAddress.TryParse(ip as string,out address) && address.AddressFamily==AddressFamily.InterNetwork)
                    try{allocation.Permit(address);}catch(Exception e){report("relay_permit_failed="+e.GetType().Name);}
                var id=(string)conn;
                Task.Run(()=>{
                    try {var pipe=Connect("/room/"+room+"/accept/"+id);pipe.ReadTimeout=8000;pipe.WriteTimeout=8000;offer(pipe);}
                    catch(Exception e){report("relay_accept_failed="+e.GetType().Name);}
                });
            }
        } catch {}
    }

    // Guest side: one channel through the room. The host's TURN address arrives
    // in "hello"; the TLS may start once the host has taken the connection,
    // which the room says with "ready".
    public static WebSocketStream GuestPipe(string room,string self,out IPEndPoint hostUdp) {
        hostUdp=null;
        var pipe=Connect("/room/"+room+"/guest"+(self!=null?"?ip="+self:""));
        try {
            pipe.ReadTimeout=10000;
            while(true) {
                bool binary;var notice=pipe.ReadMessage(out binary);
                if(pipe.Closed) {
                    if(pipe.CloseCode==4429) throw new IOException("Cette room vient d'être prise par un autre joueur. Choisissez-en une autre, ou créez la vôtre.");
                    if(pipe.CloseCode==4404) throw new IOException("Cette room n'existe plus : son hôte l'a fermée.");
                    throw new IOException("Votre ami n'est pas joignable, ni directement ni par le relais en ligne. Vérifiez qu'il a laissé sa fenêtre ouverte.");
                }
                if(notice==null)continue;
                Dictionary<string,object> n;try{n=Parse(notice);}catch{continue;}
                object type,udp;n.TryGetValue("type",out type);
                if((type as string)=="hello" && n.TryGetValue("udp",out udp))hostUdp=Endpoint(udp as string);
                else if((type as string)=="ready"){pipe.ReadTimeout=8000;pipe.WriteTimeout=8000;return pipe;}
            }
        } catch {pipe.Dispose();throw;}
    }

    // This socket's public IPv4 as a STUN server sees it (stun.cloudflare.com
    // unless told otherwise): the address the host must permit on its TURN
    // allocation. Null when it cannot be learnt; the room then falls back to the
    // address the guest's WebSocket came from.
    public static string PublicAddress(UdpClient socket,IPEndPoint stunServer=null) {
        try {
            var server=stunServer??TestStun??new IPEndPoint(Dns.GetHostAddresses("stun.cloudflare.com").First(a=>a.AddressFamily==AddressFamily.InterNetwork),3478);
            var request=Stun.New(Stun.Binding,Stun.Request);var bytes=Stun.Encode(request);
            socket.Client.ReceiveTimeout=1500;
            for(int i=0;i<2;i++) {
                socket.Send(bytes,bytes.Length,server);
                try {
                    IPEndPoint from=null;var reply=socket.Receive(ref from);var m=Stun.Decode(reply,reply.Length);
                    if(m!=null && Wire.Equal(m.Transaction,request.Transaction)){var mapped=Stun.ReadXorAddress(m.Get(Stun.XorMappedAddress));if(mapped!=null)return mapped.Address.ToString();}
                } catch(SocketException) {}
            }
            return null;
        } catch {return null;} finally {try{socket.Client.ReceiveTimeout=0;}catch{}}
    }

    // The public list (relay-worker: GET /rooms). Only rooms this player could
    // start are returned: the same PartyBoard build and the same disc.
    public sealed class PublicRoom {
        public string Room,Name,Language,Region,Game="GMPE01",State="waiting";public int Age,Playing,MaxPlayers=2;
        public List<string> Players=new List<string>();public List<RoomMod> Mods=new List<RoomMod>();
        public bool Waiting {get{return State=="waiting";}}
    }
    public sealed class RoomMod {public int Id;public string Name,Fingerprint;}
    // What a listed room says about itself: who is in it, with which mods, and
    // whether it waits for a player or plays.
    public sealed class RoomStatus {
        public string State="waiting",Game="GMPE01";public List<string> Players=new List<string>();public int MaxPlayers=2;public ModSet Mods=ModSet.Empty;
        public Dictionary<string,object> Fields() {
            return new Dictionary<string,object>{{"state",State},{"game",Game},{"players",Players.ToArray()},{"max",MaxPlayers},
                {"mods",Mods.Entries.Select(e=>new Dictionary<string,object>{{"id",e.Id},{"name",e.Name},{"fp",BitConverter.ToString(e.Fingerprint).Replace("-","").ToLowerInvariant()}}).ToArray()}};
        }
        public string Key {get{return new JavaScriptSerializer().Serialize(Fields());}}
    }
    public static string StatusRequest(RoomStatus status) {
        var fields=status.Fields();fields["type"]="status";return new JavaScriptSerializer().Serialize(fields);
    }
    public static string Short(byte[] hash){return BitConverter.ToString(hash,0,8).Replace("-","").ToLowerInvariant();}
    static string Get(string path) {
        using(var client=new WebClient()) {
            client.Headers[HttpRequestHeader.UserAgent]="PartyBoard-Online";client.Encoding=Encoding.UTF8;
            try{return client.DownloadString(BaseUrl+path);}
            catch(WebException e) {
                var r=e.Response as HttpWebResponse;
                if(r!=null && r.StatusCode==HttpStatusCode.NotFound)throw new IOException("Cette room n'existe plus : son hôte l'a fermée ou quelqu'un l'a déjà rejointe.",e);
                if(r!=null && (int)r.StatusCode==429)throw new IOException("Trop de demandes au relais en ligne. Attendez une minute.",e);
                throw new IOException("Le relais en ligne n'est pas joignable.",e);
            }
        }
    }
    public static List<PublicRoom> PublicRooms(byte[] build,byte[] disc) {
        var reply=Parse(Get("/rooms?build="+Short(build)+"&disc="+Short(disc)));
        var rooms=new List<PublicRoom>();object list;
        if(!reply.TryGetValue("rooms",out list) || !(list is System.Collections.ArrayList))return rooms;
        foreach(var item in (System.Collections.ArrayList)list) {
            var r=item as Dictionary<string,object>;if(r==null)continue;object v;
            var room=new PublicRoom{Room=r.TryGetValue("room",out v)?v as string:null,Name=r.TryGetValue("name",out v)?v as string:null,
                Language=r.TryGetValue("lang",out v)?v as string:"en",Region=r.TryGetValue("region",out v)?v as string:"us",Age=r.TryGetValue("age",out v) && v is int?(int)v:0};
            if(r.TryGetValue("game",out v) && v is string)room.Game=(string)v;
            if(r.TryGetValue("state",out v) && v is string)room.State=(string)v;
            if(r.TryGetValue("playing",out v) && v is int)room.Playing=(int)v;
            if(r.TryGetValue("max",out v) && v is int)room.MaxPlayers=(int)v;
            if(r.TryGetValue("players",out v) && v is System.Collections.ArrayList)room.Players=((System.Collections.ArrayList)v).OfType<string>().ToList();
            if(room.Players.Count==0 && room.Name!=null)room.Players.Add(room.Name);
            if(r.TryGetValue("mods",out v) && v is System.Collections.ArrayList)
                foreach(var m in ((System.Collections.ArrayList)v).OfType<Dictionary<string,object>>()) {
                    object id,name,fp;
                    if(m.TryGetValue("id",out id) && id is int && m.TryGetValue("fp",out fp) && fp is string)room.Mods.Add(new RoomMod{Id=(int)id,Name=m.TryGetValue("name",out name)?name as string??"":"",Fingerprint=(string)fp});
                }
            if(room.Room!=null && room.Room.Length==32 && room.Name!=null)rooms.Add(room);
        }
        return rooms;
    }
    public static string PublicInvite(string room) {
        if(room==null || room.Length!=32 || room.Any(c=>!Uri.IsHexDigit(c)))throw new IOException("Room inconnue.");
        object invite;
        if(!Parse(Get("/rooms/"+room)).TryGetValue("invite",out invite) || !(invite is string))throw new IOException("Cette room n'existe plus.");
        return (string)invite;
    }
    public static string PublishRequest(Invitation invite,string name,byte language,bool pal,byte[] build,byte[] disc,RoomStatus status=null) {
        var fields=new Dictionary<string,object>{
            {"type","publish"},{"invite",invite.ForPublicList().Encode()},{"name",name},{"lang",GameLanguage.Code(language)},
            {"region",pal?"eu":"us"},{"build",Short(build)},{"disc",Short(disc)}};
        if(status!=null)foreach(var f in status.Fields())fields[f.Key]=f.Value;
        return new JavaScriptSerializer().Serialize(fields);
    }

    public static Dictionary<string,object> Parse(string notice){return new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(notice);}
    public static IPEndPoint Endpoint(string text) {
        if(text==null)return null;var colon=text.LastIndexOf(':');IPAddress a;int port;
        return colon>0 && IPAddress.TryParse(text.Substring(0,colon),out a) && int.TryParse(text.Substring(colon+1),out port) && port>0 && port<65536?new IPEndPoint(a,port):null;
    }
}
}
