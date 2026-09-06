using System;
using System.IO;
using System.Linq;
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
    public int? PingMs;GameStart gameStart;
    public readonly Report Report=new Report();System.Threading.Timer diagnosticTimer;
    TcpListener listener;TcpClient peer,peerWrite;X509Certificate2 cert;Gateway mapping,mappingUdp;Firewall firewall;UdpClient internetGame;
    readonly CancellationTokenSource cancel=new CancellationTokenSource();
    readonly Action<string> status;readonly Action connected;readonly Action<string> failed;
    byte[] build; volatile bool disposed;public Process Game;
    public Session(Action<string> s,Action c,Action<string> f,PlayerInfo profile,DiscFile verifiedDisc) {status=s;connected=c;failed=f;Profile=profile;disc=verifiedDisc;}
    public void Create() {
        Host=true;status("Vérification de votre connexion…");var route=Route.Detect();
        build=Wire.BuildHash(AppDomain.CurrentDomain.BaseDirectory);
        cert=Wire.Certificate(); listener=new TcpListener(route.Local,0);listener.Start(4);
        int port=((IPEndPoint)listener.LocalEndpoint).Port;
        try {
            internetGame=new UdpClient(new IPEndPoint(route.Local,port));
            status("Autorisez PartyBoard si Windows vous le demande…"); firewall=Firewall.Open(port);
            if(disposed) throw new OperationCanceledException();
            status("Préparation automatique de votre box…"); mapping=new Gateway(route,port);mapping.Open();
            mappingUdp=new Gateway(route,port,mapping.Port,true);mappingUdp.Open();
            if(mappingUdp.Port!=mapping.Port || !mappingUdp.Address.Equals(mapping.Address))throw new IOException("La box n'a pas pu réserver le même accès rapide pour le jeu. Inversez les rôles et réessayez.");
            if(disposed) throw new OperationCanceledException();
            Invite=new Invitation{Address=mapping.Address,Port=mapping.Port,Expires=DateTime.UtcNow.AddMinutes(30),Fingerprint=Wire.Hash(cert.RawData).Take(16).ToArray(),Token=Wire.Random(16),Build=build};
            Task.Run(()=>Maintain());
            Task.Run(()=>Accept());
        } catch {Dispose();throw;}
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
        try {
            int attempts=0;var rate=Stopwatch.StartNew();var clients=new TcpClient[2];var streams=new SslStream[2];
            while(!disposed) {
                if(rate.Elapsed.TotalMinutes>=1) {attempts=0;rate.Restart();}
                if(attempts>=6) {if(cancel.Token.WaitHandle.WaitOne(1000)) return;continue;}
                var client=listener.AcceptTcpClient();attempts++;
                SslStream ssl;byte channel;
                try {ssl=Wire.Server(client,cert,Invite,build,out channel);if(streams[channel]!=null)throw new IOException("Canal déjà connecté.");} catch {client.Close();if(!disposed) status("En attente de votre ami…");continue;}
                if(disposed) {client.Close();return;}
                clients[channel]=client;streams[channel]=ssl;peer=clients[0];peerWrite=clients[1];
                if(streams[0]==null || streams[1]==null){status("Premier canal sécurisé. Préparation du second…");continue;}
                listener.Stop();
                int gamePort;using(var reserve=new UdpClient(new IPEndPoint(IPAddress.Loopback,0))) gamePort=((IPEndPoint)reserve.Client.LocalEndPoint).Port;
                HostGamePort=gamePort;Bridge=new Bridge(clients[0],streams[0],clients[1],streams[1],0,internetGame,null,Invite.Token,gamePort);internetGame=null;AttachLobby();return;
            }
        } catch {if(!disposed) {failed("L'attente a été interrompue. Recréez une partie.");Dispose();}}
    }
    public int HostGamePort;
    public void Join(string invitation) {
        Host=false;Invite=Invitation.Decode(invitation);
        build=Wire.BuildHash(AppDomain.CurrentDomain.BaseDirectory);
        if(Invite.Build!=null && !Wire.Equal(Invite.Build,build)) throw new IOException("Les versions sont différentes. Copiez le même dossier PartyBoard sur les deux PC.");
        status("Connexion à votre ami…");peer=new TcpClient(AddressFamily.InterNetwork);
        var task=peer.ConnectAsync(Invite.Address,Invite.Port);
        if(!task.Wait(10000)) {peer.Close();throw new IOException("Votre ami n'est pas joignable. Vérifiez qu'il a laissé sa fenêtre ouverte, ou essayez d'inverser les rôles.");}
        var toHost=Wire.Client(peer,Invite,build,0);
        status("Premier canal sécurisé. Préparation du second…");peerWrite=new TcpClient(AddressFamily.InterNetwork);
        task=peerWrite.ConnectAsync(Invite.Address,Invite.Port);
        if(!task.Wait(10000)){peerWrite.Close();throw new IOException("Le second canal n'a pas pu être créé.");}
        var fromHost=Wire.Client(peerWrite,Invite,build,1);if(disposed){toHost.Dispose();fromHost.Dispose();throw new OperationCanceledException();}
        internetGame=new UdpClient(new IPEndPoint(IPAddress.Any,0));
        Bridge=new Bridge(peerWrite,fromHost,peer,toHost,1,internetGame,new IPEndPoint(Invite.Address,Invite.Port),Invite.Token);internetGame=null;AttachLobby();
    }
    void AttachLobby() {
        Bridge.Diagnostic=Report.Write;
        Report.Write("role="+(Host?"host":"guest")+" build="+BitConverter.ToString(build).Replace("-","")+" tls=connected game_transport=udp-authenticated");
        Lobby=new Lobby(Host,Profile,Bridge.SendControl,id=>Task.Run(()=>LoadGame(id)),id=>{Report.Write("commit="+id);Bridge.CommitGame();gameStart.Commit(id);},connected);
        Bridge.Control=Lobby.Receive;Bridge.Ping=value=>{PingMs=value;connected();};
        Watch();Lobby.Announce();connected();
        lock(this) {if(!disposed) diagnosticTimer=new System.Threading.Timer(_=>Report.Write("phase="+Lobby.Phase+" disk_match="+Lobby.DiscMatches+" ping_ms="+PingMs+" udp_ready="+Bridge.UdpReady+" local_packets="+Bridge.GamePackets+" peer_packets="+Bridge.PeerPackets+" "+Bridge.TransportStatus),null,0,5000);}
    }
    void Watch() {
        Task.Run(async ()=> {string reason="Votre ami s'est déconnecté. Recréez une connexion.";try {await Bridge.Run();}catch(IOException e){Report.Write("transport_error="+e.GetBaseException().GetType().Name+" hresult="+e.GetBaseException().HResult);reason=e.Message;}catch(Exception e){Report.Write("transport_error="+e.GetType().Name);}finally {if(!disposed) {failed(reason);Dispose();}}});
    }
    public void Launch() {
        if(Lobby==null || disposed)throw new IOException("Attendez que votre ami soit connecté.");
        if(!Bridge.UdpReady)throw new IOException("Le canal rapide du jeu se prépare encore. Attendez deux secondes puis réessayez.");
        Lobby.Start();
    }
    void LoadGame(Guid attempt) {
        try {
        Report.Write("loading="+attempt);
        if(disc==null || !Wire.Equal(disc.Hash,Profile.DiscHash))throw new IOException("Le disque n'est pas vérifié.");
        cancel.Token.ThrowIfCancellationRequested();
        gameStart=new GameStart(attempt);
        cancel.Token.ThrowIfCancellationRequested();
        string args=Host ? "--netplay-host "+HostGamePort : "--netplay-join 127.0.0.1:"+Bridge.LocalPort;
        args+=" --netplay-loopback --netplay-full --netplay-rollback --netplay-pad 1 --netplay-delay 3";
        gameStart.Launch(args,disc.Path,false,Report.NativePath);Game=gameStart.Process;
        gameStart.WaitReady(cancel.Token);Report.Write("native_ready="+attempt);Lobby.Loaded(attempt);
        Game.WaitForExit();Report.Write("native_exit="+Game.ExitCode);if(!disposed){failed("La partie est terminée. Recréez un salon pour rejouer.");Dispose();}
        }catch(OperationCanceledException){}catch(Exception e){if(!disposed){failed(e is IOException?e.Message:"Le lancement a échoué. Fermez le salon puis réessayez.");Dispose();}}
        finally {if(gameStart!=null)gameStart.Dispose();}
    }
    public void Dispose() {
        lock(this) {
            if(!disposed)Report.Write("session_closed");if(diagnosticTimer!=null)diagnosticTimer.Dispose();
            disposed=true;cancel.Cancel();if(listener!=null) listener.Stop();if(peer!=null) peer.Close();if(peerWrite!=null)peerWrite.Close();
            if(gameStart!=null)gameStart.Abort();if(Lobby!=null)Lobby.Close();
            if(Bridge!=null) Bridge.Dispose();if(internetGame!=null)internetGame.Close();if(mappingUdp!=null)mappingUdp.Dispose();if(mapping!=null) mapping.Dispose();if(firewall!=null) firewall.Dispose();
            // The game is never killed: it stops through its existing watchdog.
            if(cert!=null) cert.Dispose();
        }
    }
}

static class Program {
    [STAThread] static int Main(string[] args) {
        if(args.Length==3 && args[0]=="--firewall") {int port,pid;return int.TryParse(args[1],out port)&&int.TryParse(args[2],out pid)?Firewall.Broker(port,pid):2;}
        if(args.Length==1 && args[0]=="--self-test") return Tests.Run();
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



