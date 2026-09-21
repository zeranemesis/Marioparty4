using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Net;
using System.Text;
using System.Security.Cryptography;
using System.Threading;
using System.Diagnostics;

namespace PartyBoardOnline {
static class ChildProcess {
    static readonly object environmentLock=new object();
    public static Process Start(ProcessStartInfo info,params string[] variables) {
        if(variables.Length%2!=0)throw new ArgumentException("Environment variables must be name/value pairs.");
        lock(environmentLock) {
            var previous=new string[variables.Length/2];
            try {
                for(int i=0;i<variables.Length;i+=2) {
                    previous[i/2]=Environment.GetEnvironmentVariable(variables[i]);
                    Environment.SetEnvironmentVariable(variables[i],variables[i+1]);
                }
                return Process.Start(info);
            } finally {
                for(int i=0;i<variables.Length;i+=2)Environment.SetEnvironmentVariable(variables[i],previous[i/2]);
            }
        }
    }
}

sealed class DiscFile : IDisposable {
    readonly FileStream file;
    public string Path {get;private set;}
    public byte[] Hash {get;private set;}
    public long Length {get;private set;}
    DiscFile(string path) {
        Path=System.IO.Path.GetFullPath(path);
        // Keep the verified file open until the session ends. Windows refuses
        // writes and replacement, including between verification and launch.
        file=new FileStream(Path,FileMode.Open,FileAccess.Read,FileShare.Read,1024*1024,FileOptions.SequentialScan);
        Length=file.Length;if(Length==0){file.Dispose();throw new IOException("Ce fichier disque est vide.");}
    }
    public static DiscFile Verify(string path,Action<int> progress,CancellationToken token,bool inspect=true) {
        var disc=new DiscFile(path);
        try {
            if(inspect) {
                var root=AppDomain.CurrentDomain.BaseDirectory;
                var info=new ProcessStartInfo(System.IO.Path.Combine(root,"partyboard.exe"),"--online-disc-check"){WorkingDirectory=root,UseShellExecute=false,CreateNoWindow=true};
                using(var p=ChildProcess.Start(info,"PARTYBOARD_ONLINE_DISC",disc.Path)) {
                    var timer=Stopwatch.StartNew();
                    while(!p.WaitForExit(100)) {
                        if(token.IsCancellationRequested || timer.Elapsed.TotalSeconds>30) {p.Kill();token.ThrowIfCancellationRequested();throw new IOException("La lecture du disque a pris trop de temps.");}
                    }
                    if(p.ExitCode!=0) throw new IOException("Choisissez un disque Mario Party 4 USA, révision 1 (ISO, GCM ou RVZ). Ce fichier n'est pas compatible.");
                }
            }
            using(var sha=SHA256.Create()) {
                var buffer=new byte[1024*1024];long read=0;int n,last=-1;
                while((n=disc.file.Read(buffer,0,buffer.Length))!=0) {
                    token.ThrowIfCancellationRequested();sha.TransformBlock(buffer,0,n,null,0);read+=n;
                    int pct=(int)(read*100/disc.Length);if(pct!=last){last=pct;progress(pct);}
                }
                sha.TransformFinalBlock(new byte[0],0,0);disc.Hash=sha.Hash;
            }
            token.ThrowIfCancellationRequested();return disc;
        } catch {disc.Dispose();throw;}
    }
    public void Dispose(){file.Dispose();}
}

sealed class PlayerInfo {
    public readonly string Name;public readonly byte[] DiscHash;public readonly long DiscLength;
    public readonly ModSet Mods;
    public PlayerInfo(string name,byte[] hash=null,long length=0,ModSet mods=null) {
        Name=CleanName(name);if(hash!=null && (hash.Length!=32 || length<=0)) throw new IOException("Information disque incorrecte.");
        DiscHash=hash==null?null:(byte[])hash.Clone();DiscLength=hash==null?0:length;Mods=mods??ModSet.Empty;
    }
    public static string CleanName(string name) {
        name=(name??"").Trim();if(name.Length<1 || name.Length>24 || name.Any(c=>char.IsControl(c) || char.IsSurrogate(c)))
            throw new IOException("Choisissez un pseudo de 1 à 24 caractères, sans retour à la ligne.");
        return name;
    }
    public bool SameDisc(PlayerInfo other) {return other!=null && DiscHash!=null && other.DiscHash!=null && DiscLength==other.DiscLength && Wire.Equal(DiscHash,other.DiscHash);}
    // The disc decides whether the two players own the same game; the mods decide
    // whether they will read the same bytes out of it. Both have to hold.
    public bool SameMods(PlayerInfo other) {return other!=null && Mods.Same(other.Mods);}
    public byte[] Encode() {
        using(var m=new MemoryStream()) using(var w=new BinaryWriter(m)) {
            byte[] name=Encoding.UTF8.GetBytes(Name);w.Write((byte)2);w.Write((byte)name.Length);w.Write(name);w.Write(DiscHash!=null);
            if(DiscHash!=null){w.Write(DiscLength);w.Write(DiscHash);}
            Mods.Write(w);return m.ToArray();
        }
    }
    public static PlayerInfo Decode(byte[] data) {
        try {
            using(var r=new BinaryReader(new MemoryStream(data))) {
                if(r.ReadByte()!=2) throw new IOException();int n=r.ReadByte();if(n<1 || n>96) throw new IOException();
                string name=new UTF8Encoding(false,true).GetString(r.ReadBytes(n));byte present=r.ReadByte();if(present>1)throw new IOException();
                long length=present==1?r.ReadInt64():0;byte[] hash=present==1?r.ReadBytes(32):null;
                // The mod list is appended after the disc, and Decode has always insisted
                // on consuming the message exactly. A peer built before mods existed
                // therefore rejects this outright instead of reading a short message and
                // believing the other player has none -- loud is the only safe failure
                // here, because a silent one launches a session that will desync.
                var mods=ModSet.Read(r);
                if(r.BaseStream.Position!=data.Length)throw new IOException();return new PlayerInfo(name,hash,length,mods);
            }
        }catch{throw new IOException("Informations du joueur incompatibles.");}
    }
}

enum LobbyPhase { Waiting, Preparing, Running, Closed }
// Why a session needs an explicit ending. Running had no exit transition at all: once
// both games were launched the phase stayed Running forever, and the player who quit
// closed their own window without telling anyone. The other player was left in a salon
// that would never empty -- and RefreshLobby actively reassured them about it, because a
// control channel dropping during a game is normal and tolerated on purpose (the game
// itself keeps running over UDP). The one signal that would have meant "they left" was
// the one the lobby was designed to ignore. So the departure is now said out loud.
enum LobbyEnding { None, LocalGameClosed, RemoteGameClosed }
// Role checks live in the protocol state machine, not just the disabled button.
//
// Seats, not "us and them". The host is always seat 0 and assigns 1..3 to guests
// as they arrive; every rule below is written over the occupied seats rather than
// over a single Remote, because a four-player salon has three of them and "the
// other player" stops naming anybody.
//
// The topology is a star: guests only ever talk to the host, and the host is the
// only one holding the whole roster. That is why sending takes a destination -- a
// guest ignores it and answers the host, while the host either names a guest or
// broadcasts. It is also why the host carries the requirement: every guest
// compares itself to seat 0 and to nobody else, so three guests cannot deadlock
// each other over whose mods are right.
sealed class Lobby {
    public const int MaxSeats = 4;
    public const int Broadcast = -1;

