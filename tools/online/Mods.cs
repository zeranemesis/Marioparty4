using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Web.Script.Serialization;

namespace PartyBoardOnline {

// Mods in an online session.
//
// Until now the salon compared discs and said nothing about mods, while
// portmain.cpp could only log "every player must run the same mod list" and hope.
// Worse, GameStart never passed PARTYBOARD_MOD_LIST at all, so a game launched
// from here ran with no mods whatever the player had enabled. Both halves are
// fixed together, because announcing a mod list the game will not load would be
// a lie, and loading one nobody announced would desync the session.
//
// A mod changes the files the game reads. In a lockstep session both machines
// must read the same bytes, so the requirement is exact equality of the list -
// same mods, same contents, same order, since order is overlay precedence. This
// is the same rule the disc already lives by, applied one level up.
sealed class ModEntry {
    public readonly int Id;                 // GameBanana submission id; also the folder name
    public readonly string Name;
    public readonly string ContentRoot;     // empty for a mod announced by a peer
    public readonly byte[] Fingerprint;     // first 8 bytes of the archive SHA-256

    public ModEntry(int id,string name,string contentRoot,byte[] fingerprint) {
        if(id<=0)throw new IOException("Identifiant de mod invalide.");
        if(fingerprint==null || fingerprint.Length!=8)throw new IOException("Empreinte de mod invalide.");
        Id=id;Name=ShortName(name);ContentRoot=contentRoot??"";Fingerprint=(byte[])fingerprint.Clone();
    }

    // Names travel over the wire so the other player can be told what to install by
    // name rather than by number. They are cosmetic, so they are trimmed hard and
    // never participate in the comparison below.
    public static string ShortName(string name) {
        name=(name??"").Replace('\r',' ').Replace('\n',' ').Trim();
        if(name.Length==0)return "";
        var bytes=Encoding.UTF8.GetBytes(name);
        if(bytes.Length<=ModSet.NameBytes)return name;
        int n=ModSet.NameBytes;
        while(n>0 && (bytes[n]&0xC0)==0x80)n--;   // never cut a UTF-8 sequence in half
        return new UTF8Encoding(false,false).GetString(bytes,0,n).TrimEnd()+"…";
    }

    public string Describe(){return Name.Length>0?Name+" (#"+Id+")":"#"+Id;}
    public string GameBananaUrl(){return "https://gamebanana.com/mods/"+Id;}
}

sealed class ModSet {
    public const int NameBytes=48;
    // A lockstep session with two dozen mods is already well past what anyone has
    // tested; the cap keeps one control message whole and says so out loud rather
    // than truncating the list and comparing something nobody has.
    public const int MaxMods=24;

    public readonly ModEntry[] Entries;
    public ModSet(IEnumerable<ModEntry> entries) {
        Entries=(entries??Enumerable.Empty<ModEntry>()).ToArray();
        if(Entries.Length>MaxMods)throw new IOException("Trop de mods actifs pour une partie en ligne : "+Entries.Length+", maximum "+MaxMods+".");
    }

    public static readonly ModSet Empty=new ModSet(new ModEntry[0]);
    public bool None {get{return Entries.Length==0;}}

    // Equality is positional. Two players with the same mods in a different order
    // do not have the same game: the overlay resolves a shared file to whichever
    // root comes first.
    public bool Same(ModSet other) {
        if(other==null || other.Entries.Length!=Entries.Length)return false;
        for(int i=0;i<Entries.Length;i++) {
            if(Entries[i].Id!=other.Entries[i].Id)return false;
            if(!Wire.Equal(Entries[i].Fingerprint,other.Entries[i].Fingerprint))return false;
        }
        return true;
    }

    public ModEntry Find(int id){return Entries.FirstOrDefault(e=>e.Id==id);}

