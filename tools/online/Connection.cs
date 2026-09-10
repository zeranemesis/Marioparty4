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
    public static SslStream Client(TcpClient tcp,Invitation invite,byte[] build,byte channel=0) {
        tcp.NoDelay=true; tcp.ReceiveTimeout=8000; tcp.SendTimeout=8000;
        var ssl=new SslStream(tcp.GetStream(),false,(s,c,ch,e)=>c!=null && Equal(Hash(c.GetRawCertData()).Take(invite.Fingerprint.Length).ToArray(),invite.Fingerprint));
        try {
            ssl.ReadTimeout=8000; ssl.WriteTimeout=8000;
            ssl.AuthenticateAsClient("PartyBoard session",null,SslProtocols.Tls12,false);
            byte[] hello=Encoding.ASCII.GetBytes("PBAUTO2\n").Concat(Hash(invite.Token)).Concat(build).Concat(new[]{channel}).ToArray();
            ssl.Write(hello); ssl.Flush();
            byte ack=Read(ssl,1)[0];
            if(ack==2) throw new IOException("Les versions du jeu sont différentes. Copiez le même dossier PartyBoard sur les deux PC.");
            if(ack!=1) throw new IOException("Cette invitation n'est plus valable. Demandez-en une nouvelle.");
            ssl.ReadTimeout=120000; ssl.WriteTimeout=120000; return ssl;
        } catch {ssl.Dispose(); throw;}
    }
    public static SslStream Server(TcpClient tcp,X509Certificate2 cert,Invitation invite,byte[] build,out byte channel) {
        channel=255;
        tcp.NoDelay=true; tcp.ReceiveTimeout=8000; tcp.SendTimeout=8000;
        var ssl=new SslStream(tcp.GetStream(),false);
        try {
            ssl.ReadTimeout=8000; ssl.WriteTimeout=8000;
            ssl.AuthenticateAsServer(cert,false,SslProtocols.Tls12,false);
            var hello=Read(ssl,73);channel=hello[72];
            if(!Equal(hello.Take(8).ToArray(),Encoding.ASCII.GetBytes("PBAUTO2\n")) || channel>1 ||
                !Equal(hello.Skip(8).Take(32).ToArray(),Hash(invite.Token)) || DateTime.UtcNow>invite.Expires)
                throw new AuthenticationException("Invitation refusée.");
            if(!Equal(hello.Skip(40).Take(32).ToArray(),build)) {ssl.WriteByte(2); ssl.Flush(); throw new AuthenticationException("Versions différentes.");}
            ssl.WriteByte(1); ssl.Flush(); ssl.ReadTimeout=120000; ssl.WriteTimeout=120000; return ssl;
        } catch {ssl.Dispose(); throw;}
    }
}

sealed class Invitation {
    public IPAddress Address; public int Port; public DateTime Expires;
    public byte[] Fingerprint,Token,Build;
    public string Encode() {
        if(Fingerprint.Length!=16 || Token.Length!=16)throw new IOException("Invitation incompatible.");
        using(var m=new MemoryStream()) using(var w=new BinaryWriter(m)) {
            w.Write(Address.GetAddressBytes());w.Write((ushort)Port);
            w.Write((uint)(Expires-new DateTime(1970,1,1,0,0,0,DateTimeKind.Utc)).TotalSeconds);
            w.Write(Fingerprint);w.Write(Token);
            return "PB2."+Convert.ToBase64String(m.ToArray()).Replace('+','-').Replace('/','_');
        }
    }
    public static Invitation Decode(string text,bool localTest=false) {
        try {
            if(text==null) throw new FormatException(); text=text.Trim();
            if(text.Length!=60 || !text.StartsWith("PB2.",StringComparison.Ordinal)) throw new FormatException();
            var b64=text.Substring(4).Replace('-','+').Replace('_','/'); b64+=new string('=',(4-b64.Length%4)%4);
            var data=Convert.FromBase64String(b64); if(data.Length!=42) throw new FormatException();
            using(var r=new BinaryReader(new MemoryStream(data))) {
                var i=new Invitation{Address=new IPAddress(r.ReadBytes(4)),Port=r.ReadUInt16(),Expires=new DateTime(1970,1,1,0,0,0,DateTimeKind.Utc).AddSeconds(r.ReadUInt32()),Fingerprint=r.ReadBytes(16),Token=r.ReadBytes(16)};
                if(i.Port==0 || (!localTest && !Gateway.Public(i.Address))) throw new FormatException();
                if(i.Expires<DateTime.UtcNow) throw new IOException("Cette invitation a expiré. L'hôte doit recréer une partie.");
                if(i.Expires>DateTime.UtcNow.AddMinutes(31)) throw new FormatException();
                return i;
            }
        } catch(IOException) {throw;} catch {throw new IOException("L'invitation est incomplète. Copiez-la entièrement depuis le PC de votre ami.");}
    }
}