    readonly Action<int,byte[]> send;readonly Action<Guid> prepare,commit;readonly Action changed;
    readonly PlayerInfo[] seats = new PlayerInfo[MaxSeats];
    readonly bool[] ready = new bool[MaxSeats];
    // Where each seat can be reached directly, for the mesh -- distinct from
    // the disc/mods PlayerInfo above, and learned independently of it. A
    // player announces this only once its own port mapping has actually
    // succeeded, so "known" here means "dialable now", not "will be soon".
    readonly IPEndPoint[] endpoints = new IPEndPoint[MaxSeats];
    public readonly bool Host;
    public int LocalSeat {get;private set;}
    public LobbyPhase Phase {get;private set;}
    public LobbyEnding Ending {get;private set;}
    Guid attempt;
    // Fired once per seat, in order, whenever this player learns where a
    // remote seat can be reached -- including catch-up replays for seats it
    // already knew about before the local one joined or was told. The mesh
    // side (not written yet) registers a peer on hearing this rather than
    // polling PeerEndpoint, so a late endpoint update after the initial one
    // (a NAT remapping, say) is not missed.
    public Action<int,IPEndPoint> EndpointLearned;

    public Lobby(bool host,PlayerInfo local,Action<int,byte[]> sender,Action<Guid> loader,Action<Guid> starter,Action refresh) {
        Host=host;send=sender;prepare=loader;commit=starter;changed=refresh;
        LocalSeat=host?0:1;   // a guest keeps 1 until the host tells it otherwise
        seats[LocalSeat]=local;
    }

