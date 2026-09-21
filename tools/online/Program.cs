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
    public static Firewall Open(int port) {
        int pid=Process.GetCurrentProcess().Id;
        var result=new Firewall{stop=new EventWaitHandle(false,EventResetMode.ManualReset,Name(pid,port,"-stop"))};
        try {
            using(var ready=new EventWaitHandle(false,EventResetMode.ManualReset,Name(pid,port,"-ready"))) {
                var info=new ProcessStartInfo(Application.ExecutablePath,"--firewall "+port+" "+pid){UseShellExecute=true,Verb="runas",WindowStyle=ProcessWindowStyle.Hidden};
                using(var p=Process.Start(info)) {
                    if(!ready.WaitOne(15000)) throw new IOException("Windows n'a pas autorisé la connexion. Réessayez et acceptez sa demande d'autorisation.");
                    if(p.HasExited) throw new IOException("Windows n'a pas pu préparer la connexion.");
                }
            }
            return result;
        } catch {result.Dispose();throw;}
    }
    public static int Broker(int port,int pid) {
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
                    datagram.ApplicationName=Application.ExecutablePath;datagram.Protocol=17;datagram.LocalPorts=port.ToString();
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

sealed class Session : IDisposable {
    public Invitation Invite; public Bridge Bridge; public bool Host;
    public Lobby Lobby;public readonly PlayerInfo Profile;readonly DiscFile disc;
    public int? PingMs;GameStart gameStart;string modListDirectory;
    public readonly Report Report=new Report();System.Threading.Timer diagnosticTimer;
    TcpListener listener;TcpClient peer,peerWrite;X509Certificate2 cert;Gateway mapping,mappingUdp;Firewall firewall;UdpClient internetGame;
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
    public Session(Action<string> s,Action c,Action<string> f,PlayerInfo profile,DiscFile verifiedDisc) {status=s;connected=c;failed=f;Profile=profile;disc=verifiedDisc;}
    // players==2 is byte for byte what Create() always did: the box maps one
    // UDP port too, and Bridge relays the single guest's game data directly.
    // Above two, there is no "the guest" to relay for -- every seat, this
    // host's own included, reaches the others through MeshRelay instead, so
    // the UDP mapping below is skipped and OpenMesh() opens its own later.
    public void Create(int players=2) {
        if(players<2 || players>Lobby.MaxSeats)throw new IOException("Nombre de joueurs invalide.");
        maxPlayers=players;
        Host=true;status("Vérification de votre connexion…");var route=Route.Detect();
        build=Wire.BuildHash(AppDomain.CurrentDomain.BaseDirectory);
        cert=Wire.Certificate(); listener=new TcpListener(route.Local,0);listener.Start(4);
        int port=((IPEndPoint)listener.LocalEndpoint).Port;
        try {
            if(players==2) internetGame=new UdpClient(new IPEndPoint(route.Local,port));
            status("Autorisez PartyBoard si Windows vous le demande…"); firewall=Firewall.Open(port);
            if(disposed) throw new OperationCanceledException();
            status("Préparation automatique de votre box…"); mapping=new Gateway(route,port);mapping.Open();
            if(players==2) {
                mappingUdp=new Gateway(route,port,mapping.Port,true);mappingUdp.Open();
                if(mappingUdp.Port!=mapping.Port || !mappingUdp.Address.Equals(mapping.Address))throw new IOException("La box n'a pas pu réserver le même accès rapide pour le jeu. Inversez les rôles et réessayez.");
            }
            if(disposed) throw new OperationCanceledException();
            // route.Local is what the listener and the game UDP socket are
            // already bound to, so a guest on the same network can reach them
            // without the invitation changing anything on this side.
            Invite=new Invitation{Address=mapping.Address,Port=mapping.Port,Expires=DateTime.UtcNow.AddMinutes(30),Fingerprint=Wire.Hash(cert.RawData).Take(16).ToArray(),Token=Wire.Random(16),Build=build,MaxPlayers=players,
                LocalAddress=Gateway.Private(route.Local)?route.Local:IPAddress.Any,LocalPort=Gateway.Private(route.Local)?port:0};
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
                if(Bridge==null && DateTime.UtcNow>Invite.Expires) throw new IOException("L'invitation a expiré. Cliquez sur Créer une partie pour recommencer.");
                lock(this) {if(disposed) return;mapping.Renew();mappingUdp.Renew();if(mappingUdp.Port!=mapping.Port || !mappingUdp.Address.Equals(mapping.Address))throw new IOException("La connexion de la box a changé.");}
            }
        } catch(OperationCanceledException) {} catch {if(!disposed) {failed("La connexion temporaire a été interrompue. Recréez une partie.");Dispose();}}
    }
    void Accept() {
        if(maxPlayers>2) {AcceptMesh();return;}
        try {
            int attempts=0;var rate=Stopwatch.StartNew();var clients=new TcpClient[2];var streams=new SslStream[2];
            // Which guest these two channels belong to. One salon, one guest for now --
            // the star that seats three needs a table of these, and the identity to key
            // it by is exactly what the handshake now carries. Holding it here, unused
            // beyond pairing, is what makes that table a change of shape rather than a
            // change of protocol.
            byte[] pairing=null;
            while(!disposed) {
                if(rate.Elapsed.TotalMinutes>=1) {attempts=0;rate.Restart();}
                if(attempts>=6) {if(cancel.Token.WaitHandle.WaitOne(1000)) return;continue;}
                var client=listener.AcceptTcpClient();attempts++;
                SslStream ssl;byte channel;byte[] clientId;
                try {
                    ssl=Wire.Server(client,cert,Invite,build,out channel,out clientId);
                    if(streams[channel]!=null)throw new IOException("Canal déjà connecté.");
                    // Both channels must come from the same guest. Without this the two
                    // halves of one bridge could belong to two different machines.
                    if(pairing!=null && !Wire.Equal(pairing,clientId))throw new IOException("Deux invités différents sur un même salon.");
                } catch {client.Close();if(!disposed) status("En attente de votre ami…");continue;}
                pairing=clientId;
                if(disposed) {client.Close();return;}
                clients[channel]=client;streams[channel]=ssl;peer=clients[0];peerWrite=clients[1];
                if(streams[0]==null || streams[1]==null){status("Premier canal sécurisé. Préparation du second…");continue;}
                listener.Stop();
                int gamePort;using(var reserve=new UdpClient(new IPEndPoint(IPAddress.Loopback,0))) gamePort=((IPEndPoint)reserve.Client.LocalEndPoint).Port;
                HostGamePort=gamePort;Bridge=new Bridge(clients[0],streams[0],clients[1],streams[1],0,internetGame,null,Invite.Token,gamePort);internetGame=null;AttachLobby();return;
            }
        } catch {if(!disposed) {failed("L'attente a été interrompue. Recréez une partie.");Dispose();}}
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
                } catch {client.Close();if(!disposed) status("En attente de votre ami…");continue;}
                if(disposed) {client.Close();return;}
                if(completed==null) {status("Un ami rejoint le salon. Préparation du second canal…");continue;}
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
                if(seated>=maxPlayers-1) {status("Salon complet.");listener.Stop();return;}
                status("En attente d'un autre invité…");
            }
        } catch {if(!disposed) {failed("L'attente a été interrompue. Recréez une partie.");Dispose();}}
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
        if(Invite.Build!=null && !Wire.Equal(Invite.Build,build)) throw new IOException("Les versions sont différentes. Copiez le même dossier PartyBoard sur les deux PC.");
        status("Connexion à votre ami…");
        // One identity for this guest, used on both of its channels, so the host can
        // pair them and seat us. Random rather than derived from anything: it only has
        // to be unique among the guests of one salon.
        var clientId=Wire.Random(16);
        // Try the host's own network first. Two seconds, because an address that
        // is not on this network refuses or times out fast, and the public
        // address is still there behind it. Reaching the wrong machine cannot
        // succeed: Wire.Client pins the certificate fingerprint carried by the
        // invitation, so a stranger at that address fails the handshake and we
        // fall through exactly as if nothing had answered.
        var target=Invite.Address;var targetPort=Invite.Port;SslStream toHost=null;
        if(Invite.HasLocalPath) {
            status("Recherche de votre ami sur votre réseau…");
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
            status("Connexion à votre ami…");peer=new TcpClient(AddressFamily.InterNetwork);
            if(!peer.ConnectAsync(Invite.Address,Invite.Port).Wait(10000)) {peer.Close();throw new IOException("Votre ami n'est pas joignable. Vérifiez qu'il a laissé sa fenêtre ouverte, ou essayez d'inverser les rôles.");}
            toHost=Wire.Client(peer,Invite,build,0,clientId);target=Invite.Address;targetPort=Invite.Port;JoinPath="internet";
        }
        status("Premier canal sécurisé. Préparation du second…");peerWrite=new TcpClient(AddressFamily.InterNetwork);
        var task=peerWrite.ConnectAsync(target,targetPort);
        if(!task.Wait(10000)){peerWrite.Close();throw new IOException("Le second canal n'a pas pu être créé.");}
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
        if(Lobby==null)throw new IOException("Le salon doit être prêt avant d'ouvrir la mise en réseau directe.");
        var route=Route.Detect();
        var socket=new UdpClient(new IPEndPoint(route.Local,0));
        int port=((IPEndPoint)socket.Client.LocalEndPoint).Port;
        try {
            meshFirewall=Firewall.Open(port);
            if(disposed)throw new OperationCanceledException();
            meshMapping=new Gateway(route,port,port,true);meshMapping.Open();
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
        if(Lobby==null || disposed)throw new IOException("Attendez que votre ami soit connecté.");
        if(!Bridge.UdpReady)throw new IOException("Le canal rapide du jeu se prépare encore. Attendez deux secondes puis réessayez.");
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
        if(disc==null || !Wire.Equal(disc.Hash,Profile.DiscHash))throw new IOException("Le disque n'est pas vérifié.");
        cancel.Token.ThrowIfCancellationRequested();
        gameStart=new GameStart(attempt);
        cancel.Token.ThrowIfCancellationRequested();
        string args;
        if(maxPlayers>2) {
            if(meshRelay==null)throw new IOException("La mise en réseau directe n'est pas prête.");
            int ownPort;using(var reserve=new UdpClient(new IPEndPoint(IPAddress.Loopback,0)))ownPort=((IPEndPoint)reserve.Client.LocalEndPoint).Port;
            var peers=new List<Tuple<int,int>>();
            for(int seat=0;seat<maxPlayers;seat++)
                if(seat!=Lobby.LocalSeat && Lobby.SeatInfo(seat)!=null)peers.Add(Tuple.Create(seat,meshRelay.LoopbackPort(seat)));
            args=GameStart.MeshArguments(ownPort,Lobby.LocalSeat,maxPlayers,peers);
        } else {
            args=Host ? "--netplay-host "+HostGamePort : "--netplay-join 127.0.0.1:"+Bridge.LocalPort;
        }
        args=GameStart.OnlineArguments(args);
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
        failed("La partie est terminée. Recréez un salon pour rejouer.");
        if(!disposed)Dispose();
        }catch(OperationCanceledException){}catch(Exception e){if(!disposed){failed(e is IOException?e.Message:"Le lancement a échoué. Fermez le salon puis réessayez.");Dispose();}}
        finally {if(gameStart!=null)gameStart.Dispose();}
    }
    public void Dispose() {
        lock(this) {
            if(!disposed)Report.Write("session_closed");
            if(modListDirectory!=null)try{Directory.Delete(modListDirectory,true);}catch{}if(diagnosticTimer!=null)diagnosticTimer.Dispose();
            disposed=true;cancel.Cancel();if(listener!=null) listener.Stop();if(peer!=null) peer.Close();if(peerWrite!=null)peerWrite.Close();
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
        if(args.Length==3 && args[0]=="--firewall") {int port,pid;return int.TryParse(args[1],out port)&&int.TryParse(args[2],out pid)?Firewall.Broker(port,pid):2;}
        if(args.Length==1 && args[0]=="--self-test") return Tests.Run();
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
            try {using(var disc=DiscFile.Verify(Environment.GetEnvironmentVariable("PARTYBOARD_ONLINE_DISC"),_=>{},CancellationToken.None))Console.WriteLine("PASS: disque USA Rev 1 compatible, SHA-256 complet calculé, verrou de lecture actif.");return 0;}
            catch(Exception e){Console.Error.WriteLine(e.Message);return 3;}
        }
        if(args.Length==2 && (args[0]=="--render-ui" || args[0]=="--render-lobby-preview")) {
            Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
            using(var form=new MainForm()) {form.ShowInTaskbar=false;form.StartPosition=FormStartPosition.Manual;form.Location=new Point(-32000,-32000);form.Show();if(args[0]=="--render-lobby-preview")form.PreviewLobby();form.PerformLayout();Application.DoEvents();using(var bitmap=new Bitmap(form.Width,form.Height)) {form.DrawToBitmap(bitmap,new Rectangle(0,0,form.Width,form.Height));bitmap.Save(args[1],System.Drawing.Imaging.ImageFormat.Png);}}
            return 0;
        }
        if(args.Length==1 && args[0]=="--diagnose") {try{var r=Route.Detect();Console.WriteLine("Connexion physique détectée. Aucun port ouvert.");return 0;}catch(Exception e){Console.WriteLine(e.Message);return 1;}}
        Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new MainForm());return 0;
    }
}
}