    // The same "missing" set DifferenceFrom already computes (required's
    // entries this ModSet has no matching id for), structured for a caller
    // that wants to act on each one -- a download button per mod -- rather
    // than fold it into one human-readable string. Ignores a version
    // mismatch on purpose: a mod already present, even at the wrong build,
    // is not something a download link would fix.
    public IEnumerable<ModEntry> Missing(ModSet required) {
        if(required==null)return Enumerable.Empty<ModEntry>();
        return required.Entries.Where(r=>Find(r.Id)==null);
    }

    // What the other player must change, in words they can act on. Order matters to
    // the comparison, so a pure reordering is reported as such instead of leaving
    // someone hunting for a mod they already have.
    public string DifferenceFrom(ModSet required) {
        if(required==null)return "Les mods de l'autre joueur ne sont pas encore connus.";
        var missing=required.Entries.Where(r=>Find(r.Id)==null).ToArray();
        var extra=Entries.Where(e=>required.Find(e.Id)==null).ToArray();
        var differing=required.Entries.Where(r=>{var m=Find(r.Id);return m!=null && !Wire.Equal(m.Fingerprint,r.Fingerprint);}).ToArray();
        var parts=new List<string>();
        if(missing.Length>0)parts.Add("à installer : "+string.Join(", ",missing.Select(m=>m.Describe()).ToArray()));
        if(extra.Length>0)parts.Add("à désactiver : "+string.Join(", ",extra.Select(m=>m.Describe()).ToArray()));
        if(differing.Length>0)parts.Add("version différente : "+string.Join(", ",differing.Select(m=>m.Describe()).ToArray()));
        if(parts.Count==0 && !Same(required))parts.Add("mêmes mods mais dans un autre ordre : alignez les priorités dans CubeShelf");
        return parts.Count==0?"":string.Join(" • ",parts.ToArray());
    }

    public void Write(BinaryWriter w) {
        w.Write((byte)Entries.Length);
        foreach(var e in Entries) {
            w.Write(e.Id);w.Write(e.Fingerprint);
            var name=Encoding.UTF8.GetBytes(e.Name);
            if(name.Length>NameBytes+4)throw new IOException("Nom de mod trop long.");
            w.Write((byte)name.Length);w.Write(name);
        }
    }

    public static ModSet Read(BinaryReader r) {
        int count=r.ReadByte();
        if(count>MaxMods)throw new IOException("Liste de mods trop longue.");
        var entries=new List<ModEntry>();
        for(int i=0;i<count;i++) {
            int id=r.ReadInt32();var fingerprint=r.ReadBytes(8);
            int n=r.ReadByte();if(n>NameBytes+4)throw new IOException("Nom de mod trop long.");
            var name=new UTF8Encoding(false,true).GetString(r.ReadBytes(n));
            entries.Add(new ModEntry(id,name,"",fingerprint));
        }
        return new ModSet(entries);
    }

    // Reads what CubeShelf has installed and switched on, applying exactly the rules
    // PortableModManager.WriteActiveList uses -- enabled, not switched off inside the
    // game, content root still present, ordered by descending priority then id. If
    // the two ever disagreed the salon would promise a load order the game does not
    // follow, which is the one failure this whole feature exists to prevent.
    public static ModSet FromCubeShelf(string gameId,out string sourceDirectory) {
        sourceDirectory=ModsDirectory(gameId);
        if(sourceDirectory==null)return Empty;
        var installed=Path.Combine(sourceDirectory,"installed.json");
        if(!File.Exists(installed))return Empty;

        var disabled=ReadDisabled(Path.Combine(sourceDirectory,"player-disabled.json"));
        var parser=new JavaScriptSerializer{MaxJsonLength=8*1024*1024};
        var rows=parser.Deserialize<object>(File.ReadAllText(installed)) as object[];
        if(rows==null)return Empty;

        var active=new List<KeyValuePair<long,ModEntry>>();
        foreach(var row in rows) {
            var map=row as Dictionary<string,object>;
            if(map==null)continue;
            if(!Truthy(map,"Enabled"))continue;
            int id=Int(map,"Id");if(id<=0)continue;
            if(disabled.Contains(id))continue;
            string root=Str(map,"ContentRoot");
            if(root.Length==0 || !Directory.Exists(root))continue;
            var sha=Str(map,"Sha256");
            active.Add(new KeyValuePair<long,ModEntry>(
                ((long)Int(map,"Priority")<<32)|(uint)id,
                new ModEntry(id,Str(map,"Name"),Path.GetFullPath(root),Fingerprint(sha,id))));
        }
        // Descending priority, then ascending id -- the key packs both so one sort does it.
        active.Sort((a,b)=>{int p=(b.Key>>32).CompareTo(a.Key>>32);return p!=0?p:((uint)a.Key).CompareTo((uint)b.Key);});
        return new ModSet(active.Select(p=>p.Value));
    }