    public PlayerInfo Local {get{lock(this)return seats[LocalSeat];}}
    // The seat every guest measures itself against, and the only one measuring back.
    public PlayerInfo HostInfo {get{lock(this)return seats[0];}}
    public int Occupied {get{lock(this){int n=0;foreach(var s in seats)if(s!=null)n++;return n;}}}
    public PlayerInfo SeatInfo(int index){lock(this)return index>=0 && index<MaxSeats?seats[index]:null;}
    // Kept for the two-player callers and the window: the other side of a pair.
    public PlayerInfo Remote {get{lock(this){for(int i=0;i<MaxSeats;i++)if(i!=LocalSeat && seats[i]!=null)return seats[i];return null;}}}

    bool AgreesWithHost(int seat) {
        var host=seats[0];var other=seats[seat];
        return host!=null && other!=null && host.SameDisc(other) && host.SameMods(other);
    }
    // Public so a player list can show each occupied seat's own agreement with
    // the host, rather than the lobby-wide ModsMatch/DiscMatches, which says
    // only whether everyone agrees and not which one does not.
    public bool SeatAgrees(int seat){lock(this)return seat>=0 && seat<MaxSeats && AgreesWithHost(seat);}
    // Every occupied seat against seat 0. A guest can only see itself and the host,
    // which is enough: if each guest agrees with the host, all of them agree.
    bool EveryoneAgrees() {
        if(seats[0]==null)return false;
        for(int i=1;i<MaxSeats;i++)if(seats[i]!=null && !AgreesWithHost(i))return false;
        return true;
    }

    public bool CanStart {get{lock(this)return Host && Phase==LobbyPhase.Waiting && Occupied>=2 && EveryoneAgrees();}}
    public bool DiscMatches {get{lock(this){
        if(seats[0]==null || Occupied<2)return false;
        for(int i=1;i<MaxSeats;i++)if(seats[i]!=null && !seats[0].SameDisc(seats[i]))return false;
        return true;
    }}}
    public bool ModsMatch {get{lock(this){
        if(seats[0]==null || Occupied<2)return false;
        for(int i=1;i<MaxSeats;i++)if(seats[i]!=null && !seats[0].SameMods(seats[i]))return false;
        return true;
    }}}
    public ModSet RequiredMods {get{lock(this)return seats[0]!=null?seats[0].Mods:null;}}

    // Said to whoever has to act. A guest is told what it must change; the host is
    // told which guest is out of step, because with three of them "the other player"
    // names nobody.
    public string ModAdvice {get{lock(this){
        if(seats[0]==null)return "";
        if(!Host)return seats[LocalSeat].Mods.Same(seats[0].Mods)?"":seats[LocalSeat].Mods.DifferenceFrom(seats[0].Mods);
        var parts=new List<string>();
        for(int i=1;i<MaxSeats;i++) {
            if(seats[i]==null || seats[i].Mods.Same(seats[0].Mods))continue;
            parts.Add(seats[i].Name+" : "+seats[i].Mods.DifferenceFrom(seats[0].Mods));
        }
        return string.Join(" - ",parts.ToArray());
    }}}

