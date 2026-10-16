using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Net.Security;
using System.Security.Cryptography.X509Certificates;
using System.Diagnostics;
using System.Drawing;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PartyBoardOnline {
sealed class Firewall : IDisposable {
    EventWaitHandle stop;
    static string Name(int pid,int port,string suffix) {return "Local\\PartyBoardOnline-"+pid+"-"+port+suffix;}
    public static Firewall Open(int port,bool lan=false) {
        int pid=Process.GetCurrentProcess().Id;
        var result=new Firewall{stop=new EventWaitHandle(false,EventResetMode.ManualReset,Name(pid,port,"-stop"))};
        try {
            using(var ready=new EventWaitHandle(false,EventResetMode.ManualReset,Name(pid,port,"-ready"))) {
                var info=new ProcessStartInfo(Application.ExecutablePath,"--firewall "+port+" "+pid+(lan?" lan":"")){UseShellExecute=true,Verb="runas",WindowStyle=ProcessWindowStyle.Hidden};
                using(var p=Process.Start(info)) {
                    if(!ready.WaitOne(15000)) throw new IOException(L.T("Windows n'a pas autorisé la connexion. Réessayez et acceptez sa demande d'autorisation."));
                    if(p.HasExited) throw new IOException(L.T("Windows n'a pas pu préparer la connexion."));
                }
            }
            return result;
        } catch {result.Dispose();throw;}
    }
    public static int Broker(int port,int pid,bool lan=false) {
        if(port<1024 || port>65535 || pid<=0) return 2;
        string ruleName="PartyBoard temporary "+pid+" "+port;dynamic policy=null;bool tcpAdded=false,udpAdded=false;
        try {
            using(var parent=Process.GetProcessById(pid)) {
                if(!String.Equals(parent.MainModule.FileName,Application.ExecutablePath,StringComparison.OrdinalIgnoreCase)) return 2;
                using(var ready=EventWaitHandle.OpenExisting(Name(pid,port,"-ready")))
                using(var stop=EventWaitHandle.OpenExisting(Name(pid,port,"-stop"))) {
                    if(stop.WaitOne(0)) return 0;
                    policy=Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FwPolicy2"));
                    dynamic rule=Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FWRule"));
                    rule.Name=ruleName+" TCP";rule.Description="PartyBoard : salon chiffré temporaire, supprimé à la fermeture.";
                    rule.ApplicationName=Application.ExecutablePath;rule.Protocol=6;rule.LocalPorts=port.ToString();
                    rule.Direction=1;rule.Action=1;rule.Enabled=true;rule.Profiles=7;
                    policy.Rules.Add(rule);tcpAdded=true;
                    dynamic datagram=Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FWRule"));
                    datagram.Name=ruleName+" UDP";datagram.Description="PartyBoard : commandes de jeu authentifiées, règle supprimée à la fermeture.";
                    datagram.ApplicationName=Application.ExecutablePath;datagram.Protocol=17;datagram.LocalPorts=lan?port+","+LanDiscovery.Port:port.ToString();
                    datagram.Direction=1;datagram.Action=1;datagram.Enabled=true;datagram.Profiles=7;
                    policy.Rules.Add(datagram);udpAdded=true;ready.Set();
                    var timer=Stopwatch.StartNew();
                    while(!stop.WaitOne(500) && !parent.HasExited && timer.Elapsed.TotalHours<12) {}
                }
            }
            return 0;
        } catch {return 1;} finally {if(tcpAdded) try{policy.Rules.Remove(ruleName+" TCP");}catch{} if(udpAdded) try{policy.Rules.Remove(ruleName+" UDP");}catch{}}
    }
    public void Dispose() {if(stop!=null) {stop.Set();stop.Dispose();stop=null;}}
}

enum PublicState {Private,Pending,Listed}

sealed class Session : IDisposable {
    public Invitation Invite; public Bridge Bridge; public bool Host;
    public Lobby Lobby;volatile PlayerInfo initialProfile;readonly DiscFile disc;
    // What this player announces right now. The salon may change its mods while
    // waiting (ChangeMods), so the copy that counts is the Lobby's own seat, and the
    // one passed in only stands in until the Lobby exists. Lobby.Update refuses any
    // change once a launch begins, which is what makes reading this in LoadGame
    // safe: the list handed to the game is the one every peer agreed to.
    public PlayerInfo Profile {get{var lobby=Lobby;return lobby!=null?lobby.Local:initialProfile;}}
    public void ChangeMods(ModSet mods) {
        var lobby=Lobby;
        if(lobby==null) {
            // A two-player host waits alone, with no salon yet: the mods it
            // will announce are the ones its profile holds. If the guest arrived
            // meanwhile, the salon is told as well.
            var before=initialProfile;
            initialProfile=new PlayerInfo(before.Name,before.DiscHash,before.DiscLength,mods,before.Language);
            lobby=Lobby;
            if(lobby==null){Report.Write("mods_changed count="+mods.Entries.Length+" before_salon");connected();return;}
        }
        var current=lobby.Local;
        lobby.Update(new PlayerInfo(current.Name,current.DiscHash,current.DiscLength,mods,current.Language));
        Report.Write("mods_changed count="+mods.Entries.Length+" ids="+string.Join(",",mods.Entries.Select(m=>m.Id.ToString()).ToArray()));
    }
    public int? PingMs;GameStart gameStart;string modListDirectory;
    public readonly Report Report=new Report();System.Threading.Timer diagnosticTimer;
    TcpListener listener;TcpClient peer,peerWrite;X509Certificate2 cert;Gateway mapping,mappingUdp;Firewall firewall;UdpClient internetGame;
    // The relay (Relay.cs): a TURN allocation for the game datagrams and the room
    // where guests who cannot reach this box directly arrive. Two players only.
    TurnClient turn;WebSocketStream relayHost;
    public bool RelayReady {get;private set;}
    // On the public list (relay-worker's directory) or not. Only a two-player
    // salon with its relay room open can be public; the room leaves the list by
    // itself once a guest is admitted, since the room's socket closes then.
    public PublicState Public {get;private set;}
    // Counts the relay's answers, so the window can follow one it did not ask for.
    public int PublicNotices {get;private set;}
    public bool CanBePublic {get{return Host && maxPlayers==2 && RelayReady && relayHost!=null && !admitted && !disposed;}}
    public void SetPublic(bool on) {
        WebSocketStream control;lock(this) control=relayHost;
        if(!CanBePublic || control==null) throw new IOException(L.T("Seul un salon à 2 joueurs, encore en attente, peut être public."));
        var profile=Profile;
        var status=RoomStatus();if(on)lastStatus=status.Key;
        control.SendText(on?Relay.PublishRequest(Invite,profile.Name,profile.Language,disc!=null && disc.Pal,build,profile.DiscHash,status):"{\"type\":\"unpublish\"}");
        Public=on?PublicState.Pending:PublicState.Private;connected();
    }
    void PublicNotice(Dictionary<string,object> notice) {
        object type,reason;notice.TryGetValue("type",out type);notice.TryGetValue("reason",out reason);
        switch(type as string) {
            case "published": Public=PublicState.Listed;Report.Write("public=listed");status("Room publique : tous les joueurs qui ont le même disque la voient dans « Rooms publiques » (lobby et CubeShelf).");break;
            case "unpublished": Public=PublicState.Private;Report.Write("public=private");status("La room n'est plus publique. Tes amis peuvent toujours la rejoindre avec l'invitation.");break;
            case "publish-refused": Public=PublicState.Private;Report.Write("public=refused reason="+(reason as string));status((reason as string)=="full"?"La liste des rooms publiques est pleine pour l'instant. Réessayez dans quelques minutes.":"Le relais a refusé de rendre la room publique. Recréez-la et réessayez.");break;
            default: return;
        }
        PublicNotices++;connected();
    }
    // Test only: the host behaves as if its box refused, without touching the
    // router or the Windows firewall, and the guest goes straight to the relay.
    static bool RelayOnly {get{return Environment.GetEnvironmentVariable("PARTYBOARD_ONLINE_RELAY_ONLY")=="1";}}
    // Test only: neither the Windows firewall nor the router is touched.
    static bool NoSystemChanges {get{return RelayOnly || Environment.GetEnvironmentVariable("PARTYBOARD_ONLINE_NO_SYSTEM_CHANGES")=="1";}}
    // On this network only: no box, no relay, nothing leaves the house. The
    // salon is found through LanDiscovery instead of an invitation.
    public bool Lan {get;set;}LanDiscovery.Beacon beacon;
    public int LanAnswers {get{var b=beacon;return b==null?0:b.Answered;}}
    // The allocation a relayed session's game runs on, for diagnostics.
    TurnClient relayAllocation;
    public int RelayRefreshes {get{var a=relayAllocation;return a==null?0:a.Refreshes;}}
    public int RelayRefreshFailures {get{var a=relayAllocation;return a==null?0:a.RefreshFailures;}}
    public string RelayStatus {get{var a=relayAllocation;return a==null?"relay=none":"relay=turn refreshes="+a.Refreshes+" failures="+a.RefreshFailures;}}
    // One guest's two channels, from either path, gathered by Offer.
    readonly object admitLock=new object();readonly IDisposable[] admitClosers=new IDisposable[2];readonly SslStream[] admitStreams=new SslStream[2];
    byte[] admitPairing;bool admitRelay;volatile bool admitted;
    // The mesh's own port mapping, distinct from the pair above: mapping/
    // mappingUdp/firewall/internetGame exist only for the two-player TCP+UDP
    // combo a host offers today. A mesh needs exactly one more UDP mapping,
    // usable by a guest as much as a host, since every seat has to be
    // dialable directly. Not opened by anything yet -- see OpenMesh.
    Gateway meshMapping;Firewall meshFirewall;MeshRelay meshRelay;
    // Two players keeps the classic single Bridge above, untouched. Above two,
    // Bridge stays null and every guest gets its own control-only leg here
    // instead, indexed by seat (1..maxPlayers-1); Lobby's send delegate routes
    // through this array rather than through a lone peer.
    int maxPlayers=2;Bridge[] guestLinks;
    public int MaxPlayers {get{return maxPlayers;}}
    readonly CancellationTokenSource cancel=new CancellationTokenSource();
    readonly Action<string> status;readonly Action connected;readonly Action<string> failed;
    byte[] build; volatile bool disposed;public Process Game;
    public Session(Action<string> s,Action c,Action<string> f,PlayerInfo profile,DiscFile verifiedDisc) {status=s;connected=()=>{c();RoomChanged();};failed=f;initialProfile=profile;disc=verifiedDisc;}
    // The room as everyone else sees it: the directory (when public) and the
    // launcher that opened this window (RoomSync).
    public Relay.RoomStatus RoomStatus() {
        var status=new Relay.RoomStatus{Game=disc!=null && disc.Pal?"GMPP01":"GMPE01",MaxPlayers=maxPlayers,Mods=Profile.Mods,Title=RoomName??""};
        var lobby=Lobby;
        if(lobby!=null && (lobby.Phase==LobbyPhase.Preparing || lobby.Phase==LobbyPhase.Running)){status.State="playing";status.Activity=Activity??"";}
        else if(lobby!=null && lobby.Occupied>=maxPlayers)status.State="full";
        if(lobby==null)status.Players.Add(Profile.Name);
        else for(int seat=0;seat<Lobby.MaxSeats;seat++){var info=lobby.SeatInfo(seat);if(info!=null)status.Players.Add(info.Name);}
        return status;
    }
    string lastStatus;
    // The room's name on the public list, at most 32 characters (the relay cleans it again).
    public string RoomName {get{return roomName;}set{roomName=value==null?null:new string(value.Where(c=>!char.IsControl(c)).Take(32).ToArray()).Trim();}}
    string roomName;
    // What the game says is happening, as the launcher reads it from the game (CubeShelf's
    // in-game bridge): a board and turn, a minigame. Sent with the room's status while it plays.
    public string Activity {get{return activity;}set{var clean=value==null?null:new string(value.Where(c=>!char.IsControl(c)).Take(120).ToArray()).Trim();if(clean==activity)return;activity=clean;RoomChanged();}}
    string activity;
    // Tells the directory when the room changed: who is in it, its mods, or
    // that it plays. Only a listed room has anything to tell.
    void RoomChanged() {
        if(Public!=PublicState.Listed)return;
        WebSocketStream control;lock(this)control=relayHost;if(control==null)return;
        Relay.RoomStatus now;try{now=RoomStatus();}catch{return;}
        var key=now.Key;if(key==lastStatus)return;lastStatus=key;
        Task.Run(()=>{try{control.SendText(Relay.StatusRequest(now));}catch(Exception e){Report.Write("public_status_failed error="+e.GetType().Name);}});
    }
    // players==2 is byte for byte what Create() always did: the box maps one
    // UDP port too, and Bridge relays the single guest's game data directly.
    // Above two, there is no "the guest" to relay for -- every seat, this
    // host's own included, reaches the others through MeshRelay instead, so
    // the UDP mapping below is skipped and OpenMesh() opens its own later.
    // No opening was possible: the salon is reachable only from this network.
    public bool LocalOnly {get{return mapping!=null && mapping.LocalOnly;}}
    public void Create(int players=2) {
        if(players<2 || players>Lobby.MaxSeats)throw new IOException(L.T("Nombre de joueurs invalide."));
        maxPlayers=players;
        Host=true;status(L.T("Vérification de votre connexion…"));var route=Route.Detect();
        build=Wire.BuildHash(AppDomain.CurrentDomain.BaseDirectory);
        NetworkSettings.Load();
        cert=Wire.Certificate();
        // The game's UDP socket takes the same number as the TCP listener. An
        // automatic port is TCP's choice, and Windows may reserve that number
        // for UDP alone (Hyper-V, WSL and Docker exclude whole UDP ranges):
        // then another port is drawn, rather than failing a salon at random.
        var local=route.Local;
        var pair=OpenPorts(NetworkSettings.Port,()=>new TcpListener(local,NetworkSettings.Port),
            players==2?(Func<int,UdpClient>)(p=>new UdpClient(new IPEndPoint(local,p))):null,
            p=>Report.Write("event=udp_port_reserved port="+p));
        listener=pair.Item1;internetGame=pair.Item2;int port=((IPEndPoint)listener.LocalEndpoint).Port;
        try {
            status(L.T("Autorisez PartyBoard si Windows vous le demande…")); firewall=NoSystemChanges?null:Firewall.Open(port,Lan);
            if(disposed) throw new OperationCanceledException();
            bool boxed=!RelayOnly && !Lan;
            if(boxed)status(NetworkSettings.Port==0?L.T("Préparation automatique de votre box…"):L.T("Préparation du port fixe ")+port+"…");
            mapping=boxed?Gateway.OpenFor(route,port,port,false):Gateway.LocalFor(route,port,false);
            if(players==2) {
                mappingUdp=boxed?Gateway.OpenFor(route,port,mapping.Port,true):Gateway.LocalFor(route,port,true);
                if(mappingUdp.Port!=mapping.Port || !mappingUdp.Address.Equals(mapping.Address))throw new IOException(L.T("La box n'a pas pu réserver le même accès rapide pour le jeu. Inversez les rôles et réessayez."));
            }
            if(disposed) throw new OperationCanceledException();
            // route.Local is what the listener and the game UDP socket are
            // already bound to, so a guest on the same network can reach them
            // without the invitation changing anything on this side.
            Invite=new Invitation{Address=mapping.Address,Port=mapping.Port,Expires=DateTime.UtcNow.AddMinutes(30),Fingerprint=Wire.Hash(cert.RawData).Take(16).ToArray(),Token=Wire.Random(16),Build=build,MaxPlayers=players,
                LocalAddress=Gateway.Private(route.Local)?route.Local:IPAddress.Any,LocalPort=Gateway.Private(route.Local)?port:0};
            // A guest that cannot reach this box -- a CGNAT, no UPnP, or a box that
            // said yes and still drops -- comes through the relay instead.
            if(players==2 && Relay.Available && !Lan) OpenRelay();
            if(Lan) {
                try{beacon=new LanDiscovery.Beacon(build,disc.Hash,DescribeForLan);}
                catch(SocketException e){throw new IOException(L.T("Le port ")+LanDiscovery.Port+L.T(" de recherche des parties LAN est occupé (")+e.SocketErrorCode+L.T("). Fermez l'autre programme qui l'utilise, puis réessayez."),e);}
            }
            if(players>2) {
                // Built now, not from the first guest's AttachLobby(): a mesh
                // salon has to exist, and announce this seat's own mesh
                // address, before anyone has connected to admit into it.
                guestLinks=new Bridge[players];
                Lobby=new Lobby(true,Profile,SendToSeat,id=>Task.Run(()=>LoadGame(id)),id=>Report.Write("commit="+id),connected);
                Report.Write("role=host mode=mesh build="+BitConverter.ToString(build).Replace("-","")+" players="+players);
                OpenMesh();
                lock(this) {if(!disposed) diagnosticTimer=new System.Threading.Timer(_=>Report.Write("phase="+Lobby.Phase+" occupied="+Lobby.Occupied+"/"+players+" disk_match="+Lobby.DiscMatches),null,0,5000);}
                Lobby.Announce();connected();
            }
            Task.Run(()=>Maintain());
            Task.Run(()=>Accept());
        } catch {Dispose();throw;}
    }
    // Routes a Lobby send through the right guest link(s) instead of through a
    // lone Bridge field. Never called with seat 0: a host's own Lobby never
    // addresses itself, exactly as in the two-player send delegate this
    // replaces.
    void SendToSeat(int seat,byte[] payload) {
        var links=guestLinks;if(links==null)return;
        if(seat==Lobby.Broadcast) {for(int s=1;s<links.Length;s++) {var link=links[s];if(link!=null)try{link.SendControl(payload);}catch{}}}
        else if(seat>=1 && seat<links.Length && links[seat]!=null)links[seat].SendControl(payload);
    }
    async Task Maintain() {
        try {
            while(!cancel.IsCancellationRequested) {
                await Task.Delay(TimeSpan.FromSeconds(Math.Max(5,mapping.Lifetime/2)),cancel.Token);
                if(Bridge==null && DateTime.UtcNow>Invite.Expires) throw new IOException(L.T("L'invitation a expiré. Cliquez sur Créer une partie pour recommencer."));
                lock(this) {if(disposed) return;mapping.Renew();mappingUdp.Renew();if(mappingUdp.Port!=mapping.Port || !mappingUdp.Address.Equals(mapping.Address))throw new IOException(L.T("La connexion de la box a changé."));}
            }
        } catch(OperationCanceledException) {} catch {if(!disposed) {failed(L.T("La connexion temporaire a été interrompue. Recréez une partie."));Dispose();}}
    }
    void Accept() {
        if(maxPlayers>2) {AcceptMesh();return;}
        try {
            int attempts=0;var rate=Stopwatch.StartNew();
            while(!disposed && !admitted) {
                if(rate.Elapsed.TotalMinutes>=1) {attempts=0;rate.Restart();}
                if(attempts>=6) {if(cancel.Token.WaitHandle.WaitOne(1000)) return;continue;}
                var client=listener.AcceptTcpClient();attempts++;
                client.NoDelay=true;client.ReceiveTimeout=8000;client.SendTimeout=8000;
                Offer(client.GetStream(),client,false);
            }
        } catch {if(!disposed && !admitted) {failed(L.T("L'attente a été interrompue. Recréez une partie."));Dispose();}}
    }
    // One channel of the guest, over TCP or through the relay. Both channels must
    // come from the same guest -- otherwise the two halves of one bridge could
    // belong to two different machines -- and by the same path, so the game
    // datagrams go where the control did.
    void Offer(Stream transport,IDisposable closer,bool viaRelay) {
        SslStream ssl;byte channel;byte[] clientId;
        try {ssl=Wire.Server(transport,cert,Invite,build,out channel,out clientId);}
        catch {try{closer.Dispose();}catch{}if(!disposed && !admitted) status(L.T("En attente de votre ami…"));return;}
        lock(admitLock) {
            bool sameGuest=admitPairing==null || (Wire.Equal(admitPairing,clientId) && admitRelay==viaRelay);
            if(admitted || disposed || admitStreams[channel]!=null || !sameGuest) {ssl.Dispose();try{closer.Dispose();}catch{}return;}
            admitPairing=clientId;admitRelay=viaRelay;admitClosers[channel]=closer;admitStreams[channel]=ssl;
            if(!viaRelay){if(channel==0)peer=(TcpClient)closer;else peerWrite=(TcpClient)closer;}
            if(admitStreams[0]==null || admitStreams[1]==null) {status(L.T("Premier canal sécurisé. Préparation du second…"));return;}
            admitted=true;
        }
        try{listener.Stop();}catch{}
        int gamePort;using(var reserve=new UdpClient(new IPEndPoint(IPAddress.Loopback,0))) gamePort=((IPEndPoint)reserve.Client.LocalEndPoint).Port;
        HostGamePort=gamePort;
        IDatagramSocket network;
        lock(this) {
            // Whichever path is left unused is released now; the room's own
            // socket too, which never carried a guest's bytes.
            if(viaRelay){network=turn;relayAllocation=turn;turn=null;if(internetGame!=null){internetGame.Close();internetGame=null;}}
            else{network=new UdpDatagramSocket(internetGame);internetGame=null;if(turn!=null){turn.Dispose();turn=null;}}
            // A public room keeps its socket while it plays: that is what keeps
            // it on the list, as a game under way. A private one has no use for it.
            if(relayHost!=null && Public!=PublicState.Listed){relayHost.Dispose();relayHost=null;Public=PublicState.Private;PublicNotices++;}
        }
        JoinPath=viaRelay?"relay":"direct";
        Bridge=new Bridge(admitClosers[0],admitStreams[0],admitClosers[1],admitStreams[1],0,network,null,Invite.Token,gamePort);AttachLobby();
    }
    // Credentials, a TURN allocation and the room, before the invitation is
    // shown. Any failure leaves the salon exactly as it was without a relay.
    // The salon's TCP listener and, for two players, the game's UDP socket on
    // the same number. An automatic port is TCP's choice, and Windows may
    // reserve that number for UDP alone (Hyper-V, WSL and Docker exclude whole
    // UDP ranges): another port is drawn then, up to twenty times, rather than
    // failing a salon at random. A fixed port has no other choice and says so.
    internal static Tuple<TcpListener,UdpClient> OpenPorts(int fixedPort,Func<TcpListener> listen,Func<int,UdpClient> bindUdp,Action<int> reserved) {
        string busy=L.T("Le port fixe ")+fixedPort+L.T(" est déjà utilisé par un autre programme. Choisissez-en un autre dans Port fixe….");
        for(int attempt=0;;attempt++) {
            var listener=listen();
            try {listener.Start(4);}
            catch(SocketException) {if(fixedPort==0)throw;throw new IOException(busy);}
            int port=((IPEndPoint)listener.LocalEndpoint).Port;
            if(bindUdp==null) return Tuple.Create(listener,(UdpClient)null);
            try {return Tuple.Create(listener,bindUdp(port));}
            catch(SocketException e) when (e.SocketErrorCode==SocketError.AccessDenied || e.SocketErrorCode==SocketError.AddressAlreadyInUse) {
                listener.Stop();
                if(fixedPort!=0)throw new IOException(busy);
                if(attempt>=20)throw;
                reserved(port);
            }
        }
    }
    // What a LAN question is answered with, while there is still a seat.
    LanDiscovery.Game DescribeForLan() {
        if(disposed || Invite==null)return null;
        int occupied;
        if(maxPlayers==2){if(admitted)return null;occupied=1;}
        else{var lobby=Lobby;if(lobby==null || lobby.Phase!=LobbyPhase.Waiting || lobby.Occupied>=maxPlayers)return null;occupied=lobby.Occupied;}
        var profile=Profile;
        return new LanDiscovery.Game{Name=profile.Name,Language=GameLanguage.Code(profile.Language),Region=disc!=null && disc.Pal?"eu":"us",Players=occupied,MaxPlayers=maxPlayers,Invite=Invite.Encode()};
    }
    void OpenRelay() {
        TurnClient allocation=null;
        try {
            status(L.T("Préparation du relais en ligne…"));
            var access=Relay.TestTurn??Relay.TurnCredentials();
            allocation=new TurnClient(access.Item3,access.Item1,access.Item2);allocation.Allocate();
            var room=Relay.RoomId(Invite.Token);
            // The key keeps the room ours: a guest knows the room from the
            // invitation, and a public room's invitation is on the list.
            var key=BitConverter.ToString(Wire.Random(16)).Replace("-","").ToLowerInvariant();
            var control=Relay.Connect("/room/"+room+"/host?udp="+Uri.EscapeDataString(allocation.Relayed.ToString())+"&key="+key);
            lock(this){if(disposed){allocation.Dispose();control.Dispose();return;}turn=allocation;relayHost=control;RelayReady=true;}
            Report.Write("relay=ready");
            var held=allocation;Task.Run(()=>RelayNotices(room,control,held));
        } catch(Exception e) {if(allocation!=null)allocation.Dispose();Report.Write("relay=unavailable error="+e.GetType().Name);}
    }
    void RelayNotices(string room,WebSocketStream control,TurnClient allocation) {
        Relay.HostNotices(room,control,allocation,()=>disposed || (admitted && Public!=PublicState.Listed),pipe=>Offer(pipe,pipe,true),Report.Write,PublicNotice,()=>!admitted);
    }
    // The N-guest loop: keeps accepting until every seat but the host's own is
    // filled, pairing each guest's two channels by the client identifier the
    // handshake carries (ChannelPairing, tested without a socket in Tests.cs)
    // rather than the single {clients,streams,pairing} trio the two-player
    // loop above uses for its one guest. Each completed pairing becomes one
    // control-only Bridge -- game data never touches it, MeshRelay carries
    // that -- addressed by the next free seat.
    void AcceptMesh() {
        try {
            int attempts=0;var rate=Stopwatch.StartNew();
            var pairing=new ChannelPairing<Tuple<TcpClient,SslStream>>();int seated=0;
            while(!disposed) {
                if(rate.Elapsed.TotalMinutes>=1) {attempts=0;rate.Restart();}
                if(attempts>=6) {if(cancel.Token.WaitHandle.WaitOne(1000)) return;continue;}
                var client=listener.AcceptTcpClient();attempts++;
                SslStream ssl;byte channel;byte[] clientId;Tuple<TcpClient,SslStream>[] completed=null;
                try {
                    ssl=Wire.Server(client,cert,Invite,build,out channel,out clientId);
                    completed=pairing.Add(clientId,channel,Tuple.Create(client,ssl));
                } catch {client.Close();if(!disposed) status(L.T("En attente de votre ami…"));continue;}
                if(disposed) {client.Close();return;}
                if(completed==null) {status(L.T("Un ami rejoint le salon. Préparation du second canal…"));continue;}
                int seat=0;for(int s=1;s<maxPlayers;s++)if(guestLinks[s]==null){seat=s;break;}
                if(seat==0) {completed[0].Item1.Close();completed[1].Item1.Close();continue;}
                // No game port, no internet peer: this Bridge only ever carries the
                // salon's TLS control messages. It still needs a UdpClient to
                // satisfy the constructor and Dispose(), so a throwaway loopback
                // socket stands in for the game/Internet ones the two-player
                // Bridge above actually uses.
                var placeholder=new UdpClient(new IPEndPoint(IPAddress.Loopback,0));
                var link=new Bridge(completed[0].Item1,completed[0].Item2,completed[1].Item1,completed[1].Item2,0,placeholder,null,Invite.Token,0,seat);
                guestLinks[seat]=link;Lobby.Admit(seat);link.Control=packet=>Lobby.Receive(seat,packet);
                WatchGuest(seat,link);seated++;
                if(seated>=maxPlayers-1) {status(L.T("Salon complet."));listener.Stop();return;}
                status(L.T("En attente d'un autre invité…"));
            }
        } catch {if(!disposed) {failed(L.T("L'attente a été interrompue. Recréez une partie."));Dispose();}}
    }
    // One guest's control link failing (or simply ending) frees only its own
    // seat -- unlike the two-player Watch() below, where losing the one
    // Bridge ends the whole session, because with three or four players a
    // single departure must not end the salon for everyone still in it.
    void WatchGuest(int seat,Bridge link) {
        Task.Run(async ()=> {
            try {await link.RunControlOnly();}
            catch(IOException e){Report.Write("guest_seat="+seat+" transport_error="+e.GetBaseException().GetType().Name);}
            catch(Exception e){Report.Write("guest_seat="+seat+" transport_error="+e.GetType().Name);}
            finally {
                lock(this) {if(!disposed && guestLinks!=null && seat<guestLinks.Length && guestLinks[seat]==link)guestLinks[seat]=null;}
                if(!disposed) try{Lobby.Leave(seat);}catch{}
                try{link.Dispose();}catch{}
            }
        });
    }
    public int HostGamePort;
    // "lan" or "internet" - which route actually carried this session. A name,
    // never an address: the diagnostic must stay free of endpoints.
    public string JoinPath="listening";
    public void Join(string invitation) {
        Host=false;Invite=Invitation.Decode(invitation);maxPlayers=Invite.MaxPlayers;
        build=Wire.BuildHash(AppDomain.CurrentDomain.BaseDirectory);
        if(Invite.Build!=null && !Wire.Equal(Invite.Build,build)) throw new IOException(L.T("Les versions sont différentes. Copiez le même dossier PartyBoard sur les deux PC."));
        status(L.T("Connexion à votre ami…"));
        // One identity for this guest, used on both of its channels, so the host can
        // pair them and seat us. Random rather than derived from anything: it only has
        // to be unique among the guests of one salon.
        var clientId=Wire.Random(16);
        // A LAN game's invitation names this network only: no relay behind it.
        bool lanGame=Gateway.Private(Invite.Address) && Invite.Address.Equals(Invite.LocalAddress) && Invite.Port==Invite.LocalPort;
        if(lanGame) {
            status(L.T("Connexion à la partie sur votre réseau…"));peer=new TcpClient(AddressFamily.InterNetwork);
            bool reachedLan=false;try{reachedLan=peer.ConnectAsync(Invite.Address,Invite.Port).Wait(5000) && peer.Connected;}catch{}
            if(!reachedLan){peer.Close();peer=null;throw new IOException(L.T("Cette partie n'est plus joignable sur votre réseau. Vérifiez que l'hôte a laissé sa fenêtre ouverte, puis actualisez la liste."));}
            var lanHost=Wire.Client(peer,Invite,build,0,clientId);JoinPath="lan";
            status(L.T("Premier canal sécurisé. Préparation du second…"));peerWrite=new TcpClient(AddressFamily.InterNetwork);
            if(!peerWrite.ConnectAsync(Invite.Address,Invite.Port).Wait(10000)){peerWrite.Close();lanHost.Dispose();throw new IOException(L.T("Le second canal n'a pas pu être créé."));}
            var lanFrom=Wire.Client(peerWrite,Invite,build,1,clientId);if(disposed){lanHost.Dispose();lanFrom.Dispose();throw new OperationCanceledException();}
            internetGame=new UdpClient(new IPEndPoint(IPAddress.Any,0));
            Bridge=new Bridge(peerWrite,lanFrom,peer,lanHost,1,internetGame,new IPEndPoint(Invite.Address,Invite.Port),Invite.Token);internetGame=null;AttachLobby();
            if(maxPlayers>2) {
                var baseReceive=Bridge.Control;bool meshOpened=false;
                Bridge.Control=packet=>{
                    baseReceive(packet);
                    if(!meshOpened) {meshOpened=true;try{OpenMesh();}catch(Exception e){Report.Write("event=mesh_open_failed error="+e.Message);}}
                };
            }
            return;
        }
        // A public room's invitation has no address: the relay is the only way in.
        if(Invite.ThroughRelayOnly) {
            if(!Relay.Available)throw new IOException(L.T("Les rooms publiques passent par le relais en ligne, qui est désactivé sur ce PC."));
            JoinViaRelay(clientId);return;
        }
        if(RelayOnly && Relay.Available && maxPlayers==2) {JoinViaRelay(clientId);return;}
        // Try the host's own network first. Two seconds, because an address that
        // is not on this network refuses or times out fast, and the public
        // address is still there behind it. Reaching the wrong machine cannot
        // succeed: Wire.Client pins the certificate fingerprint carried by the
        // invitation, so a stranger at that address fails the handshake and we
        // fall through exactly as if nothing had answered.
        var target=Invite.Address;var targetPort=Invite.Port;SslStream toHost=null;
        if(Invite.HasLocalPath) {
            status(L.T("Recherche de votre ami sur votre réseau…"));
            var lan=new TcpClient(AddressFamily.InterNetwork);
            try {
                if(lan.ConnectAsync(Invite.LocalAddress,Invite.LocalPort).Wait(2000) && lan.Connected) {
                    toHost=Wire.Client(lan,Invite,build,0,clientId);
                    peer=lan;target=Invite.LocalAddress;targetPort=Invite.LocalPort;JoinPath="lan";
                }
            } catch {}
            if(toHost==null) {try{lan.Close();}catch{}}
        }
        if(toHost==null) {
            status(L.T("Connexion à votre ami…"));peer=new TcpClient(AddressFamily.InterNetwork);
            // Only an unreachable host falls through to the relay. A host that
            // answers and refuses (another version, an expired invitation) says
            // why, and the relay would only say it again.
            bool reached=false;
            try{reached=peer.ConnectAsync(Invite.Address,Invite.Port).Wait(Relay.Available?6000:10000) && peer.Connected;}catch{}
            if(!reached) {
                peer.Close();peer=null;
                if(!Relay.Available || maxPlayers>2) throw new IOException(L.T("Votre ami n'est pas joignable. Vérifiez qu'il a laissé sa fenêtre ouverte, ou essayez d'inverser les rôles."));
                JoinViaRelay(clientId);return;
            }
            toHost=Wire.Client(peer,Invite,build,0,clientId);target=Invite.Address;targetPort=Invite.Port;JoinPath="internet";
        }
        status(L.T("Premier canal sécurisé. Préparation du second…"));peerWrite=new TcpClient(AddressFamily.InterNetwork);
        var task=peerWrite.ConnectAsync(target,targetPort);
        if(!task.Wait(10000)){peerWrite.Close();throw new IOException(L.T("Le second canal n'a pas pu être créé."));}
        var fromHost=Wire.Client(peerWrite,Invite,build,1,clientId);if(disposed){toHost.Dispose();fromHost.Dispose();throw new OperationCanceledException();}
        internetGame=new UdpClient(new IPEndPoint(IPAddress.Any,0));
        Bridge=new Bridge(peerWrite,fromHost,peer,toHost,1,internetGame,new IPEndPoint(target,targetPort),Invite.Token);internetGame=null;AttachLobby();
        if(maxPlayers>2) {
            // LocalSeat is still its default (1) at this instant: the host's
            // SeatCommand -- always the first message Admit() queues, ahead of
            // even the host's own profile -- has not been read yet, because
            // Watch() has not started the control pump that reads it. Opening
            // the mesh under the wrong seat would announce the wrong address
            // for it, so this waits for that first Receive() to run and settle
            // LocalSeat before calling OpenMesh() exactly once.
            var baseReceive=Bridge.Control;bool meshOpened=false;
            Bridge.Control=packet=>{
                baseReceive(packet);
                if(!meshOpened) {meshOpened=true;try{OpenMesh();}catch(Exception e){Report.Write("event=mesh_open_failed error="+e.Message);}}
            };
        }
    }
    // Both channels through the relay's room, the game datagrams to the host's
    // TURN address it announced there.
    void JoinViaRelay(byte[] clientId) {
        status(Invite.ThroughRelayOnly?L.T("Connexion à la room publique par le relais en ligne…"):L.T("Votre ami passe par le relais en ligne…"));
        var room=Relay.RoomId(Invite.Token);
        internetGame=new UdpClient(new IPEndPoint(IPAddress.Any,0));
        var self=Relay.PublicAddress(internetGame);
        IPEndPoint hostUdp;WebSocketStream first=Relay.GuestPipe(room,self,out hostUdp),second=null;SslStream toHost,fromHost;
        try {
            toHost=Wire.Client(first,Invite,build,0,clientId);
            status(L.T("Premier canal sécurisé. Préparation du second…"));
            IPEndPoint again;second=Relay.GuestPipe(room,self,out again);
            fromHost=Wire.Client(second,Invite,build,1,clientId);
        } catch {first.Dispose();if(second!=null)second.Dispose();throw;}
        if(disposed){toHost.Dispose();fromHost.Dispose();throw new OperationCanceledException();}
        if(hostUdp==null){toHost.Dispose();fromHost.Dispose();throw new IOException(L.T("Le relais n'a pas donné l'adresse de jeu de votre ami."));}
        JoinPath="relay";
        Bridge=new Bridge(second,fromHost,first,toHost,1,internetGame,hostUdp,Invite.Token);internetGame=null;AttachLobby();
    }
    void AttachLobby() {
        Bridge.Diagnostic=Report.Write;
        Report.Write("role="+(Host?"host":"guest")+" build="+BitConverter.ToString(build).Replace("-","")+" tls=connected game_transport=udp-authenticated path="+JoinPath);
        Lobby=new Lobby(Host,Profile,(seat,payload)=>Bridge.SendControl(payload),id=>Task.Run(()=>LoadGame(id)),id=>{Report.Write("commit="+id);Bridge.CommitGame();gameStart.Commit(id);},connected);
        Bridge.Control=Lobby.Receive;Bridge.Ping=value=>{PingMs=value;connected();};
        Watch();Lobby.Announce();connected();
        lock(this) {if(!disposed) diagnosticTimer=new System.Threading.Timer(_=>Report.Write("phase="+Lobby.Phase+" disk_match="+Lobby.DiscMatches+" ping_ms="+PingMs+" udp_ready="+Bridge.UdpReady+" local_packets="+Bridge.GamePackets+" peer_packets="+Bridge.PeerPackets+" "+Bridge.TransportStatus),null,0,5000);}
    }
    // Maps this player's own mesh port and announces it through the salon.
    // Any player can call this, host or guest, since a mesh needs every seat
    // dialable, not just the host's -- the same Gateway/Firewall dance
    // Create() already does for its own single port, standing alone because a
    // guest has no TCP listener to piggyback the mapping onto.
    //
    // Not called by anything yet. Session.Accept() still takes exactly one
    // guest, so there is no third or fourth seat for a mesh to reach, and
    // calling this today would map a port and announce an address that
    // nothing downstream uses -- a firewall prompt and a wasted mapping for
    // every ordinary two-player session. It waits for the accept loop that
    // actually seats more than one guest.
    //
    // What is verified here: the wiring from a learned endpoint to
    // MeshRelay.AddPeer (MeshWiringTest, in-memory, no socket). What is not:
    // the live Gateway/Firewall exchange against a real router, which no test
    // in this repository can reach.
    public void OpenMesh() {
        if(Lobby==null)throw new IOException(L.T("Le salon doit être prêt avant d'ouvrir la mise en réseau directe."));
        var route=Route.Detect();
        NetworkSettings.Load();
        UdpClient socket;
        try {socket=new UdpClient(new IPEndPoint(route.Local,NetworkSettings.MeshPort));}
        catch(SocketException) {if(NetworkSettings.MeshPort==0)throw;throw new IOException(L.T("Le port fixe ")+NetworkSettings.MeshPort+L.T(" (jeu à 3 ou 4) est déjà utilisé par un autre programme. Choisissez un autre port fixe."));}
        int port=((IPEndPoint)socket.Client.LocalEndPoint).Port;
        try {
            bool lanMesh=Lan || (Invite!=null && Gateway.Private(Invite.Address));
            meshFirewall=NoSystemChanges?null:Firewall.Open(port);
            if(disposed)throw new OperationCanceledException();
            meshMapping=lanMesh || NoSystemChanges?Gateway.LocalFor(route,port,true):Gateway.OpenFor(route,port,port,true);
            if(disposed)throw new OperationCanceledException();
            meshRelay=new MeshRelay(Lobby.LocalSeat,Invite.Token,socket){Diagnostic=Report.Write};
            var wiring=new MeshWiring(meshRelay,Lobby.LocalSeat);
            Lobby.EndpointLearned+=wiring.OnEndpointLearned;
            Task.Run(()=>meshRelay.Run());
            Task.Run(()=>MaintainMesh());
            Lobby.AnnounceEndpoint(new IPEndPoint(meshMapping.Address,meshMapping.Port));
        } catch {socket.Close();meshFirewall?.Dispose();meshMapping=null;throw;}
    }
    async Task MaintainMesh() {
        try {
            while(!cancel.IsCancellationRequested) {
                await Task.Delay(TimeSpan.FromSeconds(Math.Max(5,meshMapping.Lifetime/2)),cancel.Token);
                lock(this) {if(disposed)return;meshMapping.Renew();}
            }
        } catch(OperationCanceledException) {} catch {if(!disposed)Report.Write("event=mesh_mapping_lost");}
    }
    void Watch() {
        // Above two players this guest's own link to the host never carries
        // game data either -- MeshRelay does, once OpenMesh() is under way --
        // so it runs control-only, exactly like every guest link the host
        // itself holds in AcceptMesh().
        Task.Run(async ()=> {string reason="Votre ami s'est déconnecté. Recréez une connexion.";try {await (maxPlayers>2?Bridge.RunControlOnly():Bridge.Run());}catch(IOException e){Report.Write("transport_error="+e.GetBaseException().GetType().Name+" hresult="+e.GetBaseException().HResult);reason=e.Message;}catch(Exception e){Report.Write("transport_error="+e.GetType().Name);}finally {if(!disposed) {failed(reason);Dispose();}}});
    }
    public void Launch() {
        if(Lobby==null || disposed)throw new IOException(L.T("Attendez que votre ami soit connecté."));
        if(!Bridge.UdpReady)throw new IOException(L.T("Le canal rapide du jeu se prépare encore. Attendez deux secondes puis réessayez."));
        Lobby.Start();
    }
    // Not the diagnostics directory: Report is documented as never holding a file
    // path, and a mod list is nothing but file paths. This one is ours, and it goes
    // when the session does.
    string ModListDirectory() {
        lock(this) {
            if(modListDirectory==null)modListDirectory=Path.Combine(Path.GetTempPath(),"PartyBoardOnline-mods-"+Guid.NewGuid().ToString("N"));
            return modListDirectory;
        }
    }
    void LoadGame(Guid attempt) {
        try {
        Report.Write("loading="+attempt);
        if(disc==null || !Wire.Equal(disc.Hash,Profile.DiscHash))throw new IOException(L.T("Le disque n'est pas vérifié."));
        cancel.Token.ThrowIfCancellationRequested();
        gameStart=new GameStart(attempt);
        cancel.Token.ThrowIfCancellationRequested();
        string args;
        if(maxPlayers>2) {
            if(meshRelay==null)throw new IOException(L.T("La mise en réseau directe n'est pas prête."));
            int ownPort;using(var reserve=new UdpClient(new IPEndPoint(IPAddress.Loopback,0)))ownPort=((IPEndPoint)reserve.Client.LocalEndPoint).Port;
            var peers=new List<Tuple<int,int>>();
            for(int seat=0;seat<maxPlayers;seat++)
                if(seat!=Lobby.LocalSeat && Lobby.SeatInfo(seat)!=null)peers.Add(Tuple.Create(seat,meshRelay.LoopbackPort(seat)));
            args=GameStart.MeshArguments(ownPort,Lobby.LocalSeat,maxPlayers,peers);
        } else {
            args=Host ? "--netplay-host "+HostGamePort : "--netplay-join 127.0.0.1:"+Bridge.LocalPort;
        }
        args=GameStart.OnlineArguments(args);
        // A European disc loads its text in the language chosen; every seat takes the
        // host's, or the games would differ from their first tick. The game also
        // refuses a peer in another language at its handshake.
        if(disc.Pal) {
            var hostInfo=Lobby.SeatInfo(0);
            var language=GameLanguage.Code(hostInfo!=null?hostInfo.Language:Profile.Language);
            args+=" --lang "+language;Report.Write("game_language="+language);
        }
        Report.Write("netplay_mode=lockstep input_delay_frames=3");
        // Write the list that was announced, not the one CubeShelf happens to have on
        // disk: the salon launches the game itself, so its copy can be stale, and a
        // load order that differs from the announced one desyncs the session.
        string modList=Profile.Mods.None?null:Profile.Mods.WriteListFile(ModListDirectory());
        gameStart.Launch(args,disc.Path,false,Report.NativePath,modList);Game=gameStart.Process;
        gameStart.WaitReady(cancel.Token);Report.Write("native_ready="+attempt);Lobby.Loaded(attempt);
        Game.WaitForExit();Report.Write("native_exit="+Game.ExitCode);
        // Announce the departure while the control channel is still up. Dispose() closes
        // the sockets, so anything said after it is said to nobody -- which is precisely
        // how the other player used to be left alone in a salon that never emptied.
        if(Lobby!=null)Lobby.LocalGameExited();
        // The window is reset by failed(), so it has to be called even when the session
        // is already disposed. It was guarded by !disposed, which meant that anything
        // tearing the session down during the game -- a lost control channel, an expired
        // port mapping -- left the player looking at a salon still listing both of them
        // after their own game had closed. failed() is idempotent: it does nothing once
        // the form has moved on to another session.
        failed(L.T("La partie est terminée. Recréez un salon pour rejouer."));
        if(!disposed)Dispose();
        }catch(OperationCanceledException){}catch(Exception e){if(!disposed){failed(e is IOException?e.Message:L.T("Le lancement a échoué. Fermez le salon puis réessayez."));Dispose();}}
        finally {if(gameStart!=null)gameStart.Dispose();}
    }
    public void Dispose() {
        lock(this) {
            if(!disposed)Report.Write("session_closed");
            if(modListDirectory!=null)try{Directory.Delete(modListDirectory,true);}catch{}if(diagnosticTimer!=null)diagnosticTimer.Dispose();
            disposed=true;cancel.Cancel();if(listener!=null) listener.Stop();if(peer!=null) peer.Close();if(peerWrite!=null)peerWrite.Close();
            if(turn!=null){turn.Dispose();turn=null;}if(relayHost!=null){relayHost.Dispose();relayHost=null;}
            if(beacon!=null){beacon.Dispose();beacon=null;}
            if(gameStart!=null)gameStart.Abort();if(Lobby!=null)Lobby.Close();
            if(Bridge!=null) Bridge.Dispose();if(guestLinks!=null)foreach(var g in guestLinks)if(g!=null)try{g.Dispose();}catch{}
            if(internetGame!=null)internetGame.Close();if(mappingUdp!=null)mappingUdp.Dispose();if(mapping!=null) mapping.Dispose();if(firewall!=null) firewall.Dispose();
            if(meshRelay!=null)meshRelay.Dispose();if(meshMapping!=null)meshMapping.Dispose();if(meshFirewall!=null)meshFirewall.Dispose();
            // The game is never killed: it stops through its existing watchdog.
            if(cert!=null) cert.Dispose();
        }
    }
}

static class Program {
    [STAThread] static int Main(string[] args) {
        L.Init(Environment.GetEnvironmentVariable);
        // The tests read the French messages, whatever the language of Windows.
        if(args.Length>=1 && args[0].Contains("test")) L.English=false;
        if((args.Length==3 || (args.Length==4 && args[3]=="lan")) && args[0]=="--firewall") {int port,pid;return int.TryParse(args[1],out port)&&int.TryParse(args[2],out pid)?Firewall.Broker(port,pid,args.Length==4):2;}
        if(args.Length==1 && args[0]=="--self-test") return Tests.Run();
        if(args.Length==1 && args[0]=="--self-test-relay") return Tests.RunRelay();
        if(args.Length==1 && args[0]=="--public-room-test") return Tests.RunPublicRoom();
        if(args.Length==1 && args[0]=="--lan-test") return Tests.RunLan();
        if(args.Length==1 && args[0]=="--room-tags") return RoomSync.PrintTags();
        int relayMinutes;if(args.Length==2 && args[0]=="--relay-session-test" && int.TryParse(args[1],out relayMinutes)) return Tests.RunRelaySession(relayMinutes);
        // Both PCs must hold byte-identical folders or Lobby refuses to start the
        // game. Printing the same hash the handshake uses lets a player check that
        // before a session instead of discovering it as a refusal.
        if(args.Length==1 && args[0]=="--build-hash") {
            try {
                var root=AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar,Path.AltDirectorySeparatorChar);
                Console.WriteLine(BitConverter.ToString(Wire.BuildHash(root)).Replace("-","").ToLowerInvariant());
                return 0;
            } catch(Exception e) {Console.Error.WriteLine(e.Message);return 1;}
        }
        if(args.Length==1 && args[0]=="--verify-disc") {
            try {using(var disc=DiscFile.Verify(Environment.GetEnvironmentVariable("PARTYBOARD_ONLINE_DISC"),_=>{},CancellationToken.None))Console.WriteLine("PASS: disque "+(disc.Pal?L.T("européen"):"USA Rev 1")+L.T(" compatible, SHA-256 complet calculé, verrou de lecture actif."));return 0;}
            catch(Exception e){Console.Error.WriteLine(e.Message);return 3;}
        }
        if(args.Length==2 && (args[0]=="--render-ui" || args[0]=="--render-lobby-preview")) {
            Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
            using(var form=new MainForm()) {form.ShowInTaskbar=false;form.StartPosition=FormStartPosition.Manual;form.Location=new Point(-32000,-32000);form.Show();if(args[0]=="--render-lobby-preview")form.PreviewLobby();form.PerformLayout();Application.DoEvents();using(var bitmap=new Bitmap(form.Width,form.Height)) {form.DrawToBitmap(bitmap,new Rectangle(0,0,form.Width,form.Height));bitmap.Save(args[1],System.Drawing.Imaging.ImageFormat.Png);}}
            return 0;
        }
        if(args.Length==1 && args[0]=="--diagnose") {try{var r=Route.Detect();Console.WriteLine(L.T("Connexion physique détectée. Aucun port ouvert."));return 0;}catch(Exception e){Console.WriteLine(e.Message);return 1;}}
        GameLanguage.Current=GameLanguage.FromArguments(args);
        Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
        // --host / --join let a launcher open the companion on a lobby instead of on an empty
        // form. Everything it needs beyond the intent travels in the environment; see Startup.cs.
        Application.Run(new MainForm(Startup.FromEnvironment(args)));return 0;
    }
}
}



