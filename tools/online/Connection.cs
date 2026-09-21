using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;
using System.Collections.Concurrent;
using System.Collections.Generic;

namespace PartyBoardOnline {
static class Wire {
    public static void Write(this Stream s,byte[] b) {s.Write(b,0,b.Length);}
    public static byte[] Random(int n) { var b = new byte[n]; using(var r = RandomNumberGenerator.Create()) r.GetBytes(b); return b; }
    public static byte[] Hash(byte[] b) { using(var h=SHA256.Create()) return h.ComputeHash(b); }
    public static bool Equal(byte[] a, byte[] b) { if(a==null || b==null || a.Length!=b.Length) return false; int d=0; for(int i=0;i<a.Length;i++) d|=a[i]^b[i]; return d==0; }
    public static byte[] Read(Stream s,int n) { var b=new byte[n]; int p=0; while(p<n) { int k=s.Read(b,p,n-p); if(k==0) throw new IOException("L'autre joueur s'est déconnecté."); p+=k; } return b; }
    public static byte[] BuildHash(string root) {
        using(var h=SHA256.Create()) using(var m=new MemoryStream()) {
            var paths=Directory.GetFiles(root,"*.dll").Concat(new[]{Path.Combine(root,"partyboard.exe"),Path.Combine(root,"PartyBoardOnline.exe")}).OrderBy(x=>Path.GetFileName(x),StringComparer.OrdinalIgnoreCase);
            foreach(var p in paths) { byte[] name=Encoding.UTF8.GetBytes(Path.GetFileName(p).ToLowerInvariant()+"\n"); m.Write(name,0,name.Length); using(var f=File.OpenRead(p)) {var b=h.ComputeHash(f); m.Write(b,0,b.Length);} }
            return h.ComputeHash(m.ToArray());
        }
    }
    public static X509Certificate2 Certificate() {
        using(var rsa=RSA.Create()) {
            rsa.KeySize=2048;
            var req=new CertificateRequest("CN=PartyBoard session",rsa,HashAlgorithmName.SHA256,RSASignaturePadding.Pkcs1);
            req.CertificateExtensions.Add(new X509BasicConstraintsExtension(false,false,0,true));
            req.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature|X509KeyUsageFlags.KeyEncipherment,true));
            req.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(new OidCollection{new Oid("1.3.6.1.5.5.7.3.1")},true));
            using(var cert=req.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5),DateTimeOffset.UtcNow.AddHours(12))) {
                // A temporary PFX import makes the private key usable by Schannel
                // on .NET Framework. Nothing is installed in a certificate store.
                var identity=WindowsIdentity.GetCurrent();
                var keyDirectory=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "Microsoft","Crypto","RSA",identity.User.Value);
                Directory.CreateDirectory(keyDirectory);
                return new X509Certificate2(cert.Export(X509ContentType.Pfx));
            }
        }
    }
    // A guest opens two channels, and with three guests the host has six connections
    // arriving in any order and no way to tell whose is whose: the channel byte says
    // "read" or "write", never "mine". The client id says that, and it is generated
    // per guest rather than per connection so the host can pair the two.
    public static SslStream Client(TcpClient tcp,Invitation invite,byte[] build,byte channel=0,byte[] clientId=null) {
        if(clientId==null)clientId=new byte[16];
        if(clientId.Length!=16)throw new IOException("Identifiant de connexion incorrect.");
        tcp.NoDelay=true; tcp.ReceiveTimeout=8000; tcp.SendTimeout=8000;
        var ssl=new SslStream(tcp.GetStream(),false,(s,c,ch,e)=>c!=null && Equal(Hash(c.GetRawCertData()).Take(invite.Fingerprint.Length).ToArray(),invite.Fingerprint));
        try {
            ssl.ReadTimeout=8000; ssl.WriteTimeout=8000;
            ssl.AuthenticateAsClient("PartyBoard session",null,SslProtocols.Tls12,false);
            byte[] hello=Encoding.ASCII.GetBytes("PBAUTO3\n").Concat(Hash(invite.Token)).Concat(build).Concat(new[]{channel}).Concat(clientId).ToArray();
            ssl.Write(hello); ssl.Flush();
            byte ack=Read(ssl,1)[0];
            if(ack==2) throw new IOException("Les versions du jeu sont différentes. Copiez le même dossier PartyBoard sur les deux PC.");
            if(ack!=1) throw new IOException("Cette invitation n'est plus valable. Demandez-en une nouvelle.");
            ssl.ReadTimeout=120000; ssl.WriteTimeout=120000; return ssl;
        } catch {ssl.Dispose(); throw;}
    }
    public static SslStream Server(TcpClient tcp,X509Certificate2 cert,Invitation invite,byte[] build,out byte channel) {
        byte[] ignored;return Server(tcp,cert,invite,build,out channel,out ignored);
    }
    public static SslStream Server(TcpClient tcp,X509Certificate2 cert,Invitation invite,byte[] build,out byte channel,out byte[] clientId) {
        channel=255;clientId=null;
        tcp.NoDelay=true; tcp.ReceiveTimeout=8000; tcp.SendTimeout=8000;
        var ssl=new SslStream(tcp.GetStream(),false);
        try {
            ssl.ReadTimeout=8000; ssl.WriteTimeout=8000;
            ssl.AuthenticateAsServer(cert,false,SslProtocols.Tls12,false);
            var hello=Read(ssl,89);channel=hello[72];clientId=hello.Skip(73).Take(16).ToArray();
            if(!Equal(hello.Take(8).ToArray(),Encoding.ASCII.GetBytes("PBAUTO3\n")) || channel>1 ||
                !Equal(hello.Skip(8).Take(32).ToArray(),Hash(invite.Token)) || DateTime.UtcNow>invite.Expires)
                throw new AuthenticationException("Invitation refusée.");
            if(!Equal(hello.Skip(40).Take(32).ToArray(),build)) {ssl.WriteByte(2); ssl.Flush(); throw new AuthenticationException("Versions différentes.");}
            ssl.WriteByte(1); ssl.Flush(); ssl.ReadTimeout=120000; ssl.WriteTimeout=120000; return ssl;
        } catch {ssl.Dispose(); throw;}
    }
}