    public void Announce(){lock(this){if(Phase==LobbyPhase.Waiting)send(Broadcast,seats[LocalSeat].Encode());}}
    public void Update(PlayerInfo local) {lock(this){if(Phase!=LobbyPhase.Waiting)throw new IOException("Le lancement est deja en cours.");seats[LocalSeat]=local;send(Broadcast,seats[LocalSeat].Encode());changed();}}
    internal static byte[] Command(byte type,Guid id){return new[]{type}.Concat(id.ToByteArray()).ToArray();}
    // Seat assignment. Only the host sends it, and a guest accepts it only while
    // waiting: a seat moving under a launch would repoint every rule at once.
    internal static byte[] SeatCommand(int seat){return new byte[]{9,(byte)seat};}

    // type(1) + subject seat(1) + IPv4(4) + port(2) = 8 bytes. Fixed shape, like
    // the seat command, because this is protocol plumbing rather than the
    // free-form profile PlayerInfo already owns.
    internal static byte[] EndpointCommand(int subjectSeat,IPEndPoint endpoint) {
        var address=endpoint.Address.GetAddressBytes();
        if(address.Length!=4)throw new IOException("Seules les adresses IPv4 sont prises en charge pour le maillage.");
        var packet=new byte[8];packet[0]=11;packet[1]=(byte)subjectSeat;
        Buffer.BlockCopy(address,0,packet,2,4);
        packet[6]=(byte)(endpoint.Port>>8);packet[7]=(byte)endpoint.Port;
        return packet;
    }
    static bool DecodeEndpoint(byte[] packet,out int subjectSeat,out IPEndPoint endpoint) {
        subjectSeat=-1;endpoint=null;
        if(packet.Length!=8 || packet[0]!=11)return false;
        subjectSeat=packet[1];if(subjectSeat<0 || subjectSeat>=MaxSeats)return false;
        var address=new byte[4];Buffer.BlockCopy(packet,2,address,0,4);
        int port=(packet[6]<<8)|packet[7];if(port<=0 || port>65535)return false;
        endpoint=new IPEndPoint(new IPAddress(address),port);return true;
    }

    public IPEndPoint PeerEndpoint(int seat){lock(this)return seat>=0 && seat<MaxSeats?endpoints[seat]:null;}

    // Announces where OUR OWN seat can be reached. Callable at any phase --
    // unlike the profile, an address can become known (a slow port mapping
    // finishing) or change (a NAT remapping) after the game has already
    // started, and a mesh peer needs to hear that whenever it happens.
    public void AnnounceEndpoint(IPEndPoint endpoint) {
        lock(this) {
            endpoints[LocalSeat]=endpoint;
            send(Host?Broadcast:0,EndpointCommand(LocalSeat,endpoint));
        }
    }

    // info is optional because a new connection has to be told its seat before
    // the host can possibly know its profile -- that profile only exists once
    // the guest announces it, over the very link this seat number identifies.
    // Called with null, this reserves the seat and sends the seat/host-profile/
    // catch-up messages without touching seats[guest]; the guest's own
    // Announce() then fills it in through the ordinary packet[0]==2 path,
    // exactly as ever. Called with a real profile (existing callers), nothing
    // changes.
    public void Admit(int guest,PlayerInfo info=null) {
        lock(this) {
            if(!Host)throw new IOException("Seul l'hote attribue les places.");
            if(guest<1 || guest>=MaxSeats)throw new IOException("Place invalide.");
            if(Phase!=LobbyPhase.Waiting)throw new IOException("Le lancement est deja en cours.");
            if(info!=null)seats[guest]=info;
            send(guest,SeatCommand(guest));send(guest,seats[0].Encode());
            // Catch-up: seats admitted earlier already broadcast their endpoint
            // (or had it relayed to them) before this guest existed to receive
            // it. Without this replay a guest that joined third would never
            // learn the second guest's address at all.
            for(int seat=0;seat<MaxSeats;seat++)
                if(seat!=guest && endpoints[seat]!=null)send(guest,EndpointCommand(seat,endpoints[seat]));
            changed();
        }
    }

