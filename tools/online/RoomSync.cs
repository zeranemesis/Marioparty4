using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace PartyBoardOnline {

// The room as the launcher that opened this window sees it, and what that
// launcher asks of it. Files in the directory PARTYBOARD_ONLINE_STATE_DIR
// names, like CubeShelf's in-game bridge: no socket and no port, two processes
// of the same user.
//
//   room.json             written here on every change, read by the launcher
//   requests/<id>.json    written by the launcher: {"id","action","value"}
//   requests/<id>.done    the answer, written here: {"id","ok","message"}
//
// Actions: "set-visibility" ("public" or "private"), "reload-mods" (the
// launcher changed the mods; read its list again), "close" (leave the room
// and close this window). A request older than RequestLifetime is dropped
// unacted: the launcher gives up long before, and acting late would be worse.
static class RoomSync {
    public const string DirectoryVariable="PARTYBOARD_ONLINE_STATE_DIR";
    public const int Schema=1;
    public static readonly TimeSpan RequestLifetime=TimeSpan.FromSeconds(30);
    const int MaximumRequestBytes=4096,MaximumRequestsPerRead=16;
    static readonly Regex IdPattern=new Regex("^[A-Za-z0-9-]{1,64}$",RegexOptions.CultureInvariant);
    static readonly string[] Actions={"set-visibility","reload-mods","close"};

    public sealed class Request {public string Id,Action,Value;}

    public static string StateFile(string directory){return Path.Combine(directory,"room.json");}
    public static string RequestsDirectory(string directory){return Path.Combine(directory,"requests");}

    // Writes room.json whole or not at all: the launcher never reads half a file.
    public static void Write(string directory,Dictionary<string,object> state,long revision) {
        Directory.CreateDirectory(directory);
        var fields=new Dictionary<string,object>(state){{"schema",Schema},{"revision",revision},{"updated",DateTime.UtcNow.ToString("o")}};
        var file=StateFile(directory);var temporary=file+".tmp";
        File.WriteAllText(temporary,new JavaScriptSerializer().Serialize(fields),new UTF8Encoding(false));
        if(File.Exists(file))File.Replace(temporary,file,null);else File.Move(temporary,file);
    }

    public static List<Request> Read(string directory,DateTime utcNow) {
        var found=new List<Request>();var folder=RequestsDirectory(directory);
        if(!Directory.Exists(folder))return found;
        foreach(var path in Directory.GetFiles(folder,"*.json").OrderBy(p=>File.GetLastWriteTimeUtc(p)).Take(MaximumRequestsPerRead)) {
            Request request=null;
            try {
                var info=new FileInfo(path);
                bool fresh=utcNow-info.LastWriteTimeUtc<=RequestLifetime;
                if(fresh && info.Length>0 && info.Length<=MaximumRequestBytes) {
                    var r=new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(File.ReadAllText(path,Encoding.UTF8));
                    object id,action,value;
                    if(r!=null && r.TryGetValue("id",out id) && id is string && IdPattern.IsMatch((string)id) &&
                       Path.GetFileNameWithoutExtension(path)==(string)id &&
                       r.TryGetValue("action",out action) && Actions.Contains(action as string))
                        request=new Request{Id=(string)id,Action=(string)action,Value=r.TryGetValue("value",out value)?value as string:null};
                }
            } catch {}
            // Taken once, valid or not: a request is never acted on twice.
            try{File.Delete(path);}catch{}
            if(request!=null)found.Add(request);
        }
        return found;
    }

    // PartyBoardOnline.exe --room-tags, with PARTYBOARD_ONLINE_DISC: what a
    // launcher needs to read the public list exactly as this lobby does -- the
    // build and disc tags it filters on, the game, and the mods this PC would
    // play with, fingerprinted the same way. One JSON line on stdout. The disc's
    // SHA-256 takes a while, so it is kept per file (path, size, date).
    public static int PrintTags() {
        try {
            var path=Environment.GetEnvironmentVariable("PARTYBOARD_ONLINE_DISC");
            if(string.IsNullOrWhiteSpace(path) || !File.Exists(path)){Console.Error.WriteLine("PARTYBOARD_ONLINE_DISC must name the disc.");return 2;}
            var info=new FileInfo(Path.GetFullPath(path));
            var cacheFile=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"PartyBoard","disc-tags.json");
            var key=info.FullName.ToLowerInvariant()+"|"+info.Length+"|"+info.LastWriteTimeUtc.Ticks;
            var json=new JavaScriptSerializer();Dictionary<string,object> cache;
            try{cache=File.Exists(cacheFile)?json.Deserialize<Dictionary<string,object>>(File.ReadAllText(cacheFile)):null;}catch{cache=null;}
            cache=cache??new Dictionary<string,object>();
            object hit;string disc,game;
            var entry=cache.TryGetValue(key,out hit)?hit as Dictionary<string,object>:null;
            if(entry!=null && entry.ContainsKey("disc") && entry.ContainsKey("game")){disc=(string)entry["disc"];game=(string)entry["game"];}
            else {
                using(var verified=DiscFile.Verify(info.FullName,_=>{},System.Threading.CancellationToken.None)) {disc=Relay.Short(verified.Hash);game=verified.Pal?"GMPP01":"GMPE01";}
                cache[key]=new Dictionary<string,object>{{"disc",disc},{"game",game}};
                try{Directory.CreateDirectory(Path.GetDirectoryName(cacheFile));File.WriteAllText(cacheFile,json.Serialize(cache),new UTF8Encoding(false));}catch{}
            }
            string from;var mods=ModSet.FromCubeShelf("GMPE01_00",out from);
            var status=new Relay.RoomStatus{Mods=mods};
            Console.WriteLine(json.Serialize(new Dictionary<string,object>{
                {"schema",Schema},{"build",Relay.Short(Wire.BuildHash(AppDomain.CurrentDomain.BaseDirectory))},{"disc",disc},{"game",game},
                {"mods",status.Fields()["mods"]},{"relay",Relay.BaseUrl}}));
            return 0;
        } catch(Exception e) {Console.Error.WriteLine("FAIL: "+e.Message);return 1;}
    }

    public static void Complete(string directory,Request request,bool ok,string message) {
        try {
            var folder=RequestsDirectory(directory);Directory.CreateDirectory(folder);
            var file=Path.Combine(folder,request.Id+".done");var temporary=file+".tmp";
            File.WriteAllText(temporary,new JavaScriptSerializer().Serialize(new Dictionary<string,object>{{"id",request.Id},{"ok",ok},{"message",message??""}}),new UTF8Encoding(false));
            if(File.Exists(file))File.Delete(file);
            File.Move(temporary,file);
        } catch(IOException) {} catch(UnauthorizedAccessException) {}
    }
}
}
