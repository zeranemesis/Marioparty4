using System;
using System.IO;
using System.Linq;
using System.Text;
namespace PartyBoardOnline {
// Structured diagnostics only: never invitation, endpoint, nickname, disk path
// or arbitrary engine/exception output.
sealed class Report {
    public readonly string DirectoryPath;
    public string NativePath {get{return Path.Combine(DirectoryPath,"native.txt");}}
    readonly object gate=new object();int lines;
    static string Root {get{return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"PartyBoard","Diagnostics");}}
    public Report(string root=null) {
        DirectoryPath=Path.Combine(root??Root,DateTime.UtcNow.ToString("yyyyMMdd-HHmmss")+"-"+Guid.NewGuid().ToString("N"));
        try{Directory.CreateDirectory(DirectoryPath);Write("format=1");}catch{}
    }
    public void Write(string text) {
        lock(gate){if(lines>=3000)return;try{File.AppendAllText(Path.Combine(DirectoryPath,"session.txt"),DateTime.UtcNow.ToString("O")+" "+text+Environment.NewLine,Encoding.UTF8);lines++;}catch{}}
    }
    // The newest desync report the game wrote. It lives with the game data,
    // not with the lobby diagnostics, and it is the only file carrying the
    // per-field values - which is what names a divergence instead of merely
    // locating it in one of sixteen subsystems.
    static string LatestDesyncReport() {
        try {
            var folder=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "MarioPartyRD","Party Board","netplay");
            if(!Directory.Exists(folder))return null;
            return Directory.GetFiles(folder,"netplay_desync_*.log")
                .OrderByDescending(f=>File.GetLastWriteTimeUtc(f)).FirstOrDefault();
        } catch(IOException){return null;} catch(UnauthorizedAccessException){return null;}
    }

    // Reads at most `budget` characters. `fromTail` decides which end survives
    // when the file is bigger: the running trace is worth reading backwards
    // from the moment it stopped, a desync report forwards from its header.
    static void AppendSection(StringBuilder result,string title,string path,bool fromTail,int budget) {
        if(result.Length>0 && result[result.Length-1]!=(char)10)result.AppendLine();
        result.AppendLine("--- "+title+" ---");
        if(path==null){result.AppendLine("No data available.");return;}
        try{using(var f=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite)){
            if(fromTail && f.Length>budget){
                f.Seek(f.Length-budget,SeekOrigin.Begin);
                result.AppendLine("[debut tronque : "+(f.Length-budget)+" octets plus anciens omis]");
            }
            using(var reader=new StreamReader(f)){
                var buffer=new char[budget];int n=reader.ReadBlock(buffer,0,buffer.Length);
                result.Append(buffer,0,n);
                if(!fromTail && reader.Peek()>=0)
                    {result.AppendLine();result.AppendLine("[fin tronquee]");}
            }
        }}catch(IOException){result.AppendLine("No data available.");}
        catch(UnauthorizedAccessException){result.AppendLine("No data available.");}
    }

    public string Read() {
        var result=new StringBuilder("PartyBoard diagnostic v1\r\n");
        AppendSection(result,"session.txt",Path.Combine(DirectoryPath,"session.txt"),false,1024*1024);
        AppendSection(result,"native.txt",Path.Combine(DirectoryPath,"native.txt"),true,1024*1024);
        AppendSection(result,"native.txt.desync",Path.Combine(DirectoryPath,"native.txt.desync"),false,256*1024);
        var desync=LatestDesyncReport();
        AppendSection(result,desync==null?"netplay_desync (absent)":Path.GetFileName(desync),
            desync,false,2*1024*1024);
        return result.ToString();
    }
    public static string ReadLatest() {
        if(!Directory.Exists(Root))throw new IOException("Créez ou rejoignez un salon avant d'exporter le diagnostic.");
        var path=Directory.GetDirectories(Root).OrderByDescending(p=>p,StringComparer.Ordinal).FirstOrDefault();
        if(path==null)throw new IOException("Aucun diagnostic disponible.");
        return new Report(path,true).Read();
    }
    Report(string path,bool existing){DirectoryPath=path;}
}
}