    public void Leave(int guest) {
        lock(this) {
            if(guest<1 || guest>=MaxSeats || guest==LocalSeat)return;
            seats[guest]=null;ready[guest]=false;endpoints[guest]=null;changed();
        }
    }

    public void Start() {
        lock(this) {
            if(!CanStart) {
                if(!Host)throw new IOException("Seul l'hote peut lancer la partie.");
                if(Occupied<2)throw new IOException("Il faut au moins deux joueurs.");
                if(!DiscMatches)throw new IOException("Tous les joueurs doivent avoir exactement le meme fichier disque.");
                throw new IOException("Tous les joueurs doivent avoir exactement les memes mods actifs : "+ModAdvice);
            }
            attempt=Guid.NewGuid();Phase=LobbyPhase.Preparing;Array.Clear(ready,0,ready.Length);
            send(Broadcast,Command(3,attempt));prepare(attempt);changed();
        }
    }

    public void Loaded(Guid id) {
        lock(this) {
            if(Phase!=LobbyPhase.Preparing || id!=attempt || ready[LocalSeat])return;
            ready[LocalSeat]=true;
            if(Host) TryCommit();else send(0,Command(4,attempt));changed();
        }
    }

    // Everyone, not both. One silent guest holds the start, which is the same rule
    // as before written over a set rather than over a pair.
    void TryCommit() {
        if(!Host)return;
        for(int i=0;i<MaxSeats;i++)if(seats[i]!=null && !ready[i])return;
        send(Broadcast,Command(5,attempt));Phase=LobbyPhase.Running;commit(attempt);
    }