sealed class Invitation {
    public IPAddress Address; public int Port; public DateTime Expires;
    // The host's address on its own network, alongside its public one. Two PCs
    // in the same house are zero milliseconds apart, but an invitation that
    // carries only the public address sends them out through the box and back
    // in - measured at 15 ms between two machines on one switch, and cut by the
    // router after forty to fifty seconds, three sessions out of three. The
    // guest tries this one first and keeps the public address as the fallback,
    // so nothing is lost when the two really are far apart.
    public IPAddress LocalAddress=IPAddress.Any; public int LocalPort;
    public byte[] Fingerprint,Token,Build;
    // How many seats the host chose when the salon was created, 2 to
    // Lobby.MaxSeats. A guest needs this before it connects at all, since it
    // decides whether the link it is about to open is the classic single-peer
    // one or a control-only leg of a mesh -- OpenMesh() cannot wait for the
    // lobby to say so, because in mesh mode the lobby exists before any guest
    // does.
    public int MaxPlayers=2;
    public bool HasLocalPath {get{return LocalPort>0 && LocalAddress!=null && !LocalAddress.Equals(IPAddress.Any) && !LocalAddress.Equals(Address);}}
    public string Encode() {
        if(Fingerprint.Length!=16 || Token.Length!=16)throw new IOException("Invitation incompatible.");
        if(MaxPlayers<2 || MaxPlayers>Lobby.MaxSeats)throw new IOException("Invitation incompatible.");
        using(var m=new MemoryStream()) using(var w=new BinaryWriter(m)) {
            w.Write(Address.GetAddressBytes());w.Write((ushort)Port);
            w.Write((uint)(Expires-new DateTime(1970,1,1,0,0,0,DateTimeKind.Utc)).TotalSeconds);
            w.Write(Fingerprint);w.Write(Token);
            w.Write((LocalAddress??IPAddress.Any).GetAddressBytes());w.Write((ushort)LocalPort);
            w.Write((byte)MaxPlayers);
            return "PB4."+Convert.ToBase64String(m.ToArray()).Replace('+','-').Replace('/','_');
        }
    }
    public static Invitation Decode(string text,bool localTest=false) {
        try {
            if(text==null) throw new FormatException(); text=text.Trim();
            // A PB2 invitation carries no local address, and a PB3 carries no
            // player count. Both are refused by name rather than left to decode
            // short and fail later as "incomplete" -- the two PCs must share the
            // same build anyway, so a mismatched prefix only ever means one of
            // them has an old folder.
            if(text.StartsWith("PB2.",StringComparison.Ordinal))
                throw new IOException("Cette invitation vient d'une version plus ancienne de PartyBoard. Copiez le même dossier sur les deux PC et recréez le salon.");
            if(text.StartsWith("PB3.",StringComparison.Ordinal))
                throw new IOException("Cette invitation vient d'une version plus ancienne de PartyBoard. Copiez le même dossier sur les deux PC et recréez le salon.");
            if(text.Length!=72 || !text.StartsWith("PB4.",StringComparison.Ordinal)) throw new FormatException();
            var b64=text.Substring(4).Replace('-','+').Replace('_','/'); b64+=new string('=',(4-b64.Length%4)%4);
            var data=Convert.FromBase64String(b64); if(data.Length!=49) throw new FormatException();
            using(var r=new BinaryReader(new MemoryStream(data))) {
                var i=new Invitation{Address=new IPAddress(r.ReadBytes(4)),Port=r.ReadUInt16(),Expires=new DateTime(1970,1,1,0,0,0,DateTimeKind.Utc).AddSeconds(r.ReadUInt32()),Fingerprint=r.ReadBytes(16),Token=r.ReadBytes(16)};
                i.LocalAddress=new IPAddress(r.ReadBytes(4));i.LocalPort=r.ReadUInt16();
                i.MaxPlayers=r.ReadByte();
                if(i.MaxPlayers<2 || i.MaxPlayers>Lobby.MaxSeats) throw new FormatException();
                if(i.Port==0 || (!localTest && !Gateway.Public(i.Address))) throw new FormatException();
                // The local address is deliberately NOT required to be public -
                // that is the whole point - but it must be a private address, so
                // an invitation cannot redirect the first attempt anywhere else.
                // Reaching the wrong machine is harmless anyway: the TLS
                // handshake pins the certificate fingerprint from the invitation.
                if(i.LocalPort!=0 && !Gateway.Private(i.LocalAddress)) {i.LocalAddress=IPAddress.Any;i.LocalPort=0;}
                if(i.Expires<DateTime.UtcNow) throw new IOException("Cette invitation a expiré. L'hôte doit recréer une partie.");
                if(i.Expires>DateTime.UtcNow.AddMinutes(31)) throw new FormatException();
                return i;
            }
        } catch(IOException) {throw;} catch {throw new IOException("L'invitation est incomplète. Copiez-la entièrement depuis le PC de votre ami.");}
    }
}

