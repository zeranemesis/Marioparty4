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
    public PlayerInfo(string name,byte[] hash=null,long length=0) {
        Name=CleanName(name);if(hash!=null && (hash.Length!=32 || length<=0)) throw new IOException("Information disque incorrecte.");
        DiscHash=hash==null?null:(byte[])hash.Clone();DiscLength=hash==null?0:length;
    }
    public static string CleanName(string name) {
        name=(name??"").Trim();if(name.Length<1 || name.Length>24 || name.Any(c=>char.IsControl(c) || char.IsSurrogate(c)))
            throw new IOException("Choisissez un pseudo de 1 à 24 caractères, sans retour à la ligne.");
        return name;
    }
    public bool SameDisc(PlayerInfo other) {return other!=null && DiscHash!=null && other.DiscHash!=null && DiscLength==other.DiscLength && Wire.Equal(DiscHash,other.DiscHash);}
    public byte[] Encode() {
        using(var m=new MemoryStream()) using(var w=new BinaryWriter(m)) {
            byte[] name=Encoding.UTF8.GetBytes(Name);w.Write((byte)2);w.Write((byte)name.Length);w.Write(name);w.Write(DiscHash!=null);
            if(DiscHash!=null){w.Write(DiscLength);w.Write(DiscHash);}return m.ToArray();
        }
    }
    public static PlayerInfo Decode(byte[] data) {
        try {
            using(var r=new BinaryReader(new MemoryStream(data))) {
                if(r.ReadByte()!=2) throw new IOException();int n=r.ReadByte();if(n<1 || n>96) throw new IOException();
                string name=new UTF8Encoding(false,true).GetString(r.ReadBytes(n));byte present=r.ReadByte();if(present>1)throw new IOException();
                long length=present==1?r.ReadInt64():0;byte[] hash=present==1?r.ReadBytes(32):null;
                if(r.BaseStream.Position!=data.Length)throw new IOException();return new PlayerInfo(name,hash,length);
            }
        }catch{throw new IOException("Informations du joueur incompatibles.");}
    }
}

enum LobbyPhase { Waiting, Preparing, Running, Closed }
// Role checks live in the protocol state machine, not just the disabled button.
sealed class Lobby {
    readonly Action<byte[]> send;readonly Action<Guid> prepare,commit;readonly Action changed;
    public readonly bool Host;public PlayerInfo Local {get;private set;} public PlayerInfo Remote {get;private set;}
    public LobbyPhase Phase {get;private set;}
    Guid attempt;bool localReady,remoteReady;
    public Lobby(bool host,PlayerInfo local,Action<byte[]> sender,Action<Guid> loader,Action<Guid> starter,Action refresh) {
        Host=host;Local=local;send=sender;prepare=loader;commit=starter;changed=refresh;
    }
    public bool CanStart {get{lock(this)return Host && Phase==LobbyPhase.Waiting && Local.SameDisc(Remote);}}
    public bool DiscMatches {get{lock(this)return Local.SameDisc(Remote);}}
    public void Announce(){lock(this){if(Phase==LobbyPhase.Waiting)send(Local.Encode());}}
    public void Update(PlayerInfo local) {lock(this){if(Phase!=LobbyPhase.Waiting)throw new IOException("Le lancement est déjà en cours.");Local=local;send(Local.Encode());changed();}}
    internal static byte[] Command(byte type,Guid id){return new[]{type}.Concat(id.ToByteArray()).ToArray();}
    public void Start() {
        lock(this) {
            if(!CanStart)throw new IOException(Host?"Les deux joueurs doivent avoir exactement le même fichier disque.":"Seul l'hôte peut lancer la partie.");
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
                    if(Host || Phase!=LobbyPhase.Waiting || !Local.SameDisc(Remote) || id==Guid.Empty)throw new IOException("Demande de lancement non autorisée.");
                    attempt=id;Phase=LobbyPhase.Preparing;localReady=remoteReady=false;prepare(attempt);break;
                case 4:
                    if(!Host || Phase!=LobbyPhase.Preparing || id!=attempt || remoteReady)throw new IOException("Confirmation de chargement inattendue.");
                    remoteReady=true;TryCommit();break;
                case 5:
                    if(Host || Phase!=LobbyPhase.Preparing || id!=attempt || !localReady)throw new IOException("Départ de partie non autorisé.");
                    Phase=LobbyPhase.Running;commit(attempt);break;
                default:throw new IOException("Commande de salon inconnue.");
            }
            changed();
        }
    }
    public void Close(){lock(this){Phase=LobbyPhase.Closed;changed();}}
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
    public void Launch(string arguments,string path,bool probe=false,string diagnosticPath=null) {
        var root=AppDomain.CurrentDomain.BaseDirectory;
        var info=new ProcessStartInfo(System.IO.Path.Combine(root,"partyboard.exe"),arguments+(probe?" --netplay-start-probe --netplay-pad-probe":"")){WorkingDirectory=root,UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=probe,RedirectStandardError=probe};
        Process=ChildProcess.Start(info,
            "PARTYBOARD_ONLINE_DISC",path,
            "PARTYBOARD_ONLINE_READY",prefix+"-ready",
            "PARTYBOARD_ONLINE_GO",prefix+"-go",
            "PARTYBOARD_ONLINE_CANCEL",prefix+"-cancel",
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