    // Two-player wiring has exactly one link, so the seat is implied: a host hears
    // from seat 1, a guest from seat 0. Every existing caller keeps working, and a
    // star host names the seat explicitly.
    public void Receive(byte[] packet){Receive(Host?1:0,packet);}
    // from is the seat the message arrived on, which the transport knows and the
    // message does not: a guest cannot claim to be someone else by writing it down.
    public void Receive(int from,byte[] packet) {
        lock(this) {
            if(Phase==LobbyPhase.Closed)throw new IOException("Le salon est ferme.");
            if(from<0 || from>=MaxSeats || from==LocalSeat)throw new IOException("Place inconnue.");
            if(packet.Length>0 && packet[0]==2) {
                if(Phase!=LobbyPhase.Waiting)throw new IOException("Le disque ou le pseudo a change pendant le lancement.");
                seats[from]=PlayerInfo.Decode(packet);changed();return;
            }
            if(packet.Length==2 && packet[0]==9) {
                if(Host)throw new IOException("Un invite n attribue pas les places.");
                if(from!=0)throw new IOException("Seul l hote attribue les places.");
                if(Phase!=LobbyPhase.Waiting)throw new IOException("La place ne peut pas changer pendant le lancement.");
                int seat=packet[1];
                if(seat<1 || seat>=MaxSeats)throw new IOException("Place invalide.");
                if(seat!=LocalSeat){var mine=seats[LocalSeat];seats[LocalSeat]=null;LocalSeat=seat;seats[seat]=mine;}
                changed();return;
            }
            if(packet.Length==8 && packet[0]==11) {
                int subjectSeat;IPEndPoint endpoint;
                if(!DecodeEndpoint(packet,out subjectSeat,out endpoint))throw new IOException("Adresse de pair incorrecte.");
                if(Host) {
                    // Self-announcement only: a guest cannot speak for a seat that
                    // is not its own, since that is exactly the roster the other
                    // guests will trust.
                    if(subjectSeat!=from)throw new IOException("Un joueur ne peut annoncer que sa propre adresse.");
                    endpoints[subjectSeat]=endpoint;
                    for(int seat=1;seat<MaxSeats;seat++)
                        if(seats[seat]!=null && seat!=from)send(seat,packet);
                } else {
                    // A guest only ever hears this from the host (from==0 by
                    // construction, one link), whether it is the host's own
                    // address or a relay of another guest's.
                    endpoints[subjectSeat]=endpoint;
                }
                EndpointLearned?.Invoke(subjectSeat,endpoint);
                changed();return;
            }
            if(packet.Length!=17)throw new IOException("Commande de salon incorrecte.");
            var id=new Guid(packet.Skip(1).ToArray());
            switch(packet[0]) {
                case 3:
                    if(Host || from!=0 || Phase!=LobbyPhase.Waiting || !AgreesWithHost(LocalSeat) || id==Guid.Empty)throw new IOException("Demande de lancement non autorisee.");
                    attempt=id;Phase=LobbyPhase.Preparing;Array.Clear(ready,0,ready.Length);prepare(attempt);break;
                case 4:
                    if(!Host || Phase!=LobbyPhase.Preparing || id!=attempt || ready[from])throw new IOException("Confirmation de chargement inattendue.");
                    ready[from]=true;TryCommit();break;
                case 5:
                    if(Host || from!=0 || Phase!=LobbyPhase.Preparing || id!=attempt || !ready[LocalSeat])throw new IOException("Depart de partie non autorise.");
                    Phase=LobbyPhase.Running;commit(attempt);break;
                case 8:
                    // A player has closed their game. Accepted only for the attempt
                    // actually under way, and only once a launch exists, so it cannot be
                    // used to knock a waiting salon over.
                    if(Phase!=LobbyPhase.Running && Phase!=LobbyPhase.Preparing)throw new IOException("Fin de partie inattendue.");
                    if(id!=attempt)throw new IOException("Fin de partie perimee.");
                    // In a star the host has to relay it: a guest hears only from the
                    // host, so without this the other guests sit in a salon that never
                    // empties -- the very bug this protocol just learned to avoid.
                    if(Host)for(int i=1;i<MaxSeats;i++)if(seats[i]!=null && i!=from){try{send(i,Command(8,attempt));}catch{}}
                    Ending=LobbyEnding.RemoteGameClosed;Phase=LobbyPhase.Closed;break;
                default:throw new IOException("Commande de salon inconnue.");
            }
            changed();
        }
    }

    // Our own game has exited. Tell everyone before anything tears the sockets down,
    // then close. Sending is best-effort by design: if the channel is already gone the
    // session still has to end here, and a throw would only replace one stuck salon
    // with several.
    public void LocalGameExited() {
        lock(this) {
            if(Phase==LobbyPhase.Closed)return;
            var announce=Phase==LobbyPhase.Running || Phase==LobbyPhase.Preparing;
            Ending=LobbyEnding.LocalGameClosed;Phase=LobbyPhase.Closed;
            if(announce){try{send(Broadcast,Command(8,attempt));}catch{}}
            changed();
        }
    }