// Pairs the two TLS channels of one guest, which arrive independently and in
// either order, keyed by the client identifier the handshake carries. Generic
// over the channel's payload so the accept loop's bookkeeping -- the part that
// decides when a guest is fully connected -- can be tested without a socket
// or a certificate in sight.
sealed class ChannelPairing<T> {
    readonly Dictionary<string,T[]> pending=new Dictionary<string,T[]>();
    static string Key(byte[] clientId) {
        if(clientId==null || clientId.Length!=16)throw new IOException("Identifiant de connexion incorrect.");
        return Convert.ToBase64String(clientId);
    }
    public int Pending {get{return pending.Count;}}
    // Returns the completed pair, indexed by channel, the moment the second
    // channel for a guest arrives; returns null while still waiting on the
    // other one. A channel arriving twice for the same guest is refused rather
    // than silently replaced, since that would let a second connection steal
    // the first one's slot without anybody noticing.
    public T[] Add(byte[] clientId,int channel,T value) {
        if(channel!=0 && channel!=1)throw new IOException("Canal invalide.");
        string key=Key(clientId);
        T[] slots;
        if(!pending.TryGetValue(key,out slots)) {slots=new T[2];pending[key]=slots;}
        if(slots[channel]!=null)throw new IOException("Canal déjà connecté.");
        slots[channel]=value;
        if(slots[0]!=null && slots[1]!=null) {pending.Remove(key);return slots;}
        return null;
    }
}

static class GameDatagram {
    // Sized from the engine header by tools/build_online.ps1. A hardcoded copy
    // fell behind the 88 -> 152 byte change and the bridge dropped every game
    // packet, which reads to a player as "Aucun joueur compatible apres 2
    // minutes" with a lobby that looked perfectly connected.
    public const int Payload=WireFormat.PacketSize,Size=14+Payload+16;
    const int Header=14,Tag=16;
    public static byte[] Key(byte[] token) {return Wire.Hash(token.Concat(Encoding.ASCII.GetBytes("PartyBoard UDP v1")).ToArray());}
    public static byte[] Seal(byte[] key,int player,ulong sequence,byte[] payload) {
        // Seats 0..3. This said 0..1, which is a two-player limit sitting inside the
        // layer that authenticates game traffic -- the last place it would be looked
        // for, and one that no amount of work above it could get past.
        if(key==null || key.Length!=32 || payload==null || payload.Length!=Payload || player<0 || player>=Lobby.MaxSeats)throw new IOException("Paquet de jeu invalide.");
        var data=new byte[Size];data[0]=(byte)'P';data[1]=(byte)'B';data[2]=(byte)'U';data[3]=(byte)'1';data[4]=1;data[5]=(byte)player;
        Buffer.BlockCopy(BitConverter.GetBytes(sequence),0,data,6,8);Buffer.BlockCopy(payload,0,data,Header,Payload);
        using(var h=new HMACSHA256(key)) {var tag=h.ComputeHash(data,0,Header+Payload);Buffer.BlockCopy(tag,0,data,Header+Payload,Tag);}
        return data;
    }
    public static bool Open(byte[] key,int player,byte[] data,out ulong sequence,out byte[] payload) {
        sequence=0;payload=null;if(key==null || key.Length!=32 || data==null || data.Length!=Size || data[0]!='P' || data[1]!='B' || data[2]!='U' || data[3]!='1' || data[4]!=1 || data[5]!=player)return false;
        byte[] expected;using(var h=new HMACSHA256(key))expected=h.ComputeHash(data,0,Header+Payload);
        if(!Wire.Equal(expected.Take(Tag).ToArray(),data.Skip(Header+Payload).Take(Tag).ToArray()))return false;
        sequence=BitConverter.ToUInt64(data,6);payload=data.Skip(Header).Take(Payload).ToArray();return true;
    }
}

// Replay protection, one sliding window per sender.
//
// It used to be three fields, which is a single window -- correct while exactly
// one peer can send, and wrong the moment four can: a sequence number from one
// peer would move the window and make a fresh packet from another look like a
// replay. Four windows cost four words and remove the question.
sealed class ReplayWindow {
    readonly ulong[] highest=new ulong[Lobby.MaxSeats];
    readonly ulong[] window=new ulong[Lobby.MaxSeats];
    readonly bool[] seen=new bool[Lobby.MaxSeats];

    public bool IsNewest(int seat,ulong sequence){return !seen[seat] || sequence>highest[seat];}

    // True the first time a sequence is seen from that seat, false for a repeat or
    // for anything more than 64 behind its newest.
    public bool Accept(int seat,ulong sequence) {
        if(seat<0 || seat>=Lobby.MaxSeats)return false;
        if(!seen[seat]){seen[seat]=true;highest[seat]=sequence;window[seat]=1;return true;}
        if(sequence>highest[seat]) {
            ulong d=sequence-highest[seat];
            window[seat]=d>=64?1:(window[seat]<<(int)d)|1;highest[seat]=sequence;return true;
        }
        ulong behind=highest[seat]-sequence;if(behind>=64)return false;
        ulong bit=1UL<<(int)behind;if((window[seat]&bit)!=0)return false;
        window[seat]|=bit;return true;
    }
}

