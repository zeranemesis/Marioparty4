using System;
using System.IO;
using System.Net;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Diagnostics;

namespace PartyBoardOnline {
sealed class UpdateInfo {
    public Version Version;
    public string DownloadUrl;
    public string Sha256;
    public string Notes;
}

static class UpdateService {
    public const string CurrentVersion = "0.15.1";
    const string ManifestUrl = "https://raw.githubusercontent.com/zeranemesis/Marioparty4/audio-local/update.json";

    static string Json(string text, string key) {
        var match=Regex.Match(text,"\\\""+Regex.Escape(key)+"\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"",RegexOptions.CultureInvariant);
        return match.Success ? match.Groups[1].Value.Replace("\\\\","\\").Replace("\\\"","\"") : "";
    }

    public static Task<UpdateInfo> CheckAsync() {
        return Task.Run(()=> {
            using(var client=new WebClient()) {
                client.Headers[HttpRequestHeader.UserAgent]="PartyBoard-Updater/1.0";
                var raw=client.DownloadString(ManifestUrl);
                Version remote;
                if(!Version.TryParse(Json(raw,"version"),out remote)) throw new InvalidDataException("Le manifeste GitHub contient une version invalide.");
                var url=Json(raw,"downloadUrl");var hash=Json(raw,"sha256");
                if(String.IsNullOrWhiteSpace(url)||!Regex.IsMatch(hash,"^[0-9a-fA-F]{64}$")) throw new InvalidDataException("Le manifeste GitHub est incomplet.");
                return new UpdateInfo{Version=remote,DownloadUrl=url,Sha256=hash.ToUpperInvariant(),Notes=Json(raw,"notes")};
            }
        });
    }

    public static async Task InstallAsync(UpdateInfo info) {
        var zip=Path.Combine(Path.GetTempPath(),"PartyBoard-update-"+Guid.NewGuid().ToString("N")+".zip");
        using(var client=new WebClient()) {
            client.Headers[HttpRequestHeader.UserAgent]="PartyBoard-Updater/1.0";
            await client.DownloadFileTaskAsync(new Uri(info.DownloadUrl),zip);
        }
        using(var sha=SHA256.Create()) using(var stream=File.OpenRead(zip)) {
            var actual=BitConverter.ToString(sha.ComputeHash(stream)).Replace("-","");
            if(!String.Equals(actual,info.Sha256,StringComparison.OrdinalIgnoreCase)) {File.Delete(zip);throw new InvalidDataException("La signature SHA-256 de la mise à jour ne correspond pas.");}
        }
        var target=AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar,Path.AltDirectorySeparatorChar);
        var script=Path.Combine(Path.GetTempPath(),"PartyBoard-update-"+Guid.NewGuid().ToString("N")+".ps1");
        string Q(string value)=>"'"+value.Replace("'","''")+"'";
        File.WriteAllText(script,"Start-Sleep -Seconds 2\nExpand-Archive -LiteralPath "+Q(zip)+" -DestinationPath "+Q(target)+" -Force\nRemove-Item -LiteralPath "+Q(zip)+" -Force\nRemove-Item -LiteralPath $PSCommandPath -Force\nStart-Process -FilePath "+Q(Path.Combine(target,"PartyBoardOnline.exe")),System.Text.Encoding.UTF8);
        Process.Start(new ProcessStartInfo("powershell.exe","-NoProfile -ExecutionPolicy Bypass -File "+Q(script)){UseShellExecute=false,CreateNoWindow=true,WindowStyle=ProcessWindowStyle.Hidden});
    }
}
}
