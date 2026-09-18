using System;
using System.IO;
using System.Linq;
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
sealed class Lobby {
    readonly Action<byte[]> send;readonly Action<Guid> prepare,commit;readonly Action changed;
    public readonly bool Host;public PlayerInfo Local {get;private set;} public PlayerInfo Remote {get;private set;}
    public LobbyPhase Phase {get;private set;}
    public LobbyEnding Ending {get;private set;}
    Guid attempt;bool localReady,remoteReady;
    public Lobby(bool host,PlayerInfo local,Action<byte[]> sender,Action<Guid> loader,Action<Guid> starter,Action refresh) {
        Host=host;Local=local;send=sender;prepare=loader;commit=starter;changed=refresh;
    }
    public bool CanStart {get{lock(this)return Host && Phase==LobbyPhase.Waiting && Local.SameDisc(Remote) && Local.SameMods(Remote);}}
    public bool DiscMatches {get{lock(this)return Local.SameDisc(Remote);}}
    public bool ModsMatch {get{lock(this)return Local.SameMods(Remote);}}
    // The host's active list is the requirement: choosing which mods a session runs
    // is done where mods are managed, in CubeShelf, and the salon reports it rather
    // than offering a second place to disagree about it.
    public ModSet RequiredMods {get{lock(this)return Host?Local.Mods:(Remote!=null?Remote.Mods:null);}}
    public string ModAdvice {get{lock(this){
        if(Remote==null)return "";
        var required=Host?Local.Mods:Remote.Mods;
        var mine=Host?Remote.Mods:Local.Mods;      // what the player who must adapt has
        return mine.Same(required)?"":mine.DifferenceFrom(required);
    }}}
    public void Announce(){lock(this){if(Phase==LobbyPhase.Waiting)send(Local.Encode());}}
    public void Update(PlayerInfo local) {lock(this){if(Phase!=LobbyPhase.Waiting)throw new IOException("Le lancement est déjà en cours.");Local=local;send(Local.Encode());changed();}}
    internal static byte[] Command(byte type,Guid id){return new[]{type}.Concat(id.ToByteArray()).ToArray();}
    public void Start() {
        lock(this) {
            if(!CanStart)throw new IOException(!Host?"Seul l'hôte peut lancer la partie.":!Local.SameDisc(Remote)?"Les deux joueurs doivent avoir exactement le même fichier disque.":"Les deux joueurs doivent avoir exactement les mêmes mods actifs : "+ModAdvice);
            attempt=Guid.NewGuid();Phase=LobbyPhase.Preparing;localReady=remoteReady=false;
            send(Command(3,attempt));prepare(attempt);changed();
        }
    }
    public void Loaded(Guid id) {
        lock(this) {
            if(Phase!=LobbyPhase.Preparing || id!=attempt || localReady)return;
            localReady=true;
            if(Host) TryCommit();else send(Command(4,attempt));changed();
        }
    }
    void TryCommit() {
        if(!Host || !localReady || !remoteReady)return;
        send(Command(5,attempt));Phase=LobbyPhase.Running;commit(attempt);
    }
    public void Receive(byte[] packet) {
        lock(this) {
            if(Phase==LobbyPhase.Closed)throw new IOException("Le salon est fermé.");
            if(packet.Length>0 && packet[0]==2) {
                if(Phase!=LobbyPhase.Waiting)throw new IOException("Le disque ou le pseudo a changé pendant le lancement.");
                Remote=PlayerInfo.Decode(packet);changed();return;
            }
            if(packet.Length!=17)throw new IOException("Commande de salon incorrecte.");
            var id=new Guid(packet.Skip(1).ToArray());
            switch(packet[0]) {
                case 3:
                    if(Host || Phase!=LobbyPhase.Waiting || !Local.SameDisc(Remote) || !Local.SameMods(Remote) || id==Guid.Empty)throw new IOException("Demande de lancement non autorisée.");
                    attempt=id;Phase=LobbyPhase.Preparing;localReady=remoteReady=false;prepare(attempt);break;
                case 4:
                    if(!Host || Phase!=LobbyPhase.Preparing || id!=attempt || remoteReady)throw new IOException("Confirmation de chargement inattendue.");
                    remoteReady=true;TryCommit();break;
                case 5:
                    if(Host || Phase!=LobbyPhase.Preparing || id!=attempt || !localReady)throw new IOException("Départ de partie non autorisé.");
                    Phase=LobbyPhase.Running;commit(attempt);break;
                case 8:
                    // The peer's game has closed. Accepted only for the attempt actually
                    // under way, and only once a launch exists, so it cannot be used to
                    // knock a waiting salon over. Id checked like every other command.
                    if(Phase!=LobbyPhase.Running && Phase!=LobbyPhase.Preparing)throw new IOException("Fin de partie inattendue.");
                    if(id!=attempt)throw new IOException("Fin de partie périmée.");
                    Ending=LobbyEnding.RemoteGameClosed;Phase=LobbyPhase.Closed;break;
                default:throw new IOException("Commande de salon inconnue.");
            }
            changed();
        }
    }
    // Our own game has exited. Tell the peer before anything tears the sockets down,
    // then close. Sending is best-effort by design: if the channel is already gone the
    // session still has to end here, and a throw would only replace one stuck salon
    // with two.
    public void LocalGameExited() {
        lock(this) {
            if(Phase==LobbyPhase.Closed)return;
            var announce=Phase==LobbyPhase.Running || Phase==LobbyPhase.Preparing;
            Ending=LobbyEnding.LocalGameClosed;Phase=LobbyPhase.Closed;
            if(announce){try{send(Command(8,attempt));}catch{}}
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