// TLS carries lobby/control messages. Time-sensitive game inputs use their own
// authenticated UDP path so one lost Internet packet cannot block later input.
sealed class ControlChannelException : IOException {
    public ControlChannelException(Exception error):base("La connexion du salon a ete interrompue.",error){}
}
sealed class Bridge : IDisposable {
    readonly TcpClient readTcp,writeTcp; readonly SslStream readSsl,writeSsl; readonly UdpClient udp,network;
    // Our seat, and the seat of the one player this bridge carries. The remote used
    // to be derived as "the other one", which names nobody once a session has
    // four seats. A mesh is this same bridge instantiated once per peer, each with
    // its own pair, so the peer has to be said rather than derived.
    readonly int localPlayer,remotePlayer;
    IPEndPoint game,networkPeer; readonly byte[] datagramKey; readonly bool learnNetworkPeer; volatile bool closed; int stopping; Timer heartbeat;
    long networkSequence;
    readonly ReplayWindow replay=new ReplayWindow();
    readonly IPEndPoint[] peerEndpoint=new IPEndPoint[Lobby.MaxSeats];
    long pingStamp; public Action<byte[]> Control; public Action<int> Ping;
    long udpPingStamp,lastUdpPongStamp;int committed,controlLost;
    public bool ControlConnected {get{return Volatile.Read(ref controlLost)==0;}}
    // Set only by the authenticated lobby commit callback, never by UDP input.
    public void CommitGame(){if(closed || !ControlConnected)throw new IOException("Le salon s'est ferme avant le depart.");Volatile.Write(ref committed,1);}
    public Action<string> Diagnostic;
    long lastUdpStamp,lastPumpStamp,deliveredPackets,udpDropped;int lastUdpError;
    static long Age(long stamp) {return stamp==0?-1:(Stopwatch.GetTimestamp()-stamp)*1000/Stopwatch.Frequency;}
    public string TransportStatus {get{return "control_connected="+ControlConnected+" udp_age_ms="+Age(Interlocked.Read(ref lastUdpStamp))+" udp_pong_age_ms="+Age(Interlocked.Read(ref lastUdpPongStamp))+" pump_age_ms="+Age(Interlocked.Read(ref lastPumpStamp))+" delivered_packets="+Interlocked.Read(ref deliveredPackets)+" udp_dropped="+Interlocked.Read(ref udpDropped)+" udp_error="+Volatile.Read(ref lastUdpError);}}
    // Every TLS write is owned by one worker. SslStream writes previously came
    // from the game relay, heartbeat, lobby and ping callback. Even though they
    // were locked, a slow Internet write could hold all of those producers and
    // eventually stop both peers. The bounded queue also coalesces the many
    // small input frames into fewer TLS records.
    readonly BlockingCollection<byte[]> toPeer=new BlockingCollection<byte[]>(new ConcurrentQueue<byte[]>(),4096);
    long gamePackets;public long GamePackets {get{return Interlocked.Read(ref gamePackets);}}
    long statePackets;public long StatePackets {get{return Interlocked.Read(ref statePackets);}}
    long peerPackets;public long PeerPackets {get{return Interlocked.Read(ref peerPackets);}}
    public bool UdpReady {get{long stamp=Interlocked.Read(ref lastUdpPongStamp);return stamp!=0 && Age(stamp)<5000;}}
    public int LocalPort { get {return ((IPEndPoint)udp.Client.LocalEndPoint).Port;} }
    public Bridge(TcpClient reader,SslStream readStream,TcpClient writer,SslStream writeStream,int player,UdpClient internet,IPEndPoint internetPeer,byte[] token,int hostGamePort=0,int peerPlayer=-1) {
        if(peerPlayer<0)peerPlayer=player^1;   // the two-player spelling, kept for its callers
        readTcp=reader;readSsl=readStream;writeTcp=writer;writeSsl=writeStream;localPlayer=player;remotePlayer=peerPlayer;network=internet;networkPeer=internetPeer;learnNetworkPeer=internetPeer==null;datagramKey=GameDatagram.Key(token);udp=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
        if(hostGamePort!=0) {game=new IPEndPoint(IPAddress.Loopback,hostGamePort); udp.Connect(game);}
    }
    internal static bool Packet(byte[] b,int player) {return b.Length==GameDatagram.Payload && b[0]==80 && b[1]==66 && b[2]==82 && b[3]==66 && b[4]==WireFormat.VersionHigh && b[5]==WireFormat.VersionLow && b[6]>=1 && b[6]<=3 && b[7]==player;}
    void Write(byte[] payload) {
        if(payload.Length<1 || payload.Length>256)throw new IOException("Message de salon trop long.");
        var frame=new byte[payload.Length+2];frame[0]=(byte)(payload.Length>>8);frame[1]=(byte)payload.Length;
        Buffer.BlockCopy(payload,0,frame,2,payload.Length);
        if(closed || !ControlConnected)throw new ControlChannelException(new IOException("Control channel closed."));
        try{if(toPeer.IsAddingCompleted || !toPeer.TryAdd(frame,1000))throw new ControlChannelException(new IOException("Control queue unavailable."));}
        catch(InvalidOperationException e){throw new ControlChannelException(e);}
    }
    public void SendControl(byte[] payload) {Write(payload);}

