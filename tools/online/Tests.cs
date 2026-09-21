using System;
using System.IO;
using System.Text;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Net.Security;
using System.Security.Cryptography.X509Certificates;
using System.Diagnostics;
using System.Threading.Tasks;
using System.Threading;
using System.Collections.Generic;

namespace PartyBoardOnline {
static class Tests {
    // Real sockets, no router configuration: impair only the authenticated game
    // channel while the two TLS lobby connections remain healthy.
    sealed class ImpairedUdp : IDisposable {
        readonly UdpClient socket=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
        readonly IPEndPoint host,guest;readonly Thread thread;volatile bool stopped;
        public Action DuringOutage,ResetControl;public int Dropped,Reordered;public bool Outage,ResetInjected;
        public Exception Error;
        public IPEndPoint Endpoint {get{return (IPEndPoint)socket.Client.LocalEndPoint;}}
        public ImpairedUdp(IPEndPoint h,IPEndPoint g){host=h;guest=g;socket.Client.Blocking=false;thread=new Thread(Run){IsBackground=true};thread.Start();}
        void Run(){try{
            var time=Stopwatch.StartNew();long outageStart=-1,heldAt=0;byte[] held=null;IPEndPoint heldTarget=null;int count=0;bool checkedOutage=false;
            while(!stopped){
                if(outageStart>=0 && !checkedOutage && time.ElapsedMilliseconds-outageStart>5500){DuringOutage?.Invoke();checkedOutage=true;}
                if(held!=null && time.ElapsedMilliseconds-heldAt>30){socket.Send(held,held.Length,heldTarget);held=null;}
                for(int batch=0;batch<64 && !stopped;batch++)try{
                    IPEndPoint from=null;var data=socket.Receive(ref from);IPEndPoint target=from.Equals(host)?guest:from.Equals(guest)?host:null;if(target==null)continue;
                    bool input=data.Length==GameDatagram.Size && data[16]=='R' && data[17]=='B';
                    if(input && !ResetInjected && Gateway.U32(data,30)>=1200 && ResetControl!=null){ResetInjected=true;ResetControl();}
                    if(input && outageStart<0 && Gateway.U32(data,30)>=2364){outageStart=time.ElapsedMilliseconds;Outage=true;}
                    if(outageStart>=0 && time.ElapsedMilliseconds-outageStart<6500){Dropped++;continue;}
                    if(input && ++count%113==0){Dropped++;continue;}
                    if(input && count%97==0 && held==null){held=data;heldTarget=target;heldAt=time.ElapsedMilliseconds;continue;}
                    socket.Send(data,data.Length,target);
                    if(held!=null){socket.Send(held,held.Length,heldTarget);held=null;Reordered++;}
                    if(input && count%127==0)socket.Send(data,data.Length,target);
                }catch(SocketException e){if(e.SocketErrorCode!=SocketError.WouldBlock && !stopped)throw;break;}
                Thread.Sleep(1);
            }
        }catch(Exception e){if(!stopped)Error=e;}}
        public void Dispose(){stopped=true;thread.Join(2000);socket.Close();}
    }
    static int checks;
    static void Check(bool value,string name) {checks++;if(!value) throw new Exception(name);}
    static void Reject(Action action,string name) {bool rejected=false;try{action();}catch{rejected=true;}Check(rejected,name);}
    static void Codecs() {
        var invite=new Invitation{Address=IPAddress.Parse("8.8.8.8"),Port=32000,Expires=DateTime.UtcNow.AddMinutes(30),Fingerprint=Wire.Random(16),Token=Wire.Random(16),Build=Wire.Random(32)};
        Check(invite.Encode().Length==72,"compact invitation length");var decoded=Invitation.Decode(invite.Encode());Check(decoded.Port==32000 && Wire.Equal(invite.Token,decoded.Token) && Wire.Equal(invite.Fingerprint,decoded.Fingerprint),"invitation round trip");
        Check(decoded.MaxPlayers==2,"default player count round trips as two");
        invite.MaxPlayers=4;Check(Invitation.Decode(invite.Encode()).MaxPlayers==4,"a chosen player count survives the round trip");
        invite.MaxPlayers=1;Reject(()=>invite.Encode(),"a salon of one cannot be encoded");
        invite.MaxPlayers=5;Reject(()=>invite.Encode(),"a fifth seat cannot be encoded either");
        invite.MaxPlayers=2;
        Reject(()=>Invitation.Decode("PB3."+new string('A',64)),"the previous format (no player count) is refused by name, not decoded short");
        Reject(()=>Invitation.Decode("bad"),"malformed invite");Reject(()=>Invitation.Decode(new string('x',1000)),"bounded invite");
        // The local path. Without it two PCs in one house are sent out through
        // the box and back: 15 ms between machines that are 0 ms apart, and a
        // control channel the router cut after forty to fifty seconds.
        Check(!decoded.HasLocalPath,"invitation without a local address offers none");
        invite.LocalAddress=IPAddress.Parse("192.168.1.14");invite.LocalPort=32100;
        decoded=Invitation.Decode(invite.Encode());
        Check(decoded.HasLocalPath && decoded.LocalAddress.Equals(IPAddress.Parse("192.168.1.14")) && decoded.LocalPort==32100,"private local address survives the round trip");
        invite.LocalAddress=IPAddress.Parse("9.9.9.9");
        Check(!Invitation.Decode(invite.Encode()).HasLocalPath,"public local address is discarded, not dialled");
        invite.LocalAddress=IPAddress.Loopback;
        Check(!Invitation.Decode(invite.Encode()).HasLocalPath,"loopback local address is discarded");
        invite.LocalAddress=IPAddress.Any;invite.LocalPort=0;
        Reject(()=>Invitation.Decode("PB2."+new string('A',56)),"previous invitation format refused by name");
        invite.Address=IPAddress.Loopback;Reject(()=>Invitation.Decode(invite.Encode()),"loopback invite rejected");
        invite.Address=IPAddress.Parse("8.8.8.8");invite.Expires=DateTime.UtcNow.AddSeconds(-1);Reject(()=>Invitation.Decode(invite.Encode()),"expired invite");
        foreach(string addr in new[]{"0.1.2.3","10.0.0.1","127.0.0.1","100.64.0.1","192.168.1.1","169.254.1.1","198.18.0.1","203.0.113.1","224.0.0.1","::1"}) Check(!Gateway.Public(IPAddress.Parse(addr)),"public address filter");
        var router=IPAddress.Parse("192.168.1.1");
        Check(Gateway.SafeUrl(new Uri("http://192.168.1.1:5000/control"),router),"local gateway URL");
        foreach(string url in new[]{"http://example.org/x","http://127.0.0.1/x","file:///C:/test","http://user@192.168.1.1/x","http://192.168.1.1/x#fragment"}) Check(!Gateway.SafeUrl(new Uri(url),router),"untrusted URL rejected");
        byte[] key=Wire.Random(12);var request=Gateway.PcpRequest(IPAddress.Parse("192.168.1.2"),key,32000,32000,120);
        Check(request.Length==60 && request[36]==6 && request[18]==255 && Gateway.U32(request,4)==120,"PCP request");
        var reply=(byte[])request.Clone();reply[1]=129;Array.Clear(reply,44,16);reply[54]=reply[55]=255;reply[56]=8;reply[57]=8;reply[58]=8;reply[59]=8;
        int port;IPAddress ip;uint life;
        Check(Gateway.PcpReply(reply,key,32000,out port,out ip,out life) && port==32000 && life==120,"PCP response");
        Check(!Gateway.PcpReply(reply,Wire.Random(12),32000,out port,out ip,out life),"PCP nonce mismatch");
        Check(!Gateway.PcpReply(reply,key,32001,out port,out ip,out life),"PCP internal port mismatch");
        Check(!Gateway.PcpReply(new byte[4],key,32000,out port,out ip,out life),"PCP truncation");
        request=Gateway.PmpRequest(32000,32000,0);Check(request[1]==2 && Gateway.U32(request,8)==0,"NAT-PMP deletion");
        reply=new byte[16];reply[1]=130;Gateway.Put16(reply,8,32000);Gateway.Put16(reply,10,32100);Gateway.Put32(reply,12,120);
        Check(Gateway.PmpReply(reply,32000,out port,out life) && port==32100 && life==120,"NAT-PMP response");
        reply[3]=2;Check(!Gateway.PmpReply(reply,32000,out port,out life),"NAT-PMP refusal");
        // Shaped from the engine's own constants, never from a literal. The
        // literal version of these four lines said 88 bytes and v6 for two days
        // after the engine moved to 152 and v7, and the bridge silently dropped
        // every game packet between the two PCs.
        var payload=new byte[GameDatagram.Payload];payload[0]=80;payload[1]=66;payload[2]=82;payload[3]=66;payload[4]=WireFormat.VersionHigh;payload[5]=WireFormat.VersionLow;payload[6]=1;payload[7]=0;
        Check(Bridge.Packet(payload,0), "current native protocol accepted by bridge");
        foreach(byte type in new byte[]{2,3}) { payload[6]=type; Check(Bridge.Packet(payload,0),"explicit repair/state packet accepted"); }
        payload[6]=4;Check(!Bridge.Packet(payload,0),"unknown packet type rejected");
        payload[6]=1;payload[5]=(byte)(WireFormat.VersionLow-1);Check(!Bridge.Packet(payload,0),"old native protocol rejected");payload[5]=WireFormat.VersionLow;
        var truncated=new byte[GameDatagram.Payload-1];Buffer.BlockCopy(payload,0,truncated,0,truncated.Length);
        Check(!Bridge.Packet(truncated,0),"packet of the wrong length rejected");
        var datagramKey=GameDatagram.Key(invite.Token);var datagram=GameDatagram.Seal(datagramKey,0,42,payload);ulong sequence;byte[] opened;
        Check(GameDatagram.Open(datagramKey,0,datagram,out sequence,out opened) && sequence==42 && Wire.Equal(payload,opened),"authenticated UDP game packet round trip");
        datagram[20]^=1;Check(!GameDatagram.Open(datagramKey,0,datagram,out sequence,out opened),"tampered UDP game packet rejected");
        Check(!GameDatagram.Open(Wire.Random(32),0,GameDatagram.Seal(datagramKey,0,43,payload),out sequence,out opened),"foreign UDP game packet rejected");
        // Every seat can seal and open, and a datagram authored by one seat must not
        // verify as another's: the seat is inside the signed bytes, not beside them.
        for(int seat=0;seat<Lobby.MaxSeats;seat++) {
            var sealed4=GameDatagram.Seal(datagramKey,seat,77,new byte[GameDatagram.Payload]);
            ulong seq4;byte[] out4;
            Check(GameDatagram.Open(datagramKey,seat,sealed4,out seq4,out out4) && seq4==77,"seat "+seat+" seals and opens");
            for(int other=0;other<Lobby.MaxSeats;other++)
                if(other!=seat) Check(!GameDatagram.Open(datagramKey,other,sealed4,out seq4,out out4),"a datagram from seat "+seat+" is not seat "+other+"'s");
        }
        Reject(()=>GameDatagram.Seal(datagramKey,Lobby.MaxSeats,1,new byte[GameDatagram.Payload]),"a fifth seat cannot seal");
        // One window per sender. A shared one would let a high sequence from one
        // peer make a fresh packet from another look like a replay -- silent packet
        // loss on three of four links, which is the kind of fault that reads as lag.
        var replay=new ReplayWindow();
        Check(replay.Accept(1,1000),"first packet from a seat is fresh");
        Check(!replay.Accept(1,1000),"the same packet twice is a replay");
        Check(replay.Accept(2,5),"a low sequence from another seat is still fresh");
        Check(replay.Accept(3,1),"and so is seat 3 starting at one");
        Check(replay.Accept(1,999),"a packet just behind is accepted once");
        Check(!replay.Accept(1,999),"but not twice");
        Check(!replay.Accept(1,900),"and not far behind the window");
        Check(replay.Accept(2,6) && replay.Accept(3,2),"the other seats are untouched by any of it");
        Check(!replay.Accept(Lobby.MaxSeats,1),"a seat outside the table is refused");
        var udpPcp=Gateway.PcpRequest(IPAddress.Parse("192.168.1.2"),key,32000,32000,120,17);Check(udpPcp[36]==17,"PCP UDP mapping request");
        var udpPmp=Gateway.PmpRequest(32000,32000,120,1);Check(udpPmp[1]==1,"NAT-PMP UDP mapping request");
    }
    // A control-only Bridge on real TLS sockets, paired against an ordinary
    // Bridge -- the same two-channel construction Tls()'s bridgeTest uses, but
    // without the game-launch machinery, since nothing here is about a game.
    // What is under test: lobby messages flow through the control-only side,
    // and it never once touches UDP, even though the other side keeps sending
    // heartbeats onto sockets nobody is reading.
    static void ControlOnlyLink() {
        using(var cert=Wire.Certificate()) {
            var listener=new TcpListener(IPAddress.Loopback,0);listener.Start();
            var invite=new Invitation{Address=IPAddress.Loopback,Port=((IPEndPoint)listener.LocalEndpoint).Port,Expires=DateTime.UtcNow.AddMinutes(1),Fingerprint=Wire.Hash(cert.RawData).Take(16).ToArray(),Token=Wire.Random(16),Build=Wire.Random(32)};
            var other=Invitation.Decode(invite.Encode(),true);
            TcpClient host=null;SslStream server=null;
            var accept=Task.Run(()=>{host=listener.AcceptTcpClient();byte channel;server=Wire.Server(host,cert,invite,invite.Build,out channel);return channel==0;});
            using(var client=new TcpClient()) {
                client.Connect(IPAddress.Loopback,invite.Port);var ssl=Wire.Client(client,other,invite.Build);
                Check(accept.Wait(5000) && accept.Result,"first channel of the control-only pair");
                TcpClient host2=null;SslStream server2=null;
                var accept2=Task.Run(()=>{host2=listener.AcceptTcpClient();byte channel;server2=Wire.Server(host2,cert,invite,invite.Build,out channel);return channel==1;});
                using(var client2=new TcpClient()) {
                    client2.Connect(IPAddress.Loopback,invite.Port);var ssl2=Wire.Client(client2,other,invite.Build,1);
                    Check(accept2.Wait(5000) && accept2.Result,"second channel of the control-only pair");
                    var hostNet=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
                    var guestNet=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
                    using(var a=new Bridge(host,server,host2,server2,0,hostNet,(IPEndPoint)guestNet.Client.LocalEndPoint,invite.Token))
                    using(var b=new Bridge(client2,ssl2,client,ssl,1,guestNet,(IPEndPoint)hostNet.Client.LocalEndPoint,invite.Token)) {
                        var seen=new List<byte[]>();
                        var profileHost=new PlayerInfo("Camille",Wire.Hash(new byte[]{1,2,3}),3);
                        var profileGuest=new PlayerInfo("Alex",Wire.Hash(new byte[]{1,2,3}),3);
                        var hl=new Lobby(true,profileHost,(seat,payload)=>a.SendControl(payload),id=>{},id=>{},()=>{});
                        var cl=new Lobby(false,profileGuest,(seat,payload)=>b.SendControl(payload),id=>{},id=>{},()=>{});
                        a.Control=hl.Receive;b.Control=cl.Receive;
                        var at=a.Run();
                        // The one line this test exists to exercise: the guest side never
                        // starts a UDP pump.
                        var bt=b.RunControlOnly();
                        hl.Announce();cl.Announce();
                        Check(SpinWait.SpinUntil(()=>hl.Remote!=null && cl.Remote!=null,5000),
                            "lobby profiles cross a control-only link exactly as a full one");
                        Check(hl.Remote.Name=="Alex" && cl.Remote.Name=="Camille","and carry the right names in each direction");

                        // Give the host's own (ordinary) heartbeat pump several real
                        // ticks to run -- it sends UDP whether the other side is
                        // listening or not. If RunControlOnly ever started reading its
                        // own network/udp sockets, GamePackets/PeerPackets would move.
                        Thread.Sleep(500);
                        Check(!b.UdpReady,"the control-only side never becomes UDP-ready");
                        Check(b.GamePackets==0 && b.PeerPackets==0,"and never counts a single UDP packet, sent or received");
                        Check(b.TransportStatus.Contains("udp_age_ms=-1"),"its own UDP age stays unset, not merely small");

                        client.Close();client2.Close();
                        Check(SpinWait.SpinUntil(()=>bt.IsCompleted,5000),"closing the TLS pair ends a control-only link on its own");
                        host.Close();host2.Close();
                        try{at.Wait(2000);}catch{}
                    }
                }
            }
        }
    }

