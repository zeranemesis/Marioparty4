using System;
using System.IO;
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
                    bool input=data.Length==82 && data[16]=='R' && data[17]=='B';
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
        Check(invite.Encode().Length==60,"compact invitation length");var decoded=Invitation.Decode(invite.Encode());Check(decoded.Port==32000 && Wire.Equal(invite.Token,decoded.Token) && Wire.Equal(invite.Fingerprint,decoded.Fingerprint),"invitation round trip");
        Reject(()=>Invitation.Decode("bad"),"malformed invite");Reject(()=>Invitation.Decode(new string('x',1000)),"bounded invite");
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
        var payload=new byte[52];payload[0]=80;payload[1]=66;payload[2]=82;payload[3]=66;payload[4]=0;payload[5]=5;payload[6]=1;payload[7]=0;
        var datagramKey=GameDatagram.Key(invite.Token);var datagram=GameDatagram.Seal(datagramKey,0,42,payload);ulong sequence;byte[] opened;
        Check(GameDatagram.Open(datagramKey,0,datagram,out sequence,out opened) && sequence==42 && Wire.Equal(payload,opened),"authenticated UDP game packet round trip");
        datagram[20]^=1;Check(!GameDatagram.Open(datagramKey,0,datagram,out sequence,out opened),"tampered UDP game packet rejected");
        Check(!GameDatagram.Open(Wire.Random(32),0,GameDatagram.Seal(datagramKey,0,43,payload),out sequence,out opened),"foreign UDP game packet rejected");
        var udpPcp=Gateway.PcpRequest(IPAddress.Parse("192.168.1.2"),key,32000,32000,120,17);Check(udpPcp[36]==17,"PCP UDP mapping request");
        var udpPmp=Gateway.PmpRequest(32000,32000,120,1);Check(udpPmp[1]==1,"NAT-PMP UDP mapping request");
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
                        var hl=new Lobby(true,profile,a.SendControl,id=>{hi=id;hg=new GameStart(id);hg.Launch(GameStart.OnlineArguments("--netplay-host "+gamePort)+" --netplay-probe-realtime","barrier-test-only",true,diagHost.NativePath);hr.Set();},id=>{a.CommitGame();hg.Commit(id);},()=>{});
                        var cl=new Lobby(false,profile,b.SendControl,id=>{ci=id;cg=new GameStart(id);cg.Launch(GameStart.OnlineArguments("--netplay-join 127.0.0.1:"+b.LocalPort)+" --netplay-probe-realtime","barrier-test-only",true,diagGuest.NativePath);cr.Set();},id=>{b.CommitGame();cg.Commit(id);},()=>{});
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
                                Check(ho.Result.Contains("PASS: 2400/2400") && co.Result.Contains("PASS: 2400/2400"),"4800 native input ticks through authenticated UDP with 12-second pause and dropped input repair");Check(diagHost.Read().Contains("event=checkpoint") && diagGuest.Read().Contains("live_rng="),"frame-aligned live state diagnostics on both peers");Check(System.Text.RegularExpressions.Regex.IsMatch(diagHost.Read(),@"repaired=[1-9][0-9]*"),"report identifies retained-input repair");Check(a.GamePackets<8000 && b.GamePackets<8000,"bounded redundant input and repair traffic");
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
    static void LobbyRules() {
        var launch=GameStart.OnlineArguments("--netplay-host 32100");
        Check(launch.Contains("--netplay-full") && launch.Contains("--netplay-delay 3")
            && !launch.Contains("--netplay-rollback"),"normal lobby uses lockstep without automatic rollback");
        var same=Wire.Hash(new byte[]{1,2,3});var profile=new PlayerInfo("Camille",same,3);
        var hq=new Queue<byte[]>();var cq=new Queue<byte[]>();Guid hId=Guid.Empty,cId=Guid.Empty;int starts=0,loads=0;
        var h=new Lobby(true,profile,b=>hq.Enqueue(b),id=>{hId=id;loads++;},id=>starts++,()=>{});
        var c=new Lobby(false,new PlayerInfo("Alex",same,3),b=>cq.Enqueue(b),id=>{cId=id;loads++;},id=>starts++,()=>{});
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
        h.Close();h.Loaded(hId);Check(starts==2 && !h.CanStart,"closed session cannot start");
        Reject(()=>new PlayerInfo("\n"),"empty/control nickname rejected");Reject(()=>PlayerInfo.Decode(new byte[]{2,96,1}),"truncated metadata rejected");
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
        Check(report.Read().Contains("native_fixture"),"combined native export");
        for(int i=0;i<3010;i++)report.Write("bounded");
        Check(File.ReadAllLines(Path.Combine(report.DirectoryPath,"session.txt")).Length==3000,"bounded diagnostic log");
    }
    public static int Run() {try{Reports();Codecs();Leases();LobbyRules();Discs();CancelLoading();Tls(0,true,true);for(int mode=0;mode<4;mode++)Tls(mode,mode==0);Console.WriteLine("PASS: "+checks+" checks; host-only lobby start, full disk hash and locks, UDP ping, native loading barrier/cancel, TLS loss before/after commit and authenticated UDP for 4800 native ticks. No real router/firewall changes.");return 0;}catch(Exception e){Console.Error.WriteLine("FAIL: "+e.ToString());return 1;}}
}
}