    void SendGame(byte[] payload) {
        var target=networkPeer;if(target==null)return;
        var packet=GameDatagram.Seal(datagramKey,localPlayer,(ulong)Interlocked.Increment(ref networkSequence),payload);
        try {network.Send(packet,packet.Length,target);}catch(SocketException e){if(!Recoverable(e,"udp_send"))throw;}
    }
    bool Recoverable(SocketException e,string operation) {
        if(e.SocketErrorCode==SocketError.WouldBlock || e.SocketErrorCode==SocketError.TimedOut)return true;
        // UDP ICMP errors and transient send pressure are packet loss. The native
        // input history can repair it; neither condition closes the lobby.
        if(e.SocketErrorCode!=SocketError.ConnectionReset && e.SocketErrorCode!=SocketError.ConnectionRefused
            && e.SocketErrorCode!=SocketError.NetworkUnreachable && e.SocketErrorCode!=SocketError.HostUnreachable
            && e.SocketErrorCode!=SocketError.NoBufferSpaceAvailable && e.SocketErrorCode!=SocketError.MessageSize)return false;
        Interlocked.Increment(ref udpDropped);
        if(Interlocked.Exchange(ref lastUdpError,e.NativeErrorCode)!=e.NativeErrorCode)Diagnostic?.Invoke("transport_event="+operation+" socket_code="+e.NativeErrorCode);
        return true;
    }
    byte[] ReadControl(int size){try{return Wire.Read(readSsl,size);}catch(IOException e){throw new ControlChannelException(e);}catch(ObjectDisposedException e){throw new ControlChannelException(e);}}
    void WriteControl(byte[] bytes,int size){try{writeSsl.Write(bytes,0,size);}catch(IOException e){throw new ControlChannelException(e);}catch(ObjectDisposedException e){throw new ControlChannelException(e);}}
    bool KeepCommittedGame() {
        if(closed || Volatile.Read(ref committed)==0)return false;
        if(Interlocked.Exchange(ref controlLost,1)==0){
            Diagnostic?.Invoke("event=control_lost game_transport=udp_continues");
            if(heartbeat!=null)heartbeat.Dispose();toPeer.CompleteAdding();
            // Closing the TCP sockets wakes both TLS workers, without touching
            // the input sockets or triggering the game startup cancellation.
            readTcp.Close();writeTcp.Close();
        }
        return true;
    }
    Task Worker(string name,Action work) {return Task.Factory.StartNew(()=>{try{work();}catch(Exception e){
        if(!closed){var socket=e.GetBaseException() as SocketException;Diagnostic?.Invoke("worker="+name+" error="+e.GetType().Name+(socket==null?"":" socket_code="+socket.NativeErrorCode));}
        if(e is ControlChannelException && KeepCommittedGame())return;
        Dispose();throw;
    }},CancellationToken.None,TaskCreationOptions.LongRunning,TaskScheduler.Default);}
    // internal rather than private: MeshRelay speaks the exact same heartbeat
    // wire format for its own discovery/keepalive, and duplicating these four
    // lines would be how the two protocols quietly drift apart.
    internal static byte[] Heartbeat(int player,long stamp) {var b=new byte[GameDatagram.Payload];b[0]=(byte)'P';b[1]=(byte)'B';b[2]=(byte)'H';b[3]=(byte)'B';b[4]=1;b[5]=(byte)player;b[6]=1;Buffer.BlockCopy(BitConverter.GetBytes(stamp),0,b,8,8);return b;}
    internal static bool IsHeartbeat(byte[] b,int player) {return b.Length==GameDatagram.Payload && b[0]=='P' && b[1]=='B' && b[2]=='H' && b[3]=='B' && b[4]==1 && b[5]==player && (b[6]==1 || b[6]==2);}
    // The tls_read worker's body. A single implementation so a control-only
    // link (RunControlOnly) and a full link (Run) read the wire exactly the
    // same way -- the two must never drift on what a message means.
    void ReceiveLoop() {
        while(!closed && ControlConnected) {
            var size=ReadControl(2); int n=(size[0]<<8)|size[1];
            // The player announcement now carries the mod list, which a disc hash and a
            // nickname alone never needed. 2048 leaves room for the 24 mods ModSet
            // allows, names included, and still refuses anything a peer could use to
            // make us allocate.
            if(n<1 || n>2048) throw new IOException("Message réseau incompatible.");
            var data=ReadControl(n);
            if(n==1 && data[0]==0) continue;
            if(n==9 && data[0]==6) {data[0]=7;Write(data);continue;}
            if(n==9 && data[0]==7) {
                long sent=BitConverter.ToInt64(data,1);
                // TLS probes keep the lobby connection alive. The displayed
                // latency now comes exclusively from the signed UDP echo.
                if(sent!=Interlocked.Read(ref pingStamp))continue;
                continue;
            }
            // 8 is the end-of-session notice. It sits outside the 2..5 block because
            // 6 and 7 are this layer's own ping and pong; a peer built before this
            // existed will reject it and drop the control channel, which at the end
            // of a session leaves it exactly where it was before -- no worse.
            if((data[0]>=2 && data[0]<=5) || data[0]==8) {if(Control==null)throw new IOException("Salon indisponible.");Control(data);continue;}
            throw new IOException("Message réseau incompatible.");
        }
    }
    // The tls_write worker's body, same reasoning as ReceiveLoop above.
    void WriteLoop() {
        while(!closed) {
            byte[] first;
            try {first=toPeer.Take();} catch(InvalidOperationException) {break;}
            using(var batch=new MemoryStream(8192)) {
                batch.Write(first,0,first.Length);
                byte[] next;int count=1;
                while(count<64 && batch.Length<8192 && toPeer.TryTake(out next)) {batch.Write(next,0,next.Length);count++;}
                var bytes=batch.GetBuffer();WriteControl(bytes,(int)batch.Length);
            }
        }
    }
    public Task Run() { return Task.Run(async ()=> {
        heartbeat=new Timer(_=>{if(closed || !ControlConnected)return;try{long stamp=Stopwatch.GetTimestamp();Interlocked.Exchange(ref pingStamp,stamp);Write(new byte[]{6}.Concat(BitConverter.GetBytes(stamp)).ToArray());}catch(ControlChannelException){if(!KeepCommittedGame())Dispose();}},null,250,2000);
        var send=Worker("udp_pump",()=> {
            // One owner for both UDP sockets, including heartbeat sends. No
            // blocking receive, concurrent UdpClient access or unbounded queue.
            udp.Client.Blocking=false;network.Client.Blocking=false;
            long nextHeartbeat=0;
            while(!closed) {
                long now=Stopwatch.GetTimestamp();Interlocked.Exchange(ref lastPumpStamp,now);
                if(now>=nextHeartbeat){udpPingStamp=now;SendGame(Heartbeat(localPlayer,now));nextHeartbeat=now+Stopwatch.Frequency;}
                for(int i=0;i<64 && !closed;i++)try {
                    IPEndPoint from=null;var data=udp.Receive(ref from);
                    if(!IPAddress.IsLoopback(from.Address) || !Packet(data,localPlayer))continue;
                    if(game==null){game=from;udp.Connect(game);}else if(!game.Equals(from))continue;
                    Interlocked.Increment(ref gamePackets);if(data[6]==3)Interlocked.Increment(ref statePackets);SendGame(data);
                }catch(SocketException e){if(!Recoverable(e,"local_receive"))throw;break;}
                for(int i=0;i<64 && !closed;i++)try {
                    IPEndPoint from=null;var data=network.Receive(ref from);ulong sequence;byte[] payload;
                    if(!GameDatagram.Open(datagramKey,remotePlayer,data,out sequence,out payload))continue;
                    if(!learnNetworkPeer && !from.Equals(networkPeer))continue;
                    if(!IsHeartbeat(payload,remotePlayer) && !Packet(payload,remotePlayer))continue;
                    bool newest=replay.IsNewest(remotePlayer,sequence);
                    if(!replay.Accept(remotePlayer,sequence))continue;
                    if(learnNetworkPeer && newest){networkPeer=from;peerEndpoint[remotePlayer]=from;}
                    Interlocked.Exchange(ref lastUdpStamp,Stopwatch.GetTimestamp());
                    if(IsHeartbeat(payload,remotePlayer)){
                        if(payload[6]==1){payload[5]=(byte)localPlayer;payload[6]=2;SendGame(payload);}
                        else{long stamp=BitConverter.ToInt64(payload,8);if(stamp==udpPingStamp){long receivedAt=Stopwatch.GetTimestamp();if(receivedAt>=stamp){Interlocked.Exchange(ref lastUdpPongStamp,receivedAt);Ping?.Invoke((int)Math.Min(15000,(receivedAt-stamp)*1000.0/Stopwatch.Frequency));}}}
                        continue;
                    }
                    Interlocked.Increment(ref peerPackets);
                    if(game!=null)try{udp.Send(payload,payload.Length);Interlocked.Increment(ref deliveredPackets);}
                    catch(SocketException e){if(!Recoverable(e,"local_send"))throw;}
                }catch(SocketException e){if(!Recoverable(e,"udp_receive"))throw;break;}
                Thread.Sleep(1);
                }
        });
        var receive=Worker("tls_read",ReceiveLoop);
        var writer=Worker("tls_write",WriteLoop);
        try {
            var done=await Task.WhenAny(send,receive,writer);await done;
            if(!closed && !ControlConnected)await send;
        } finally {
            Dispose();
            // Observe all workers after closing their blocking sockets.
            try{await Task.WhenAll(send,receive,writer);}catch{}
        }
    }); }

