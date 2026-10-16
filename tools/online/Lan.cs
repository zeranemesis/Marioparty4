using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace PartyBoardOnline {

// Games on this network, without Internet, a box or a code. A player looking
// for one broadcasts a question; each host waiting for players answers it
// directly with its invitation. The question goes out from the asker, so only
// the host needs a way in through Windows' firewall (its salon's own rule
// covers this port too), and the answer is the unicast reply to a broadcast
// Windows lets through by default.
//
// The question carries the asker's build and disc tags, and a host answers only
// when both match its own: a game the asker could not start is not shown.
static class LanDiscovery {
    public const int Port=47474;
    static readonly byte[] Question=Encoding.ASCII.GetBytes("PBLANQ1"),Answer=Encoding.ASCII.GetBytes("PBLANR1");

    public sealed class Game {public string Name,Language,Region,Invite;public int Players,MaxPlayers;}

    static byte[] Tag(byte[] hash){return hash.Take(8).ToArray();}
    static bool Starts(byte[] data,int length,byte[] prefix){return length>=prefix.Length && prefix.Select((b,i)=>data[i]==b).All(x=>x);}

    // The host's side: answers while describe() returns something, that is,
    // while its salon still waits for players.
    public sealed class Beacon : IDisposable {
        readonly UdpClient socket;readonly byte[] build,disc;readonly Func<Game> describe;volatile bool closed;
        public int Answered;
        public Beacon(byte[] build,byte[] disc,Func<Game> describe,int port=Port) {
            this.build=Tag(build);this.disc=Tag(disc);this.describe=describe;
            socket=new UdpClient(AddressFamily.InterNetwork);
            socket.Client.SetSocketOption(SocketOptionLevel.Socket,SocketOptionName.ReuseAddress,true);
            socket.Client.Bind(new IPEndPoint(IPAddress.Any,port));
            new Thread(Serve){IsBackground=true,Name="LAN beacon"}.Start();
        }
        void Serve() {
            while(!closed) {
                byte[] d;IPEndPoint from=null;
                try{d=socket.Receive(ref from);}catch(SocketException){if(closed)return;continue;}catch(ObjectDisposedException){return;}
                // Only this network asks: a question from a public address is
                // not one a LAN game answers.
                if(d.Length!=Question.Length+16 || !Starts(d,d.Length,Question) || Gateway.Public(from.Address))continue;
                if(!d.Skip(Question.Length).Take(8).SequenceEqual(build) || !d.Skip(Question.Length+8).Take(8).SequenceEqual(disc))continue;
                Game game;try{game=describe();}catch{continue;}
                if(game==null)continue;
                var json=new JavaScriptSerializer().Serialize(new Dictionary<string,object>{
                    {"name",game.Name},{"lang",game.Language},{"region",game.Region},{"players",game.Players},{"max",game.MaxPlayers},{"invite",game.Invite}});
                var reply=Answer.Concat(Encoding.UTF8.GetBytes(json)).ToArray();
                try{socket.Send(reply,reply.Length,from);Interlocked.Increment(ref Answered);}catch{}
            }
        }
        public void Dispose(){closed=true;try{socket.Close();}catch{}}
    }

    // Every IPv4 broadcast address this PC can reach, plus the limited one, plus
    // loopback for a second window on the same PC.
    static IEnumerable<IPAddress> Targets() {
        yield return IPAddress.Broadcast;yield return IPAddress.Loopback;
        foreach(var nic in NetworkInterface.GetAllNetworkInterfaces()) {
            if(nic.OperationalStatus!=OperationalStatus.Up)continue;
            foreach(var a in nic.GetIPProperties().UnicastAddresses) {
                if(a.Address.AddressFamily!=AddressFamily.InterNetwork || IPAddress.IsLoopback(a.Address) || a.IPv4Mask==null)continue;
                var ip=a.Address.GetAddressBytes();var mask=a.IPv4Mask.GetAddressBytes();
                yield return new IPAddress(ip.Select((b,i)=>(byte)(b|~mask[i])).ToArray());
            }
        }
    }

    public static List<Game> Find(byte[] build,byte[] disc,int milliseconds=1500,int port=Port) {
        var found=new List<Game>();
        using(var socket=new UdpClient(new IPEndPoint(IPAddress.Any,0))) {
            socket.EnableBroadcast=true;
            var question=Question.Concat(Tag(build)).Concat(Tag(disc)).ToArray();
            var targets=Targets().Distinct().ToArray();
            // Twice: one lost broadcast should not hide a game.
            for(int round=0;round<2;round++)
                foreach(var t in targets)try{socket.Send(question,question.Length,new IPEndPoint(t,port));}catch(SocketException){}
            var timer=Stopwatch.StartNew();
            while(timer.ElapsedMilliseconds<milliseconds) {
                socket.Client.ReceiveTimeout=Math.Max(1,milliseconds-(int)timer.ElapsedMilliseconds);
                byte[] d;IPEndPoint from=null;
                try{d=socket.Receive(ref from);}catch(SocketException){break;}
                if(!Starts(d,d.Length,Answer) || Gateway.Public(from.Address))continue;
                try {
                    var r=new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(Encoding.UTF8.GetString(d,Answer.Length,d.Length-Answer.Length));
                    object v;var game=new Game{
                        Name=r.TryGetValue("name",out v)?v as string:null,Language=r.TryGetValue("lang",out v)?v as string:"en",Region=r.TryGetValue("region",out v)?v as string:"us",
                        Players=r.TryGetValue("players",out v) && v is int?(int)v:1,MaxPlayers=r.TryGetValue("max",out v) && v is int?(int)v:2,Invite=r.TryGetValue("invite",out v)?v as string:null};
                    if(game.Name==null || game.Invite==null)continue;
                    // The invitation must point where the answer came from (or
                    // this PC itself): a LAN list never sends anyone elsewhere.
                    var invite=Invitation.Decode(game.Invite,true);
                    if(!invite.Address.Equals(from.Address) && !IPAddress.IsLoopback(from.Address))continue;
                    if(!found.Any(g=>g.Invite==game.Invite))found.Add(game);
                } catch {}
            }
        }
        return found;
    }
}
}