    public void Close(){lock(this){if(Phase!=LobbyPhase.Closed)Phase=LobbyPhase.Closed;changed();}}
}

// Local process readiness is independent of network readiness. The game signals
// READY only after initialization; GO is released by the authenticated host.
sealed class GameStart : IDisposable {
    // Normal online play stays in lockstep for the whole session. Live
    // rollback remains an explicit native developer option, not a lobby default.
    public static string OnlineArguments(string transportArguments) {
        return transportArguments+" --netplay-loopback --netplay-full --netplay-pad 1 --netplay-delay 3";
    }
    // Above two players there is no single host/join pair left to name: every
    // seat, the salon's host included, reaches every other seat through its
    // own MeshRelay leg. --netplay-host here only binds this seat's own port
    // (peer discovery is harmless and unused -- every real peer arrives
    // through an explicit --netplay-peer instead); ownPort therefore does not
    // need to be anything in particular, only a real one. Kept free of Session,
    // Lobby and every socket type so the string itself is what Tests.cs checks,
    // the same way OnlineArguments already is.
    public static string MeshArguments(int ownPort,int seat,int players,IEnumerable<Tuple<int,int>> peers) {
        if(ownPort<=0 || ownPort>65535)throw new IOException("Port de jeu invalide.");
        if(players<3 || players>Lobby.MaxSeats || seat<0 || seat>=players)throw new IOException("Configuration de maillage invalide.");
        var args="--netplay-host "+ownPort+" --netplay-players "+players+" --netplay-seat "+seat;
        var seen=new bool[players];
        foreach(var peer in peers) {
            int peerSeat=peer.Item1,peerPort=peer.Item2;
            if(peerSeat==seat || peerSeat<0 || peerSeat>=players)throw new IOException("Pair de maillage invalide.");
            if(seen[peerSeat])throw new IOException("Pair de maillage répété.");
            seen[peerSeat]=true;
            args+=" --netplay-peer "+peerSeat+":127.0.0.1:"+peerPort;
        }
        return args;
    }
    readonly EventWaitHandle ready,go,cancel;readonly string prefix;
    public Process Process {get;private set;} public readonly Guid Attempt;
    public GameStart(Guid id){Attempt=id;prefix="Local\\PartyBoardOnlineStart-"+Guid.NewGuid().ToString("N");ready=new EventWaitHandle(false,EventResetMode.ManualReset,prefix+"-ready");go=new EventWaitHandle(false,EventResetMode.ManualReset,prefix+"-go");cancel=new EventWaitHandle(false,EventResetMode.ManualReset,prefix+"-cancel");}
    // modListPath is the list the salon announced and both players agreed on. It is
    // passed explicitly because the game loads mods from PARTYBOARD_MOD_LIST and this
    // launcher never set it: an online game used to run with no mods at all, whatever
    // either player had enabled. A null or empty path keeps that behaviour, which is
    // the right answer when neither side has any.
    public void Launch(string arguments,string path,bool probe=false,string diagnosticPath=null,string modListPath=null) {
        var root=AppDomain.CurrentDomain.BaseDirectory;
        var info=new ProcessStartInfo(System.IO.Path.Combine(root,"partyboard.exe"),arguments+(probe?" --netplay-start-probe --netplay-pad-probe":"")){WorkingDirectory=root,UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=probe,RedirectStandardError=probe};
        Process=ChildProcess.Start(info,
            "PARTYBOARD_ONLINE_DISC",path,
            "PARTYBOARD_ONLINE_READY",prefix+"-ready",
            "PARTYBOARD_ONLINE_GO",prefix+"-go",
            "PARTYBOARD_ONLINE_CANCEL",prefix+"-cancel",
            "PARTYBOARD_MOD_LIST",string.IsNullOrEmpty(modListPath)?null:modListPath,
            "PARTYBOARD_NET_DIAGNOSTIC",diagnosticPath);
    }
    public void WaitReady(CancellationToken token) {
        var timer=Stopwatch.StartNew();
        while(!ready.WaitOne(50)) {
            token.ThrowIfCancellationRequested();
            if(Process.HasExited)throw new IOException("Le jeu s'est fermé pendant le chargement. Vérifiez votre disque et relancez le salon.");
            if(timer.Elapsed.TotalSeconds>=120)throw new IOException("Le jeu n'a pas terminé son chargement après deux minutes.");
        }
        token.ThrowIfCancellationRequested();if(Process.HasExited)throw new IOException("Le jeu s'est fermé avant le départ.");
    }
    public void Commit(Guid id){if(id!=Attempt)throw new IOException("Départ périmé.");go.Set();}
    public void Abort(){try{cancel.Set();}catch(ObjectDisposedException){}}
    public void Dispose(){cancel.Set();ready.Dispose();go.Dispose();cancel.Dispose();}
}
}