    // A guest link carrying only lobby/control traffic -- every guest beyond
    // the first, once a session has more than two players and all game UDP
    // has moved to one shared MeshRelay instead of a Bridge-per-peer relay.
    // Same TLS read/write/heartbeat as Run(), reused rather than copied; what
    // it never starts is the udp_pump task, so this Bridge's udp/network
    // fields are simply never touched.
    //
    // What it does not carry over: Run()'s "keep the UDP relay alive past a
    // dropped control channel" behaviour (the trailing "await send" and the
    // KeepCommittedGame check in the heartbeat's catch). There is no UDP relay
    // on a control-only link to keep alive, so a lost control channel ends it
    // outright -- CommitGame()/KeepCommittedGame() are not meaningful here and
    // must not be called against a control-only Bridge.
    public Task RunControlOnly() { return Task.Run(async ()=> {
        heartbeat=new Timer(_=>{if(closed || !ControlConnected)return;try{long stamp=Stopwatch.GetTimestamp();Interlocked.Exchange(ref pingStamp,stamp);Write(new byte[]{6}.Concat(BitConverter.GetBytes(stamp)).ToArray());}catch(ControlChannelException){Dispose();}},null,250,2000);
        var receive=Worker("tls_read",ReceiveLoop);
        var writer=Worker("tls_write",WriteLoop);
        try {
            await Task.WhenAny(receive,writer);
        } finally {
            Dispose();
            try{await Task.WhenAll(receive,writer);}catch{}
        }
    }); }
    public void Dispose() {if(Interlocked.Exchange(ref stopping,1)!=0)return;closed=true;if(heartbeat!=null)heartbeat.Dispose();toPeer.CompleteAdding();readTcp.Close();writeTcp.Close();udp.Close();network.Close();readSsl.Dispose();writeSsl.Dispose();}
}