static class GameDatagram {
    public const int Payload=88,Size=14+Payload+16;
    const int Header=14,Tag=16;
    public static byte[] Key(byte[] token) {return Wire.Hash(token.Concat(Encoding.ASCII.GetBytes("PartyBoard UDP v1")).ToArray());}
    public static byte[] Seal(byte[] key,int player,ulong sequence,byte[] payload) {
        if(key==null || key.Length!=32 || payload==null || payload.Length!=Payload || player<0 || player>1)throw new IOException("Paquet de jeu invalide.");
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

// TLS carries lobby/control messages. Time-sensitive game inputs use their own
// authenticated UDP path so one lost Internet packet cannot block later input.
sealed class ControlChannelException : IOException {
    public ControlChannelException(Exception error):base("La connexion du salon a ete interrompue.",error){}
}
sealed class Bridge : IDisposable {
    readonly TcpClient readTcp,writeTcp; readonly SslStream readSsl,writeSsl; readonly UdpClient udp,network;
    readonly int localPlayer;
    IPEndPoint game,networkPeer; readonly byte[] datagramKey; readonly bool learnNetworkPeer; volatile bool closed; int stopping; Timer heartbeat;
    long networkSequence;ulong highestPeerSequence,peerWindow;bool hasPeerSequence;
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
    public Bridge(TcpClient reader,SslStream readStream,TcpClient writer,SslStream writeStream,int player,UdpClient internet,IPEndPoint internetPeer,byte[] token,int hostGamePort=0) {
        readTcp=reader;readSsl=readStream;writeTcp=writer;writeSsl=writeStream;localPlayer=player;network=internet;networkPeer=internetPeer;learnNetworkPeer=internetPeer==null;datagramKey=GameDatagram.Key(token);udp=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
        if(hostGamePort!=0) {game=new IPEndPoint(IPAddress.Loopback,hostGamePort); udp.Connect(game);}
    }
    internal static bool Packet(byte[] b,int player) {return b.Length==GameDatagram.Payload && b[0]==80 && b[1]==66 && b[2]==82 && b[3]==66 && b[4]==0 && b[5]==6 && b[6]>=1 && b[6]<=3 && b[7]==player;}
    void Write(byte[] payload) {
        if(payload.Length<1 || payload.Length>256)throw new IOException("Message de salon trop long.");
        var frame=new byte[payload.Length+2];frame[0]=(byte)(payload.Length>>8);frame[1]=(byte)payload.Length;
        Buffer.BlockCopy(payload,0,frame,2,payload.Length);
        if(closed || !ControlConnected)throw new ControlChannelException(new IOException("Control channel closed."));
        try{if(toPeer.IsAddingCompleted || !toPeer.TryAdd(frame,1000))throw new ControlChannelException(new IOException("Control queue unavailable."));}
        catch(InvalidOperationException e){throw new ControlChannelException(e);}
    }
    public void SendControl(byte[] payload) {Write(payload);}
    bool Fresh(ulong sequence) {
        if(!hasPeerSequence){hasPeerSequence=true;highestPeerSequence=sequence;peerWindow=1;return true;}
        if(sequence>highestPeerSequence){ulong d=sequence-highestPeerSequence;peerWindow=d>=64?1:(peerWindow<<(int)d)|1;highestPeerSequence=sequence;return true;}
        ulong behind=highestPeerSequence-sequence;if(behind>=64)return false;ulong bit=1UL<<(int)behind;if((peerWindow&bit)!=0)return false;peerWindow|=bit;return true;
    }
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
    static byte[] Heartbeat(int player,long stamp) {var b=new byte[GameDatagram.Payload];b[0]=(byte)'P';b[1]=(byte)'B';b[2]=(byte)'H';b[3]=(byte)'B';b[4]=1;b[5]=(byte)player;b[6]=1;Buffer.BlockCopy(BitConverter.GetBytes(stamp),0,b,8,8);return b;}
    static bool IsHeartbeat(byte[] b,int player) {return b.Length==GameDatagram.Payload && b[0]=='P' && b[1]=='B' && b[2]=='H' && b[3]=='B' && b[4]==1 && b[5]==player && (b[6]==1 || b[6]==2);}
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
                    if(!GameDatagram.Open(datagramKey,localPlayer^1,data,out sequence,out payload))continue;
                    if(!learnNetworkPeer && !from.Equals(networkPeer))continue;
                    if(!IsHeartbeat(payload,localPlayer^1) && !Packet(payload,localPlayer^1))continue;
                    bool newest=!hasPeerSequence || sequence>highestPeerSequence;
                    if(!Fresh(sequence))continue;
                    if(learnNetworkPeer && newest)networkPeer=from;
                    Interlocked.Exchange(ref lastUdpStamp,Stopwatch.GetTimestamp());
                    if(IsHeartbeat(payload,localPlayer^1)){
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
        var receive=Worker("tls_read",()=> {
            while(!closed && ControlConnected) {
                var size=ReadControl(2); int n=(size[0]<<8)|size[1];
                if(n<1 || n>256) throw new IOException("Message réseau incompatible.");
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
                if(data[0]>=2 && data[0]<=5) {if(Control==null)throw new IOException("Salon indisponible.");Control(data);continue;}
                throw new IOException("Message réseau incompatible.");
            }
        });
        var writer=Worker("tls_write",()=> {
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
        });
        try {
            var done=await Task.WhenAny(send,receive,writer);await done;
            if(!closed && !ControlConnected)await send;
        } finally {
            Dispose();
            // Observe all workers after closing their blocking sockets.
            try{await Task.WhenAll(send,receive,writer);}catch{}
        }
    }); }
    public void Dispose() {if(Interlocked.Exchange(ref stopping,1)!=0)return;closed=true;if(heartbeat!=null)heartbeat.Dispose();toPeer.CompleteAdding();readTcp.Close();writeTcp.Close();udp.Close();network.Close();readSsl.Dispose();writeSsl.Dispose();}
}
}

