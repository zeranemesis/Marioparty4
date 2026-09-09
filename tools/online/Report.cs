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
    public string Read() {
        var result=new StringBuilder("PartyBoard diagnostic v1\r\n");
        foreach(var name in new[]{"session.txt","native.txt","native.txt.desync"}){
            result.AppendLine("--- "+name+" ---");
            try{using(var f=new FileStream(Path.Combine(DirectoryPath,name),FileMode.Open,FileAccess.Read,FileShare.ReadWrite))using(var reader=new StreamReader(f)){
                var buffer=new char[1024*1024];int n=reader.ReadBlock(buffer,0,buffer.Length);result.Append(buffer,0,n);
            }}catch(IOException){result.AppendLine("No data available.");}
        }
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