// A mesh needs no relay in the sense Bridge is one -- each peer's game talks
// straight to the others -- but the native engine identifies a registered peer
// purely by which UDP address a packet arrived from (see UdpTransport::
// receiveInput), so three remote seats sharing one loopback port would collide:
// the engine could bind only one of them to that address, and the other two
// would be silently unroutable. Each remote seat therefore gets its own
// loopback port, and the native game is launched with one
// --netplay-peer <seat>:127.0.0.1:<that port> per remote seat.
//
// Those loopback ports do not each need their own Internet-facing socket,
// though. GameDatagram already puts the author's seat inside the HMAC-covered
// bytes (data[5]), so one shared Internet socket can demultiplex incoming
// packets by that authenticated byte and hand each to the loopback leg the
// engine is expecting it on. Four players fully meshed is then four port
// mappings total -- one per player, for their one shared socket -- not twelve.
//
// What this reuses from Bridge rather than reimplementing: the wire-level
// packet/heartbeat framing (Bridge.Packet/Heartbeat/IsHeartbeat) and the
// per-seat ReplayWindow. What it does not do yet: own a TLS control channel.
// The salon stays the star it already is -- text only -- and this is only the
// UDP game mesh a session's Bridge-per-guest-TLS-link will hand endpoints to.
sealed class MeshRelay : IDisposable {
    sealed class Leg {
        public readonly UdpClient Loopback=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
        public IPEndPoint NetworkPeer;
        // Where the local game itself is listening on this leg's port, learned
        // from the sender address of its first outgoing packet -- Bridge does
        // exactly this for its one peer ("game=from"). Without it a leg has
        // nowhere to send an inbound packet back to except the address it just
        // received FROM, which is a different socket than the one the engine
        // is actually reading.
        public IPEndPoint Game;
        public readonly bool LearnPeer;
        public long LastPongStamp;
        public Leg(IPEndPoint peer) {NetworkPeer=peer;LearnPeer=peer==null;}
        public int LoopbackPort {get{return ((IPEndPoint)Loopback.Client.LocalEndPoint).Port;}}
    }

    readonly int localPlayer;readonly byte[] datagramKey;readonly UdpClient network;
    readonly Leg[] legs=new Leg[Lobby.MaxSeats];
    readonly ReplayWindow replay=new ReplayWindow();
    long networkSequence;long lastPumpStamp;long udpDropped;int lastUdpError;volatile bool closed;int stopping;
    public Action<string> Diagnostic;

    // token is the salon's invitation token, shared by the whole session --
    // the same one every Bridge already derives its key from -- so a mesh peer
    // and a star peer speaking to the same session agree on the key without
    // needing a channel of their own to negotiate one.
    public MeshRelay(int localPlayer,byte[] token,UdpClient internetSocket) {
        this.localPlayer=localPlayer;datagramKey=GameDatagram.Key(token);network=internetSocket;
    }

    // Registers a remote seat and returns the loopback port the native engine
    // must be told to reach it on. peerEndpoint null means learn it from the
    // first authenticated packet that seat sends, exactly like Bridge's
    // learnNetworkPeer -- the case a guest is in before it knows a fellow
    // guest's address, only the host having handed out the roster.
    public int AddPeer(int seat,IPEndPoint peerEndpoint) {
        if(seat<0 || seat>=Lobby.MaxSeats || seat==localPlayer)throw new IOException("Siège de pair invalide.");
        if(legs[seat]!=null)throw new IOException("Ce siège a déjà un pair.");
        var leg=new Leg(peerEndpoint);legs[seat]=leg;return leg.LoopbackPort;
    }

    // So a caller wiring this to repeatable events (a lobby endpoint
    // announcement can fire more than once for the same seat -- a NAT remap,
    // or simply hearing about it twice) can check before calling AddPeer a
    // second time, rather than relying on it to throw.
    public bool HasPeer(int seat){return seat>=0 && seat<Lobby.MaxSeats && legs[seat]!=null;}
    // The port LoadGame() has to tell the native engine to reach this seat on
    // (--netplay-peer), looked up after AddPeer already handed it out once --
    // by the time a game actually launches, every peer this attempt needs is
    // already registered, and threading the return value all the way from
    // MeshWiring's event handler to LoadGame() would be a longer path to the
    // same number.
    public int LoopbackPort(int seat) {
        if(seat<0 || seat>=Lobby.MaxSeats || legs[seat]==null)throw new IOException("Ce siège n'a pas de pair de maillage.");
        return legs[seat].LoopbackPort;
    }

    bool Recoverable(SocketException e,string operation) {
        // Same recoverable set as Bridge.Recoverable: UDP send/receive pressure
        // and ICMP unreachables are packet loss the native input history papers
        // over, not a reason to end the mesh.
        if(e.SocketErrorCode==SocketError.WouldBlock || e.SocketErrorCode==SocketError.TimedOut)return true;
        if(e.SocketErrorCode!=SocketError.ConnectionReset && e.SocketErrorCode!=SocketError.ConnectionRefused
            && e.SocketErrorCode!=SocketError.NetworkUnreachable && e.SocketErrorCode!=SocketError.HostUnreachable
            && e.SocketErrorCode!=SocketError.NoBufferSpaceAvailable && e.SocketErrorCode!=SocketError.MessageSize)return false;
        Interlocked.Increment(ref udpDropped);
        if(Interlocked.Exchange(ref lastUdpError,e.NativeErrorCode)!=e.NativeErrorCode)Diagnostic?.Invoke("mesh_event="+operation+" socket_code="+e.NativeErrorCode);
        return true;
    }

    void SendToLeg(Leg leg,byte[] payload) {
        if(leg.NetworkPeer==null)return;
        var packet=GameDatagram.Seal(datagramKey,localPlayer,(ulong)Interlocked.Increment(ref networkSequence),payload);
        try{network.Send(packet,packet.Length,leg.NetworkPeer);}catch(SocketException e){if(!Recoverable(e,"mesh_send"))throw;}
    }