    static void Tls(int mode,bool bridgeTest=false,bool resetBeforeCommit=false) {
        using(var cert=Wire.Certificate()) {
            var listener=new TcpListener(IPAddress.Loopback,0);listener.Start();
            var invite=new Invitation{Address=IPAddress.Loopback,Port=((IPEndPoint)listener.LocalEndpoint).Port,Expires=DateTime.UtcNow.AddMinutes(1),Fingerprint=Wire.Hash(cert.RawData).Take(16).ToArray(),Token=Wire.Random(16),Build=Wire.Random(32)};
            TcpClient host=null;SslStream server=null;
            var accept=Task.Run(()=>{host=listener.AcceptTcpClient();try {byte channel;server=Wire.Server(host,cert,invite,invite.Build,out channel);return channel==0;}catch{return false;}});
            var other=Invitation.Decode(invite.Encode(),true);
            if(mode==1) other.Fingerprint=Wire.Random(16);
            if(mode==2) other.Token=Wire.Random(16);
            var build=mode==3 ? Wire.Random(32):invite.Build;
            using(var client=new TcpClient()) {
                client.Connect(IPAddress.Loopback,invite.Port);SslStream ssl=null;bool ok=false;
                try{ssl=Wire.Client(client,other,build);ok=true;}catch{}finally{if(!ok) client.Close();}
                Check(accept.Wait(12000),"TLS handshake bounded");Check(ok==(mode==0) && accept.Result==(mode==0),"TLS mode "+mode);
                if(mode==0 && bridgeTest) {
                    TcpClient host2=null;SslStream server2=null;
                    var accept2=Task.Run(()=>{host2=listener.AcceptTcpClient();byte channel;server2=Wire.Server(host2,cert,invite,invite.Build,out channel);return channel==1;});
                    var client2=new TcpClient();client2.Connect(IPAddress.Loopback,invite.Port);var ssl2=Wire.Client(client2,other,build,1);
                    Check(accept2.Wait(12000) && accept2.Result,"second directional TLS channel");
                    int gamePort;using(var reserve=new UdpClient(new IPEndPoint(IPAddress.Loopback,0))) gamePort=((IPEndPoint)reserve.Client.LocalEndPoint).Port;
                    var hostNet=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));var guestNet=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
                    var hostNetEndpoint=(IPEndPoint)hostNet.Client.LocalEndPoint;
                    using(var impairment=new ImpairedUdp(hostNetEndpoint,(IPEndPoint)guestNet.Client.LocalEndPoint))
                    using(var a=new Bridge(host,server,host2,server2,0,hostNet,null,invite.Token,gamePort)) using(var b=new Bridge(client2,ssl2,client,ssl,1,guestNet,impairment.Endpoint,invite.Token)) {
                        bool staleDetected=false;impairment.DuringOutage=()=>staleDetected=!a.UdpReady && !b.UdpReady;
                        var diagHost=new Report(Path.GetTempPath());var diagGuest=new Report(Path.GetTempPath());GameStart hg=null,cg=null;Guid hi=Guid.Empty,ci=Guid.Empty;int ping=-1,pingCount=0,pingsAtReset=0;
                        a.Diagnostic=diagHost.Write;b.Diagnostic=diagGuest.Write;
                        Action resetTls=()=>{pingsAtReset=Volatile.Read(ref pingCount);client.Client.LingerState=new LingerOption(true,0);client.Close();client2.Client.LingerState=new LingerOption(true,0);client2.Close();};
                        impairment.ResetControl=resetTls;
                        using(var hr=new ManualResetEventSlim())using(var cr=new ManualResetEventSlim()) {
                        var profile=new PlayerInfo("Test",Wire.Hash(new byte[]{1,2,3}),3);
                        var hl=new Lobby(true,profile,(seat,payload)=>a.SendControl(payload),id=>{hi=id;hg=new GameStart(id);hg.Launch(GameStart.OnlineArguments("--netplay-host "+gamePort)+" --netplay-probe-realtime","barrier-test-only",true,diagHost.NativePath);hr.Set();},id=>{a.CommitGame();hg.Commit(id);},()=>{});
                        var cl=new Lobby(false,profile,(seat,payload)=>b.SendControl(payload),id=>{ci=id;cg=new GameStart(id);cg.Launch(GameStart.OnlineArguments("--netplay-join 127.0.0.1:"+b.LocalPort)+" --netplay-probe-realtime","barrier-test-only",true,diagGuest.NativePath);cr.Set();},id=>{b.CommitGame();cg.Commit(id);},()=>{});
                        a.Control=hl.Receive;b.Control=cl.Receive;a.Ping=value=>{Interlocked.Exchange(ref ping,value);Interlocked.Increment(ref pingCount);};
                        var at=a.Run();var bt=b.Run();hl.Announce();cl.Announce();
                        Check(SpinWait.SpinUntil(()=>hl.CanStart,5000),"encrypted lobby metadata exchange");
                        Check(SpinWait.SpinUntil(()=>a.UdpReady && b.UdpReady,5000),"authenticated UDP path ready before launch");
                        if(resetBeforeCommit){
                            resetTls();Check(SpinWait.SpinUntil(()=>at.IsCompleted && bt.IsCompleted,10000),"TLS reset before host commit still terminates both lobbies");
                            Check(a.GamePackets==0 && b.GamePackets==0 && hg==null && cg==null,"TLS loss cannot start uncommitted games");
                            Reject(()=>a.CommitGame(),"closed lobby cannot authorize game continuation");
                        }else{
                        Reject(()=>cl.Start(),"guest cannot launch encrypted session");
                        hl.Start();Check(hr.Wait(10000) && cr.Wait(10000),"host starts both processes");
                        hg.WaitReady(CancellationToken.None);cg.WaitReady(CancellationToken.None);
                        Check(a.GamePackets==0 && b.GamePackets==0,"no simulation before host commit");
                        hl.Loaded(hi);Thread.Sleep(150);
                        Check(a.GamePackets==0 && b.GamePackets==0,"host waits for guest loading readiness");
                        Check(SpinWait.SpinUntil(()=>Interlocked.CompareExchange(ref ping,0,0)>=0,5000),"actual authenticated UDP round-trip measured");
                        cl.Loaded(ci);
                        var h=hg.Process;var c=cg.Process;
                        {
                            var ho=h.StandardOutput.ReadToEndAsync();var he=h.StandardError.ReadToEndAsync();var co=c.StandardOutput.ReadToEndAsync();var ce=c.StandardError.ReadToEndAsync();
                            try {
                                Check(h.WaitForExit(150000) && c.WaitForExit(150000),"game probes timeout");
                                if(h.ExitCode!=0 || c.ExitCode!=0) {Console.Error.WriteLine("HOST "+h.ExitCode+"\n"+ho.Result+he.Result);Console.Error.WriteLine("CLIENT "+c.ExitCode+"\n"+co.Result+ce.Result);}
                                Check(h.ExitCode==0 && c.ExitCode==0,"encrypted game probes exit");
                                Check(ho.Result.Contains("PASS: 2400/2400") && co.Result.Contains("PASS: 2400/2400"),"4800 native input ticks through authenticated UDP with 12-second pause and dropped input repair");Check(diagHost.Read().Contains("event=checkpoint") && diagGuest.Read().Contains("state_hash="),"canonical state diagnostics on both peers");Check(ho.Result.Contains("equal_states=2400") && co.Result.Contains("equal_states=2400"),"all committed gameplay state hashes compared after loss and TLS reset");Check(System.Text.RegularExpressions.Regex.IsMatch(diagHost.Read(),@"repaired=[1-9][0-9]*"),"report identifies retained-input repair");Check(a.GamePackets-a.StatePackets<8000 && b.GamePackets-b.StatePackets<8000,"bounded redundant input and repair traffic");Check(a.StatePackets<4000 && b.StatePackets<4000,"bounded canonical hash stream and repairs");
                                Check(impairment.Error==null && impairment.Outage && impairment.Dropped>20 && impairment.Reordered>10,"packet loss, duplication, reordering and outage after frame 2364 exercised");
                                Check(staleDetected,"UDP readiness expires during outage after TLS is lost");
                                Check(impairment.ResetInjected && !a.ControlConnected && !b.ControlConnected,"both TLS connections deliberately reset during committed game");
                                Check(!at.IsCompleted && !bt.IsCompleted && Volatile.Read(ref pingCount)>pingsAtReset+5,"UDP game and fresh ping continue after TLS reset");
                                Check(diagHost.Read().Contains("event=control_lost") && diagGuest.Read().Contains("event=control_lost"),"TLS loss diagnosed separately on both peers");
                                Check(diagHost.Read().Contains("rollback_active=0") && diagGuest.Read().Contains("rollback_active=0")
                                    && !diagHost.Read().Contains("rollback_active=1") && !diagGuest.Read().Contains("rollback_active=1"),"production launch remains in lockstep throughout impaired transport test");
                                Console.WriteLine("Impaired UDP: dropped="+impairment.Dropped+", reordered="+impairment.Reordered+", 6.5-second outage recovered at frame 2364.");
                                Console.WriteLine("TLS reset at frame 1200: both games completed frame 2400 and UDP ping continued.");
                            }finally {if(!h.HasExited)h.Kill();if(!c.HasExited)c.Kill();hg.Dispose();cg.Dispose();h.Dispose();c.Dispose();}
                        }
                        }
                        a.Dispose();b.Dispose();try{Task.WaitAll(new[]{at,bt},5000);}catch{}
                        }
                    }
                }
                if(ssl!=null) ssl.Dispose();
            }
            listener.Stop();if(host!=null)host.Close();if(server!=null)server.Dispose();
        }
    }
    static void Leases() {
        var route=new Route{Local=IPAddress.Parse("192.168.1.2"),Router=IPAddress.Parse("192.168.1.1")};
        int creates=0,deletes=0;byte[] initialNonce=null;
        using(var gateway=new Gateway(route,32100,q=>{
            if(Gateway.U32(q,4)==0) deletes++;else creates++;
            var nonce=q.Skip(24).Take(12).ToArray();if(initialNonce==null)initialNonce=nonce;else Check(Wire.Equal(nonce,initialNonce),"lease renewal/deletion nonce stable");
            var r=(byte[])q.Clone();r[1]=129;r[54]=r[55]=255;r[56]=8;r[57]=8;r[58]=8;r[59]=8;return r;
        })) {
            gateway.Open();Check(gateway.Port==32100 && gateway.Lifetime==120,"finite lease creation");
            gateway.Renew();Check(creates==2,"lease renewal");
        }
        Check(deletes==1,"lease deletion on dispose");
        int cleanup=0;
        using(var gateway=new Gateway(route,32100,q=>{
            var r=(byte[])q.Clone();r[1]=129;r[54]=r[55]=255;r[56]=8;r[57]=8;r[58]=8;r[59]=8;
            if(Gateway.U32(q,4)==0) cleanup++;else Gateway.Put32(r,4,86400);
            return r;
        })) Reject(()=>gateway.Pcp(120),"excessive lease refused");
        Check(cleanup==1,"excessive lease cleaned up");
    }
    static Process Probe(string root,string args) {return Process.Start(new ProcessStartInfo(Path.Combine(root,"partyboard.exe"),GameStart.OnlineArguments(args)+" --netplay-pad-probe"){WorkingDirectory=root,UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true});}
    static ModEntry Mod(int id,string name,byte seed) {
        var fingerprint=new byte[8];for(int i=0;i<8;i++)fingerprint[i]=(byte)(seed+i);
        return new ModEntry(id,name,@"C:\mods\"+id,fingerprint);
    }

    // A mod changes the bytes the game reads, so in lockstep it has to be agreed the
    // same way the disc is. These checks pin the three ways two players can differ -
    // a missing mod, a different build of the same mod, and the same mods loaded in
    // the other order - because only the first of the three is obvious to a player.
    static byte[] FakeGamePacket(int player,byte type=1) {
        var b=new byte[GameDatagram.Payload];
        b[0]=80;b[1]=66;b[2]=82;b[3]=66;b[4]=WireFormat.VersionHigh;b[5]=WireFormat.VersionLow;b[6]=type;b[7]=(byte)player;
        return b;
    }
    static bool TryReceive(UdpClient socket,int timeoutMs,out byte[] data,out IPEndPoint from) {
        socket.Client.ReceiveTimeout=timeoutMs;from=null;
        try{data=socket.Receive(ref from);return true;}catch(SocketException){data=null;return false;}
    }
    // The pump sends a heartbeat to every leg on its very first iteration
    // (nextHeartbeat starts at 0), so a socket standing in for a remote peer
    // sees that keepalive before it sees whatever the test actually sent.
    // Skipping heartbeats here is not a workaround for a race: real peers do
    // exactly this too, which is the whole reason IsHeartbeat exists.
    static bool TryReceiveGamePacket(byte[] key,int expectedSeat,UdpClient socket,int timeoutMs,out byte[] payload,out IPEndPoint from) {
        var deadline=Stopwatch.StartNew();payload=null;from=null;
        while(deadline.ElapsedMilliseconds<timeoutMs) {
            byte[] data;IPEndPoint sender;
            int remaining=(int)Math.Max(1,timeoutMs-deadline.ElapsedMilliseconds);
            if(!TryReceive(socket,remaining,out data,out sender))return false;
            ulong sequence;byte[] opened;
            if(!GameDatagram.Open(key,expectedSeat,data,out sequence,out opened))continue;
            if(Bridge.IsHeartbeat(opened,expectedSeat))continue;
            payload=opened;from=sender;return true;
        }
        return false;
    }

    // MeshRelay end to end, on real sockets standing in for what a real mesh
    // has: three remote peers, each its own relay on its own machine, and one
    // local game that only ever speaks to loopback ports. Nothing here is the
    // native engine -- these are the exact bytes it would send and expect, built
    // by hand, so what is under test is MeshRelay's routing and authentication
    // and nothing about the game.
    //
    // The local game side is deliberately ONE socket sending to three different
    // leg ports with the targeted Send overload, not three throwaway sockets:
    // the real engine's UdpTransport is one socket registering N peer addresses,
    // and a leg only learns where to send a reply once it has seen that ONE
    // socket's port at least once -- exactly the property that a naive test
    // with a fresh socket per send would never exercise.
    // MeshWiring in isolation, no router or remote peer needed: it only has to
    // decide when to call AddPeer, and that decision does not touch a socket.
    static void MeshWiringTest() {
        var internet=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
        var relay=new MeshRelay(0,Wire.Random(16),internet);
        try {
            var wiring=new MeshWiring(relay,0);
            Check(!relay.HasPeer(1),"a fresh relay has no peers");
            wiring.OnEndpointLearned(1,new IPEndPoint(IPAddress.Loopback,40100));
            Check(relay.HasPeer(1),"the first announcement for a seat registers it");

            // A second announcement for the same seat must not throw -- Lobby
            // fires this from inside its own lock, and an unhandled exception
            // there would be a lot harder to diagnose than a silently ignored
            // duplicate.
            wiring.OnEndpointLearned(1,new IPEndPoint(IPAddress.Loopback,40100));
            wiring.OnEndpointLearned(1,new IPEndPoint(IPAddress.Loopback,40999));

            wiring.OnEndpointLearned(0,new IPEndPoint(IPAddress.Loopback,40200));
            Check(!relay.HasPeer(0),"our own seat is never registered as a peer of itself");

            wiring.OnEndpointLearned(2,new IPEndPoint(IPAddress.Loopback,40300));
            Check(relay.HasPeer(1) && relay.HasPeer(2) && !relay.HasPeer(3),
                "seats are registered independently of each other");
        } finally {
            relay.Dispose();
        }
    }

    static void MeshRouting() {
        var token=Wire.Random(16);var key=GameDatagram.Key(token);
        var remotes=new UdpClient[Lobby.MaxSeats];
        for(int seat=1;seat<Lobby.MaxSeats;seat++)remotes[seat]=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
        var internet=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
        var relay=new MeshRelay(0,token,internet);
        var events=new List<string>();relay.Diagnostic=events.Add;

        int port1=relay.AddPeer(1,(IPEndPoint)remotes[1].Client.LocalEndPoint);
        int port2=relay.AddPeer(2,(IPEndPoint)remotes[2].Client.LocalEndPoint);
        // Seat 3 is registered without an address, the way a guest is before the
        // host has told it who else is in the salon: learned from the first
        // authenticated packet that arrives claiming to be seat 3.
        int port3=relay.AddPeer(3,null);
        Reject(()=>relay.AddPeer(1,null),"a seat cannot be registered twice");
        Reject(()=>relay.AddPeer(0,null),"our own seat is not a peer");
        Reject(()=>relay.AddPeer(Lobby.MaxSeats,null),"a seat outside the table is refused");

        var run=relay.Run();
        var game=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
        try {
            var hostEndpoint=(IPEndPoint)internet.Client.LocalEndPoint;
            var leg1=new IPEndPoint(IPAddress.Loopback,port1);
            var leg2=new IPEndPoint(IPAddress.Loopback,port2);
            var leg3=new IPEndPoint(IPAddress.Loopback,port3);

            // The one local-game socket -> leg 1 -> remote 1, and nowhere else.
            game.Send(FakeGamePacket(0),GameDatagram.Payload,leg1);
            byte[] payload1;IPEndPoint from1;
            Check(TryReceiveGamePacket(key,0,remotes[1],2000,out payload1,out from1) && Bridge.Packet(payload1,0),
                "a local-game packet on leg 1 reaches remote 1, authentic and addressed as seat 0");
            byte[] stray;IPEndPoint strayFrom;
            Check(!TryReceiveGamePacket(key,0,remotes[2],150,out stray,out strayFrom),"and remote 2 receives nothing from that");

            // The same socket, sent to leg 2, must land on remote 2 and nowhere else.
            game.Send(FakeGamePacket(0),GameDatagram.Payload,leg2);
            byte[] payload2;IPEndPoint from2;
            Check(TryReceiveGamePacket(key,0,remotes[2],2000,out payload2,out from2),"the same socket reaches remote 2 through leg 2");
            Check(!TryReceiveGamePacket(key,0,remotes[1],150,out stray,out strayFrom),"remote 1 saw nothing from that second send");

            // Remote 1 -> us -> leg 1's loopback port -> the local game, tagged
            // with seat 1's own identity. This only works because leg 1 already
            // learned the game's address from the very first send above; that
            // dependency is the point, not an accident of ordering.
            var fromRemote1=GameDatagram.Seal(key,1,1000,FakeGamePacket(1));
            remotes[1].Send(fromRemote1,fromRemote1.Length,hostEndpoint);
            byte[] arrived;IPEndPoint arrivedFrom;
            Check(TryReceive(game,2000,out arrived,out arrivedFrom) && Bridge.Packet(arrived,1),
                "a packet authenticated as seat 1 is delivered back to the local game");

            // A forged seat byte cannot be produced without the key that signs
            // it; corrupting one after the fact must fail the HMAC, not just get
            // routed to the wrong leg.
            var tampered=(byte[])fromRemote1.Clone();tampered[5]=2;
            remotes[1].Send(tampered,tampered.Length,hostEndpoint);
            byte[] leaked;IPEndPoint leakedFrom;
            Check(!TryReceive(game,150,out leaked,out leakedFrom),
                "a seat byte flipped after sealing fails the HMAC, and reaches the game as nothing");

            // Seat 3 was registered with no known network address. There is
            // nowhere for the relay to send a game packet on that leg until
            // something arrives to learn an address from -- SendToLeg has
            // nothing to target, exactly like a two-player Bridge in
            // learnNetworkPeer mode before its one peer has said anything. The
            // attempt still teaches the relay where the LOCAL game listens on
            // this leg, though: that half of discovery does not wait on the
            // other half succeeding.
            byte[] nothingYet;IPEndPoint nothingYetFrom;
            game.Send(FakeGamePacket(0),GameDatagram.Payload,leg3);
            Check(!TryReceiveGamePacket(key,0,remotes[3],150,out nothingYet,out nothingYetFrom),
                "leg 3 has no address to relay to before seat 3 has ever spoken");

            // Seat 3 speaks first, teaching the relay its address. The game's own
            // address on this leg was already learned above, so this is
            // delivered immediately -- discovery in one direction does not have
            // to wait for discovery in the other.
            var fromRemote3=GameDatagram.Seal(key,3,500,FakeGamePacket(3));
            remotes[3].Send(fromRemote3,fromRemote3.Length,hostEndpoint);
            byte[] arrived3;IPEndPoint arrivedFrom3;
            Check(TryReceive(game,2000,out arrived3,out arrivedFrom3) && Bridge.Packet(arrived3,3),
                "seat 3's first packet reaches the game once the game's own address on that leg is known");

            // And now that seat 3's address is known too, a further game packet
            // on leg 3 relays all the way out.
            game.Send(FakeGamePacket(0),GameDatagram.Payload,leg3);
            byte[] sealed3;IPEndPoint from3;
            Check(TryReceiveGamePacket(key,0,remotes[3],2000,out sealed3,out from3),
                "leg 3 relays outbound now that seat 3's address is known too");

            // Replay: the exact same sequence number from seat 1 again must not
            // reach the local game a second time.
            remotes[1].Send(fromRemote1,fromRemote1.Length,hostEndpoint);
            byte[] replayed;IPEndPoint replayedFrom;
            Check(!TryReceive(game,150,out replayed,out replayedFrom),"a repeated sequence number from seat 1 is dropped as a replay");
        } finally {
            relay.Dispose();
            game.Close();
            foreach(var socket in remotes)if(socket!=null)socket.Close();
            try{run.Wait(2000);}catch{}
        }
    }

    static void Mods(byte[] disc) {
        var a=Mod(546878,"MP4DX",0x10);var b=Mod(620561,"Candlelight Fright Name Fix",0x40);
        var full=new ModSet(new[]{a,b});
        Check(full.Same(new ModSet(new[]{a,b})) && !full.Same(ModSet.Empty),"identical mod lists match, an empty one does not");
        Check(!full.Same(new ModSet(new[]{b,a})),"load order is part of the agreement");
        Check(!full.Same(new ModSet(new[]{a,Mod(620561,"Candlelight Fright Name Fix",0x99)})),"same mod, different build, blocked");

        var moved=new ModSet(new[]{b,a});
        Check(moved.DifferenceFrom(full).Contains("autre ordre"),"a pure reordering is named as one");
        Check(new ModSet(new[]{a}).DifferenceFrom(full).Contains("620561"),"a missing mod is named by its GameBanana id");
        Check(full.DifferenceFrom(new ModSet(new[]{a})).Contains("désactiver"),"an extra mod is reported as one to switch off");

        // The same "missing" set, structured for a download button per mod
        // instead of folded into ModAdvice's one string.
        var oneMissing=new ModSet(new[]{a}).Missing(full).ToArray();
        Check(oneMissing.Length==1 && oneMissing[0].Id==620561,"Missing names exactly the one mod this side lacks");
        Check(!full.Missing(full).Any(),"a set with everything required has nothing missing");
        Check(!full.Missing(ModSet.Empty).Any(),"nothing is missing from an empty requirement");
        Check(!ModSet.Empty.Missing(null).Any(),"a null requirement is treated as nothing required, not a crash");
        var localWrongBuild=new ModSet(new[]{a,Mod(620561,"Candlelight Fright Name Fix",0x99)});
        Check(!localWrongBuild.Missing(full).Any(),"a mod present at the wrong build is not reported as missing -- that's a CubeShelf update, not a download");

        var host=new PlayerInfo("Camille",disc,3,full);var guest=new PlayerInfo("Alex",disc,3,new ModSet(new[]{a}));
        Check(host.SameDisc(guest) && !host.SameMods(guest),"same disc is no longer enough");
        var hq=new Queue<byte[]>();var cq=new Queue<byte[]>();
        var h=new Lobby(true,host,(seat,x)=>hq.Enqueue(x),id=>{},id=>{},()=>{});
        var c=new Lobby(false,guest,(seat,x)=>cq.Enqueue(x),id=>{},id=>{},()=>{});
        h.Announce();c.Announce();c.Receive(hq.Dequeue());h.Receive(cq.Dequeue());
        Check(h.DiscMatches && !h.ModsMatch && !h.CanStart,"host cannot start while the mods differ");
        Check(h.ModAdvice.Contains("620561"),"the host is told exactly what the guest is missing");
        Reject(()=>c.Receive(Lobby.Command(3,Guid.NewGuid())),"guest refuses a launch its mods do not match");
        c.Update(new PlayerInfo("Alex",disc,3,full));h.Receive(cq.Dequeue());
        Check(h.ModsMatch && h.CanStart,"agreeing on the mods unblocks the launch");

        Check(PlayerInfo.Decode(host.Encode()).Mods.Same(full),"mod list survives the wire");
        Check(PlayerInfo.Decode(new PlayerInfo("Zoé",disc,3).Encode()).Mods.None,"a player with no mods announces none");
        Reject(()=>new ModSet(Enumerable.Range(1,ModSet.MaxMods+1).Select(i=>Mod(i,"m"+i,(byte)i))),"more mods than one message can carry is refused");
        ModsFromDisk();
    }

    // Reading CubeShelf's installed.json is the one part of this that talks to a file
    // written by another program, so it is exercised against a real one rather than a
    // stub. The rules mirrored here are PortableModManager.WriteActiveList's: enabled,
    // not switched off in-game, content root present, descending priority then id.
    static void ModsFromDisk() {
        var root=Path.Combine(Path.GetTempPath(),"pb-modtest-"+Guid.NewGuid().ToString("N"));
        var previous=Environment.GetEnvironmentVariable("PARTYBOARD_MOD_LIST");
        try {
            Directory.CreateDirectory(root);
            string low=Path.Combine(root,"100"),high=Path.Combine(root,"200"),off=Path.Combine(root,"300"),gone=Path.Combine(root,"400");
            foreach(var d in new[]{low,high,off})Directory.CreateDirectory(d);
            // "GO!, PAUSED," is a real mod name and it contains escaped quotes: a
            // hand-rolled parser would have swallowed the rest of the file on it.
            File.WriteAllText(Path.Combine(root,"installed.json"),
                "[{\"Id\":100,\"Name\":\"\\\"GO!, PAUSED,\\\" and \\\"TIE!\\\"\",\"Enabled\":true,\"Priority\":100,\"ContentRoot\":"+Quote(low)+",\"Sha256\":\"0102030405060708aabbccdd\"},"+
                "{\"Id\":200,\"Name\":\"MP4DX\",\"Enabled\":true,\"Priority\":120,\"ContentRoot\":"+Quote(high)+",\"Sha256\":\"1112131415161718\"},"+
                "{\"Id\":300,\"Name\":\"Switched off\",\"Enabled\":false,\"Priority\":130,\"ContentRoot\":"+Quote(off)+",\"Sha256\":\"21\"},"+
                "{\"Id\":400,\"Name\":\"Folder deleted\",\"Enabled\":true,\"Priority\":140,\"ContentRoot\":"+Quote(gone)+",\"Sha256\":\"31\"}]",
                new UTF8Encoding(false));
            Environment.SetEnvironmentVariable("PARTYBOARD_MOD_LIST",Path.Combine(root,"online-mods.txt"));

            string from;var set=ModSet.FromCubeShelf("GMPE01_00",out from);
            Check(set.Entries.Length==2,"only enabled mods whose folder still exists are announced");
            Check(set.Entries[0].Id==200 && set.Entries[1].Id==100,"highest priority first, exactly as the game loads them");
            Check(set.Entries[0].Fingerprint[0]==0x11 && set.Entries[1].Fingerprint[0]==0x01,"the recorded SHA-256 is what identifies a build");
            Check(set.Entries[1].Name.Contains("GO!"),"a name with escaped quotes survives");

            File.WriteAllText(Path.Combine(root,"player-disabled.json"),"[200]",new UTF8Encoding(false));
            set=ModSet.FromCubeShelf("GMPE01_00",out from);
            Check(set.Entries.Length==1 && set.Entries[0].Id==100,"a mod switched off inside the game is not announced");

            var listPath=set.WriteListFile(root);
            Check(File.ReadAllLines(listPath).Length==1 && File.ReadAllLines(listPath)[0]==Path.GetFullPath(low),
                "the list handed to the game is the list that was announced");
        } finally {
            Environment.SetEnvironmentVariable("PARTYBOARD_MOD_LIST",previous);
            try{Directory.Delete(root,true);}catch{}
        }
    }

    static string Quote(string path){return "\""+path.Replace("\\","\\\\")+"\"";}

    // Four seats, driven entirely in memory: the state machine is the part that can
    // be checked without sockets, and it is the part that decides whether a salon
    // starts a session everyone agreed to.
    //
    // The cases that a two-player salon cannot express are the point. One silent
    // guest must hold the start. One disagreeing guest must block it while the
    // others are fine. And a guest leaving must reach the guests it never talks to,
    // because in a star they only ever hear the host.
    // The endpoint-announcement protocol, in memory, the same way Seats() drives
    // the seat/readiness state machine: one host Lobby with a queue-capturing
    // send, fed hand-built wire bytes standing in for what a real guest's Lobby
    // would produce. What matters here is the star's relaying and bookkeeping,
    // not the sockets underneath it -- those are MeshRelay's job, already
    // covered on its own.
    static void Endpoints(byte[] disc) {
        var mod=Mod(546878,"MP4DX",0x10);var full=new ModSet(new[]{mod});
        Func<string,ModSet,PlayerInfo> who=(name,mods)=>new PlayerInfo(name,disc,3,mods);

        var hq=new Dictionary<int,Queue<byte[]>>();
        for(int i=-1;i<Lobby.MaxSeats;i++)hq[i]=new Queue<byte[]>();
        var learned=new List<Tuple<int,IPEndPoint>>();
        var host=new Lobby(true,who("Camille",full),(seat,b)=>hq[seat].Enqueue(b),id=>{},id=>{},()=>{});
        host.EndpointLearned=(seat,endpoint)=>learned.Add(Tuple.Create(seat,endpoint));
        host.Admit(1,who("Alex",full));
        host.Admit(2,who("Zoe",full));
        hq[1].Clear();hq[2].Clear();   // discard the seat-command/profile pair Admit already sent each

        var hostEp=new IPEndPoint(IPAddress.Parse("203.0.113.10"),40001);
        host.AnnounceEndpoint(hostEp);
        Check(host.PeerEndpoint(0).Equals(hostEp),"the host knows its own announced address");
        Check(hq[Lobby.Broadcast].Count==1,"the host's own announcement goes out once, addressed Broadcast");
        int subjectSeat0;IPEndPoint decoded0;
        Check(TryDecodeEndpoint(hq[Lobby.Broadcast].Dequeue(),out subjectSeat0,out decoded0) && subjectSeat0==0 && decoded0.Equals(hostEp),
            "the host's announcement names seat 0 and its own address");

        // Guest 1 announces itself. The host must learn it, and relay it to
        // every OTHER seat -- seat 2, not back to seat 1.
        var guest1Ep=new IPEndPoint(IPAddress.Parse("203.0.113.20"),40002);
        host.Receive(1,Lobby.EndpointCommand(1,guest1Ep));
        Check(host.PeerEndpoint(1).Equals(guest1Ep),"the host learns guest 1's announced address");
        Check(learned.Count==1 && learned[0].Item1==1 && learned[0].Item2.Equals(guest1Ep),
            "EndpointLearned fires with the seat and the address");
        Check(hq[2].Count==1,"the relay reaches seat 2");
        Check(hq[1].Count==0,"and is not echoed back to seat 1");
        int relayedSeat;IPEndPoint relayedEp;
        Check(TryDecodeEndpoint(hq[2].Dequeue(),out relayedSeat,out relayedEp) && relayedSeat==1 && relayedEp.Equals(guest1Ep),
            "the relayed packet still names seat 1, not the host relaying it");

        Reject(()=>host.Receive(1,Lobby.EndpointCommand(2,new IPEndPoint(IPAddress.Parse("203.0.113.30"),1))),
            "a guest cannot announce another seat's address");
        Reject(()=>host.Receive(1,new byte[]{11,1,1,2,3,4,0}),"a truncated endpoint command is refused");
        Reject(()=>host.Receive(1,new byte[]{11,1,1,2,3,4,0,0,0}),"an oversized endpoint command is refused");

        // A guest admitted after seat 1 already announced still needs seat 1's
        // address -- without a catch-up replay it would never learn it, since
        // that broadcast happened before this guest existed.
        host.Admit(3,who("Remi",full));
        var toGuest3=new List<byte[]>();while(hq[3].Count>0)toGuest3.Add(hq[3].Dequeue());
        Check(toGuest3.Any(p=>{int s;IPEndPoint e;return TryDecodeEndpoint(p,out s,out e) && s==1 && e.Equals(guest1Ep);}),
            "the late guest's admission replays seat 1's already-known address");
        Check(toGuest3.Any(p=>{int s;IPEndPoint e;return TryDecodeEndpoint(p,out s,out e) && s==0 && e.Equals(hostEp);}),
            "and the host's own address too");

        // Leaving clears the bookkeeping, not just the roster entry.
        host.Leave(1);
        Check(host.PeerEndpoint(1)==null,"a departed seat's address is forgotten");

        // The guest side of the same protocol: a guest only ever hears from the
        // host (from==0), whether the subject is the host itself or a fellow
        // guest being relayed. Either way the guest just records it.
        var gq=new Dictionary<int,Queue<byte[]>>();
        for(int i=-1;i<Lobby.MaxSeats;i++)gq[i]=new Queue<byte[]>();
        var guestLearned=new List<Tuple<int,IPEndPoint>>();
        var guest=new Lobby(false,who("Alex",full),(seat,b)=>gq[seat].Enqueue(b),id=>{},id=>{},()=>{});
        guest.EndpointLearned=(seat,endpoint)=>guestLearned.Add(Tuple.Create(seat,endpoint));
        guest.Receive(0,Lobby.EndpointCommand(0,hostEp));
        Check(guest.PeerEndpoint(0).Equals(hostEp),"a guest records the host's announced address");
        var guest2Ep=new IPEndPoint(IPAddress.Parse("203.0.113.40"),40003);
        guest.Receive(0,Lobby.EndpointCommand(2,guest2Ep));
        Check(guest.PeerEndpoint(2).Equals(guest2Ep),"and a fellow guest's address, relayed by the host");
        Check(guestLearned.Count==2,"both arrivals fired the callback");
        gq[0].Clear();
        guest.AnnounceEndpoint(new IPEndPoint(IPAddress.Parse("203.0.113.50"),40004));
        Check(gq[0].Count==1,"a guest announces to the host, not by broadcast");
    }
    static bool TryDecodeEndpoint(byte[] packet,out int seat,out IPEndPoint endpoint) {
        seat=-1;endpoint=null;
        if(packet.Length!=8 || packet[0]!=11)return false;
        seat=packet[1];
        var address=new byte[4];Buffer.BlockCopy(packet,2,address,0,4);
        int port=(packet[6]<<8)|packet[7];
        endpoint=new IPEndPoint(new IPAddress(address),port);return true;
    }

    static void Seats(byte[] disc) {
        var mod=Mod(546878,"MP4DX",0x10);
        var full=new ModSet(new[]{mod});
        Func<string,ModSet,PlayerInfo> who=(name,mods)=>new PlayerInfo(name,disc,3,mods);

        var hq=new Dictionary<int,Queue<byte[]>>();
        for(int i=-1;i<Lobby.MaxSeats;i++)hq[i]=new Queue<byte[]>();
        int starts=0,loads=0;
        var host=new Lobby(true,who("Camille",full),(seat,b)=>hq[seat].Enqueue(b),id=>loads++,id=>starts++,()=>{});

        Reject(()=>host.Start(),"a host alone cannot start");
        host.Admit(1,who("Alex",full));
        Check(host.Occupied==2 && host.CanStart,"two seats agreeing is enough to start");
        host.Admit(2,who("Zoe",full));
        host.Admit(3,who("Remi",full));
        Check(host.Occupied==4 && host.CanStart,"four seats agreeing still start");
        Reject(()=>host.Admit(4,who("Trop",full)),"a fifth player is refused");
        Reject(()=>host.Admit(0,who("Usurpateur",full)),"nobody is admitted to the host seat");

        // One guest out of step blocks everyone, and is named rather than implied.
        host.Leave(2);host.Admit(2,who("Zoe",ModSet.Empty));
        Check(!host.CanStart && !host.ModsMatch,"one guest with different mods blocks the launch");
        Check(host.ModAdvice.Contains("Zoe") && host.ModAdvice.Contains("546878"),"the host is told which guest, and which mod");
        Check(host.DiscMatches,"the disc is still agreed while only the mods differ");
        host.Leave(2);host.Admit(2,who("Zoe",full));
        Check(host.CanStart,"putting it right unblocks the launch");

        // The readiness barrier: every occupied seat, not two of them.
        host.Start();
        Check(loads==1 && starts==0,"starting prepares the host and commits nobody");
        var attempt=new Guid(hq[Lobby.Broadcast].Dequeue().Skip(1).ToArray());
        host.Loaded(attempt);
        Check(starts==0,"the host being ready is not enough");
        host.Receive(1,Lobby.Command(4,attempt));
        Check(starts==0,"one guest ready is not enough");
        host.Receive(2,Lobby.Command(4,attempt));
        Check(starts==0,"two guests ready is still not enough");
        Reject(()=>host.Receive(2,Lobby.Command(4,attempt)),"a guest cannot confirm twice");
        host.Receive(3,Lobby.Command(4,attempt));
        Check(starts==1 && host.Phase==LobbyPhase.Running,"the last guest releases the start");

        // A departure has to be relayed, or the guests that never hear from each
        // other stay in a salon that cannot empty.
        hq[1].Clear();hq[2].Clear();hq[3].Clear();
        host.Receive(1,Lobby.Command(8,attempt));
        Check(host.Phase==LobbyPhase.Closed,"the host closes when a guest quits");
        Check(hq[2].Count==1 && hq[3].Count==1 && hq[1].Count==0,"the host relays the departure to the others, not back to the sender");
        Check(hq[2].Peek()[0]==8,"and it relays it as a departure");

        // A guest learns its seat from the host, and only from the host.
        var gq=new Dictionary<int,Queue<byte[]>>();
        for(int i=-1;i<Lobby.MaxSeats;i++)gq[i]=new Queue<byte[]>();
        var guest=new Lobby(false,who("Alex",full),(seat,b)=>gq[seat].Enqueue(b),id=>{},id=>{},()=>{});
        Check(guest.LocalSeat==1,"a guest starts at seat 1 until told otherwise");
        guest.Receive(0,Lobby.SeatCommand(3));
        Check(guest.LocalSeat==3 && guest.Local.Name=="Alex","the host moves a guest, and it takes its profile with it");
        Reject(()=>guest.Receive(0,Lobby.SeatCommand(0)),"a guest is never moved into the host seat");
        Reject(()=>guest.Receive(0,new byte[]{9,7}),"a seat beyond the table is refused");
        Reject(()=>host.Receive(1,Lobby.SeatCommand(2)),"a guest cannot hand out seats");
        Reject(()=>guest.Receive(guest.LocalSeat,Lobby.SeatCommand(2)),"nothing arrives from our own seat");

        // A seat can be reserved the moment a connection arrives, before the
        // host has any idea who is on the other end -- the profile shows up
        // later, over the very link this seat number identifies.
        var hq2=new Dictionary<int,Queue<byte[]>>();
        for(int i=-1;i<Lobby.MaxSeats;i++)hq2[i]=new Queue<byte[]>();
        var host2=new Lobby(true,who("Camille",full),(seat,b)=>hq2[seat].Enqueue(b),id=>{},id=>{},()=>{});
        host2.Admit(1);
        Check(host2.SeatInfo(1)==null,"reserving a seat without a profile leaves its occupant unknown");
        Check(host2.Occupied==1,"and does not count as an occupied seat yet");
        Check(hq2[1].Count==2,"the reservation still sends exactly the seat number and the host's own profile");
        Check(hq2[1].Dequeue()[0]==9,"the first message is the seat assignment");
        Check(hq2[1].Dequeue()[0]==2,"the second is the host's profile, same as an ordinary admit");
        Reject(()=>host2.Start(),"a reserved-but-unknown seat still cannot start a game");
        host2.Receive(1,who("Alex",full).Encode());
        Check(host2.SeatInfo(1)!=null && host2.SeatInfo(1).Name=="Alex","the guest's own Announce() fills the reservation in, exactly as for an ordinary admit");
        Check(host2.Occupied==2 && host2.CanStart,"and from then on it counts like any other occupied seat");

        // Admitting without a profile must never clobber one that is already
        // known -- the omitted argument means "leave it alone," not "erase it."
        host2.Admit(1);
        Check(host2.SeatInfo(1)!=null && host2.SeatInfo(1).Name=="Alex","re-admitting a seat with no profile leaves its known occupant untouched");
    }

    // Pure string-building, no socket in sight: what seat 2 of a four-player
    // mesh is actually told to run with, given the loopback ports its two
    // peers' legs were assigned.
    static void MeshArgumentsTest() {
        var peers=new[]{Tuple.Create(0,50010),Tuple.Create(3,50013)};
        var args=GameStart.MeshArguments(50002,2,4,peers);
        Check(args=="--netplay-host 50002 --netplay-players 4 --netplay-seat 2 --netplay-peer 0:127.0.0.1:50010 --netplay-peer 3:127.0.0.1:50013",
            "mesh arguments name this seat, the table size, and one peer per remote seat, in the order given");
        Check(GameStart.MeshArguments(50000,0,3,new Tuple<int,int>[0])=="--netplay-host 50000 --netplay-players 3 --netplay-seat 0",
            "a seat with no peers yet still gets a valid, peerless command line");
        Reject(()=>GameStart.MeshArguments(50000,0,2,peers),"two players has no mesh arguments -- that path stays the classic host/join pair");
        Reject(()=>GameStart.MeshArguments(50000,0,5,peers),"a fifth seat is refused, same bound as Lobby.MaxSeats");
        Reject(()=>GameStart.MeshArguments(50000,4,4,peers),"a seat outside its own table is refused");
        Reject(()=>GameStart.MeshArguments(0,0,4,peers),"port zero is not a real port");
        Reject(()=>GameStart.MeshArguments(50000,0,4,new[]{Tuple.Create(0,50010)}),"a peer cannot be our own seat");
        Reject(()=>GameStart.MeshArguments(50000,0,4,new[]{Tuple.Create(5,50010)}),"a peer seat outside the table is refused");
        Reject(()=>GameStart.MeshArguments(50000,0,4,new[]{Tuple.Create(1,50010),Tuple.Create(1,50011)}),"the same peer seat cannot be named twice");
    }
    static void LobbyRules() {
        var launch=GameStart.OnlineArguments("--netplay-host 32100");
        Check(launch.Contains("--netplay-full") && launch.Contains("--netplay-delay 3")
            && !launch.Contains("--netplay-rollback"),"normal lobby uses lockstep without automatic rollback");
        MeshArgumentsTest();
        var same=Wire.Hash(new byte[]{1,2,3});var profile=new PlayerInfo("Camille",same,3);
        var hq=new Queue<byte[]>();var cq=new Queue<byte[]>();Guid hId=Guid.Empty,cId=Guid.Empty;int starts=0,loads=0;
        var h=new Lobby(true,profile,(seat,b)=>hq.Enqueue(b),id=>{hId=id;loads++;},id=>starts++,()=>{});
        var c=new Lobby(false,new PlayerInfo("Alex",same,3),(seat,b)=>cq.Enqueue(b),id=>{cId=id;loads++;},id=>starts++,()=>{});
        Reject(()=>h.Start(),"host cannot start alone");h.Announce();c.Announce();c.Receive(hq.Dequeue());h.Receive(cq.Dequeue());
        Check(h.CanStart && !c.CanStart && h.Remote.Name=="Alex","host authority and roster");
        Reject(()=>c.Start(),"guest launch rejected");Reject(()=>h.Receive(Lobby.Command(3,Guid.NewGuid())),"forged guest PREPARE rejected");
        c.Update(new PlayerInfo("Alex",Wire.Hash(new byte[]{1,2,4}),3));h.Receive(cq.Dequeue());Check(!h.CanStart,"different full disk hash blocks launch");
        c.Update(new PlayerInfo("Alex",same,4));h.Receive(cq.Dequeue());Check(!h.CanStart,"different disk length blocks launch");
        c.Update(new PlayerInfo("Alex",same,3));h.Receive(cq.Dequeue());h.Start();var prepare=hq.Dequeue();c.Receive(prepare);
        Check(loads==2 && starts==0,"prepare loads everyone without simulation");
        Reject(()=>c.Receive(prepare),"repeated prepare cannot relaunch");Reject(()=>c.Receive(Lobby.Command(5,cId)),"commit before loaded rejected");
        Reject(()=>h.Receive(Lobby.Command(4,Guid.NewGuid())),"stale ready rejected");
        h.Loaded(hId);Check(starts==0,"one ready is insufficient");c.Loaded(cId);h.Receive(cq.Dequeue());c.Receive(hq.Dequeue());
        Check(starts==2 && h.Phase==LobbyPhase.Running && c.Phase==LobbyPhase.Running,"both ready then authenticated host commits");
        Reject(()=>h.Start(),"double start rejected");Reject(()=>h.Update(profile),"metadata frozen during game");
        // The reported bug, reproduced: one player quits the game, and the other used to
        // stay in Running with no way out. The departure is announced and the peer leaves.
        Reject(()=>c.Receive(Lobby.Command(8,Guid.NewGuid())),"end notice for another attempt rejected");
        h.LocalGameExited();
        Check(h.Phase==LobbyPhase.Closed && h.Ending==LobbyEnding.LocalGameClosed,"quitting closes the salon that quit");
        c.Receive(hq.Dequeue());
        Check(c.Phase==LobbyPhase.Closed && c.Ending==LobbyEnding.RemoteGameClosed,"the other player leaves the room too");
        Reject(()=>c.Receive(Lobby.Command(8,cId)),"end notice on a closed salon rejected");
        var wq=new Queue<byte[]>();var w=new Lobby(true,profile,(seat,b)=>wq.Enqueue(b),id=>{},id=>{},()=>{});
        Reject(()=>w.Receive(Lobby.Command(8,Guid.NewGuid())),"end notice cannot knock over a waiting salon");
        h.Close();h.Loaded(hId);Check(starts==2 && !h.CanStart,"closed session cannot start");
        Reject(()=>new PlayerInfo("\n"),"empty/control nickname rejected");Reject(()=>PlayerInfo.Decode(new byte[]{2,96,1}),"truncated metadata rejected");
        Mods(same);
        Seats(same);
        Endpoints(same);
        Check(PlayerInfo.Decode(new PlayerInfo("Élodie",same,3).Encode()).Name=="Élodie","UTF8 nickname preserved");
    }
    static void Discs() {
        var dir=Path.Combine(Path.GetTempPath(),"PartyBoardDiscTest-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(dir);
        var a=Path.Combine(dir,"a.iso");var b=Path.Combine(dir,"different-name.iso");var c=Path.Combine(dir,"c.iso");
        try {
            var data=Wire.Random(65536);File.WriteAllBytes(a,data);File.WriteAllBytes(b,data);data[65535]^=1;File.WriteAllBytes(c,data);
            Reject(()=>DiscFile.Verify(a,_=>{},CancellationToken.None),"native disc checker rejects arbitrary file");
            using(var first=DiscFile.Verify(a,_=>{},CancellationToken.None,false))
            using(var second=DiscFile.Verify(b,_=>{},CancellationToken.None,false))
            using(var third=DiscFile.Verify(c,_=>{},CancellationToken.None,false)) {
                Check(Wire.Equal(first.Hash,second.Hash),"same bytes with different filename accepted");
                Check(!Wire.Equal(first.Hash,third.Hash),"one changed byte at end of disk detected");
                Reject(()=>{using(var f=File.Open(a,FileMode.Open,FileAccess.Write,FileShare.ReadWrite)){}},"verified disk cannot be modified");
            }
            using(var token=new CancellationTokenSource()){token.Cancel();Reject(()=>DiscFile.Verify(a,_=>{},token.Token,false),"hash cancellation");}
            using(var file=File.Open(a,FileMode.Open,FileAccess.Write,FileShare.None)){} Check(true,"file lock released after verification/session");
        }finally{File.Delete(a);File.Delete(b);File.Delete(c);Directory.Delete(dir);}
    }
    static void CancelLoading() {
        int port;using(var reserve=new UdpClient(new IPEndPoint(IPAddress.Loopback,0)))port=((IPEndPoint)reserve.Client.LocalEndPoint).Port;
        using(var game=new GameStart(Guid.NewGuid())) {
            game.Launch("--netplay-host "+port+" --netplay-loopback --netplay-full","barrier-test-only",true);
            try {
                game.WaitReady(CancellationToken.None);Check(!game.Process.HasExited,"native process waits at loading barrier");
                game.Abort();Check(game.Process.WaitForExit(5000) && game.Process.ExitCode==4,"cancel closes native startup without simulation");
            }finally{if(!game.Process.HasExited)game.Process.Kill();game.Process.Dispose();}
        }
    }
    static void Reports() {
        var report=new Report(Path.GetTempPath());report.Write("role=host test=export");
        Check(report.Read().Contains("role=host test=export"),"report export");
        File.WriteAllText(report.NativePath,"native_fixture");
        File.WriteAllText(report.NativePath+".desync","DESYNC frame=87 localHash=111 remoteHash=222");
        Check(report.Read().Contains("DESYNC frame=87"),"desync sidecar survives capped native diagnostic export");
        Check(report.Read().Contains("native_fixture"),"combined native export");
        for(int i=0;i<3010;i++)report.Write("bounded");
        Check(File.ReadAllLines(Path.Combine(report.DirectoryPath,"session.txt")).Length==3000,"bounded diagnostic log");
    }
    // The accept loop's own bookkeeping, exercised without a socket in sight:
    // three guests whose two channels arrive out of order and interleaved,
    // exactly as three real connections racing over the Internet would land.
    static void ChannelPairingTest() {
        var pairing=new ChannelPairing<string>();
        var alex=Wire.Random(16);var zoe=Wire.Random(16);var remi=Wire.Random(16);
        Check(pairing.Add(alex,0,"alex-0")==null,"a first channel alone never completes a guest");
        Check(pairing.Pending==1,"and leaves exactly one guest waiting on its second channel");
        Check(pairing.Add(zoe,1,"zoe-1")==null,"a different guest's channel does not complete anyone either");
        var done=pairing.Add(alex,1,"alex-1");
        Check(done!=null && done[0]=="alex-0" && done[1]=="alex-1","the second channel completes the right guest, in channel order");
        Check(pairing.Pending==1,"completing one guest leaves the other still waiting");
        Reject(()=>pairing.Add(zoe,1,"zoe-1-again"),"the same channel cannot arrive twice for one guest");
        Check(pairing.Add(zoe,0,"zoe-0")!=null,"zoe's outstanding channel completes her, unaffected by the earlier rejection");
        Check(pairing.Pending==0,"nobody is left waiting once both guests are complete");
        Check(pairing.Add(remi,0,"remi-0")==null && pairing.Add(remi,1,"remi-1")!=null,"a third guest pairs exactly like the first two");
        Reject(()=>pairing.Add(Wire.Random(16),2,"x"),"a channel index outside 0/1 is refused");
    }
    public static int Run() {try{Reports();Codecs();Leases();LobbyRules();Discs();CancelLoading();MeshWiringTest();MeshRouting();ControlOnlyLink();ChannelPairingTest();Tls(0,true,true);for(int mode=0;mode<4;mode++)Tls(mode,mode==0);Console.WriteLine("PASS: "+checks+" checks; host-only lobby start, full disk hash and locks, UDP ping, native loading barrier/cancel, TLS loss before/after commit, authenticated UDP for 4800 native ticks and a real three-peer UDP mesh. No real router/firewall changes.");return 0;}catch(Exception e){Console.Error.WriteLine("FAIL: "+e.ToString());return 1;}}
}
}