    // A mod with no recorded hash still has to be comparable, or a player who
    // installed it before CubeShelf started recording hashes would silently match
    // anyone. Falling back to the id alone makes such a mod match only another mod
    // in the same state, which is the honest answer.
    static byte[] Fingerprint(string sha256,int id) {
        var bytes=new byte[8];
        if(!string.IsNullOrEmpty(sha256) && sha256.Length>=16) {
            for(int i=0;i<8;i++) {
                int high=HexDigit(sha256[i*2]),low=HexDigit(sha256[i*2+1]);
                if(high<0 || low<0){bytes=null;break;}
                bytes[i]=(byte)((high<<4)|low);
            }
            if(bytes!=null)return bytes;
        }
        bytes=new byte[8];
        var fallback=BitConverter.GetBytes((long)id);
        Array.Copy(fallback,bytes,8);
        return bytes;
    }

    static int HexDigit(char c) {
        if(c>='0' && c<='9')return c-'0';
        if(c>='a' && c<='f')return c-'a'+10;
        if(c>='A' && c<='F')return c-'A'+10;
        return -1;
    }

    // The salon writes its own list instead of reusing CubeShelf's active-mods.txt.
    // That file is refreshed when CubeShelf launches the game, and the salon launches
    // it itself, so the copy on disk can be stale. Writing what was announced is the
    // only way the two are the same thing.
    public string WriteListFile(string directory) {
        Directory.CreateDirectory(directory);
        var path=Path.Combine(directory,"online-mods.txt");
        File.WriteAllLines(path,Entries.Select(e=>e.ContentRoot).Where(r=>r.Length>0).ToArray(),new UTF8Encoding(false));
        return path;
    }

    static HashSet<int> ReadDisabled(string path) {
        var set=new HashSet<int>();
        if(!File.Exists(path))return set;
        try {
            var rows=new JavaScriptSerializer().Deserialize<object>(File.ReadAllText(path)) as object[];
            if(rows==null)return set;
            foreach(var row in rows){try{set.Add(Convert.ToInt32(row));}catch{}}
        }catch{}
        return set;
    }

    static string ModsDirectory(string gameId) {
        // If CubeShelf started this salon it already said where the mods live; trust
        // that over a guessed path, because a portable install is somewhere else.
        var fromList=Environment.GetEnvironmentVariable("PARTYBOARD_MOD_LIST");
        if(!string.IsNullOrEmpty(fromList)) {
            try {var dir=Path.GetDirectoryName(Path.GetFullPath(fromList));if(Directory.Exists(dir))return dir;}catch{}
        }
        try {
            var dir=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"CubeShelf","Mods",gameId);
            return Directory.Exists(dir)?dir:null;
        }catch{return null;}
    }

    static bool Truthy(Dictionary<string,object> map,string key) {
        object value;if(!map.TryGetValue(key,out value) || value==null)return false;
        if(value is bool)return (bool)value;
        return false;
    }
    static int Int(Dictionary<string,object> map,string key) {
        object value;if(!map.TryGetValue(key,out value) || value==null)return 0;
        try{return Convert.ToInt32(value);}catch{return 0;}
    }
    static string Str(Dictionary<string,object> map,string key) {
        object value;if(!map.TryGetValue(key,out value) || value==null)return "";
        return value.ToString();
    }
}
}