    public bool AllPeersReady(long maxAgeMs) {
        var now=Stopwatch.GetTimestamp();
        foreach(var leg in legs) {
            if(leg==null)continue;
            if(leg.LastPongStamp==0)return false;
            if((now-leg.LastPongStamp)*1000/Stopwatch.Frequency>maxAgeMs)return false;
        }
        return true;
    }

    public Task Run() {
        return Task.Run(()=> {
            try {
                network.Client.Blocking=false;
                foreach(var leg in legs)if(leg!=null)leg.Loopback.Client.Blocking=false;
                long nextHeartbeat=0;
                while(!closed) {
                    long now=Stopwatch.GetTimestamp();Interlocked.Exchange(ref lastPumpStamp,now);
                    if(now>=nextHeartbeat) {
                        var beat=Bridge.Heartbeat(localPlayer,now);
                        foreach(var leg in legs)if(leg!=null)SendToLeg(leg,beat);
                        nextHeartbeat=now+Stopwatch.Frequency;
                    }
                    // Local game -> us -> that seat's remote endpoint. One drain per
                    // leg: each is its own socket, so there is no ordering hazard
                    // between seats, only within one -- the same 64-per-turn bound
                    // Bridge uses to keep a busy peer from starving the others.
                    for(int seat=0;seat<Lobby.MaxSeats && !closed;seat++) {
                        var leg=legs[seat];if(leg==null)continue;
                        for(int i=0;i<64 && !closed;i++)try {
                            IPEndPoint from=null;var data=leg.Loopback.Receive(ref from);
                            if(!IPAddress.IsLoopback(from.Address) || !Bridge.Packet(data,localPlayer))continue;
                            // Learn once, then insist: a second sender appearing on this
                            // leg's port is not the engine we already found, the same
                            // guard Bridge applies to its own single game endpoint.
                            if(leg.Game==null)leg.Game=from;else if(!leg.Game.Equals(from))continue;
                            SendToLeg(leg,data);
                        }catch(SocketException e){if(!Recoverable(e,"mesh_local_receive"))throw;break;}
                    }
                    // Any remote peer -> us -> that seat's own loopback leg. The
                    // seat is read from data[5] before it is trusted: Open still
                    // has to verify the HMAC over exactly those bytes first, so a
                    // forged seat byte fails there, not here.
                    for(int i=0;i<256 && !closed;i++)try {
                        IPEndPoint from=null;var data=network.Receive(ref from);
                        if(data.Length!=GameDatagram.Size)continue;
                        int seat=data[5];if(seat<0 || seat>=Lobby.MaxSeats)continue;
                        var leg=legs[seat];if(leg==null)continue;
                        ulong sequence;byte[] payload;
                        if(!GameDatagram.Open(datagramKey,seat,data,out sequence,out payload))continue;
                        if(!leg.LearnPeer && !from.Equals(leg.NetworkPeer))continue;
                        if(!Bridge.IsHeartbeat(payload,seat) && !Bridge.Packet(payload,seat))continue;
                        bool newest=replay.IsNewest(seat,sequence);
                        if(!replay.Accept(seat,sequence))continue;
                        if(leg.LearnPeer && newest)leg.NetworkPeer=from;
                        if(Bridge.IsHeartbeat(payload,seat)) {
                            if(payload[6]==1){payload[5]=(byte)localPlayer;payload[6]=2;SendToLeg(leg,payload);}
                            else leg.LastPongStamp=Stopwatch.GetTimestamp();
                            continue;
                        }
                        // Nowhere to deliver to until the engine has sent at least once
                        // on this leg -- the same gate Bridge applies ("if(game!=null)")
                        // before handing a decoded packet back to the local process.
                        if(leg.Game==null)continue;
                        try{leg.Loopback.Send(payload,payload.Length,leg.Game);}catch(SocketException e){if(!Recoverable(e,"mesh_local_send"))throw;}
                    }catch(SocketException e){if(!Recoverable(e,"mesh_network_receive"))throw;break;}
                    Thread.Sleep(1);
                }
            } catch(Exception e) {
                if(!closed)Diagnostic?.Invoke("mesh_worker_error="+e.GetType().Name);
                Dispose();
            }
        });
    }

    public void Dispose() {
        if(Interlocked.Exchange(ref stopping,1)!=0)return;closed=true;
        network.Close();
        foreach(var leg in legs)if(leg!=null)leg.Loopback.Close();
    }
}

// The glue between Lobby.EndpointLearned and MeshRelay.AddPeer, pulled out on
// its own because it is the one part of that connection worth testing without
// a router or even a real socket: MeshRelay.AddPeer throws on a seat it
// already has, and a lobby announcement can legitimately fire twice for the
// same seat (the catch-up replay on a late Admit, or a genuine re-announcement
// after a NAT remap) -- if this callback just forwarded blindly, the second
// call would throw from inside a Lobby callback holding Lobby's own lock.
//
// What it does not do: handle a CHANGED address for an already-registered
// seat. A NAT remap mid-session is real and this silently ignores it rather
// than updating the peer, which is a known gap, not an oversight -- MeshRelay
// has no "update this leg's address" operation yet for it to call.
sealed class MeshWiring {
    readonly MeshRelay relay;readonly int localSeat;
    public MeshWiring(MeshRelay relay,int localSeat){this.relay=relay;this.localSeat=localSeat;}
    public void OnEndpointLearned(int seat,IPEndPoint endpoint) {
        if(seat==localSeat || relay.HasPeer(seat))return;
        relay.AddPeer(seat,endpoint);
    }
}
}

