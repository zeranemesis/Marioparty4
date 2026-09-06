using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Xml;
using System.Threading;
using System.Diagnostics;

namespace PartyBoardOnline {
sealed class Route {
    public IPAddress Local,Router;
    [DllImport("iphlpapi.dll")] static extern int GetBestInterface(uint destination,out uint index);
    public static Route Detect() {
        uint index,other;
        if(GetBestInterface(BitConverter.ToUInt32(new byte[]{1,1,1,1},0),out index)!=0 ||
           GetBestInterface(BitConverter.ToUInt32(new byte[]{208,67,222,222},0),out other)!=0 || index!=other)
            throw new IOException("Plusieurs connexions sont actives. Gardez une seule connexion Internet puis réessayez.");
        var nic=NetworkInterface.GetAllNetworkInterfaces().FirstOrDefault(n=>n.Supports(NetworkInterfaceComponent.IPv4) && n.GetIPProperties().GetIPv4Properties().Index==index);
        if(nic==null) throw new IOException("Aucune connexion Internet disponible.");
        string name=(nic.Name+" "+nic.Description).ToLowerInvariant();
        if((nic.NetworkInterfaceType!=NetworkInterfaceType.Ethernet && nic.NetworkInterfaceType!=NetworkInterfaceType.Wireless80211) ||
            new[]{"vpn","nord","lynx","wireguard","tailscale","tunnel","proton","wintun","virtual","tap-","tap "}.Any(name.Contains))
            throw new IOException("Votre VPN ou votre connexion virtuelle empêche la préparation automatique. Mettez votre VPN en pause, puis cliquez sur Créer une partie.");
        var props=nic.GetIPProperties();
        var local=props.UnicastAddresses.FirstOrDefault(a=>a.Address.AddressFamily==AddressFamily.InterNetwork && !IPAddress.IsLoopback(a.Address));
        var gateway=props.GatewayAddresses.FirstOrDefault(a=>a.Address.AddressFamily==AddressFamily.InterNetwork && !a.Address.Equals(IPAddress.Any));
        if(local==null || gateway==null || !Gateway.Private(gateway.Address))
            throw new IOException("Cette connexion ne donne pas accès à une box compatible. Essayez depuis votre Wi-Fi ou votre câble Ethernet habituel.");
        return new Route{Local=local.Address,Router=gateway.Address};
    }
}

// Finite leases only. No external discovery service, permanent mapping, DMZ,
// firewall disabling, VPN change, or unbounded URL fetch is implemented here.
sealed class Gateway : IDisposable {
    readonly Route route; readonly int internalPort; readonly byte[] nonce=Wire.Random(12);
    readonly Func<byte[],byte[]> exchangeOverride;
    readonly bool udp; readonly byte ipProtocol; readonly int requestedExternalPort;
    string method,service; Uri control; int externalPort; public IPAddress Address;
    public int Port {get{return externalPort;}} public uint Lifetime {get;private set;}
    public Gateway(Route r,int port,Func<byte[],byte[]> exchange=null) : this(r,port,port,false,exchange) {}
    public Gateway(Route r,int port,int requestedPort,bool datagram,Func<byte[],byte[]> exchange=null) {
        route=r;internalPort=port;requestedExternalPort=requestedPort;externalPort=requestedPort;udp=datagram;ipProtocol=(byte)(udp?17:6);exchangeOverride=exchange;
    }
    public static bool Private(IPAddress a) {var b=a.GetAddressBytes(); return b.Length==4 && (b[0]==10 || b[0]==172 && b[1]>=16 && b[1]<=31 || b[0]==192 && b[1]==168);}
    public static bool Public(IPAddress a) {
        var b=a.GetAddressBytes(); return b.Length==4 && !Private(a) && b[0]!=0 && b[0]!=127 && b[0]<224 &&
            !(b[0]==100 && b[1]>=64 && b[1]<=127) && !(b[0]==169 && b[1]==254) &&
            !(b[0]==192 && (b[1]==0 || b[1]==2)) && !(b[0]==198 && (b[1]==18 || b[1]==19 || b[1]==51 && b[2]==100)) &&
            !(b[0]==203 && b[1]==0 && b[2]==113);
    }
    internal static void Put16(byte[] b,int p,int v){b[p]=(byte)(v>>8);b[p+1]=(byte)v;}
    internal static void Put32(byte[] b,int p,uint v){b[p]=(byte)(v>>24);b[p+1]=(byte)(v>>16);b[p+2]=(byte)(v>>8);b[p+3]=(byte)v;}
    internal static int U16(byte[] b,int p){return b[p]*256+b[p+1];}
    internal static uint U32(byte[] b,int p){return ((uint)b[p]<<24)|((uint)b[p+1]<<16)|((uint)b[p+2]<<8)|b[p+3];}
    byte[] Exchange(byte[] request) {
        if(exchangeOverride!=null) return exchangeOverride(request);
        using(var u=new UdpClient(new IPEndPoint(route.Local,0))) {
            u.Connect(route.Router,5351); u.Client.ReceiveTimeout=1400;
            for(int i=0;i<2;i++) {u.Send(request,request.Length); try {IPEndPoint from=null;return u.Receive(ref from);} catch(SocketException) {if(i==1) throw;}}
            throw new IOException("La box ne répond pas.");
        }
    }
    internal static byte[] PcpRequest(IPAddress local,byte[] key,int inside,int outside,uint lifetime) {return PcpRequest(local,key,inside,outside,lifetime,6);}
    internal static byte[] PcpRequest(IPAddress local,byte[] key,int inside,int outside,uint lifetime,byte protocol) {
        var q=new byte[60];q[0]=2;q[1]=1;Put32(q,4,lifetime);q[18]=q[19]=255;
        Array.Copy(local.GetAddressBytes(),0,q,20,4);Array.Copy(key,0,q,24,12);q[36]=protocol;
        Put16(q,40,inside);Put16(q,42,outside);return q;
    }
    internal static bool PcpReply(byte[] b,byte[] key,int inside,out int outside,out IPAddress address,out uint life) {return PcpReply(b,key,inside,6,out outside,out address,out life);}
    internal static bool PcpReply(byte[] b,byte[] key,int inside,byte protocol,out int outside,out IPAddress address,out uint life) {
        outside=0;address=null;life=0;
        if(b.Length!=60 || b[0]!=2 || b[1]!=129 || b[3]!=0 || b[36]!=protocol || U16(b,40)!=inside || !Wire.Equal(b.Skip(24).Take(12).ToArray(),key)) return false;
        if(b.Skip(44).Take(10).Any(v=>v!=0) || b[54]!=255 || b[55]!=255) return false;
        outside=U16(b,42);address=new IPAddress(b.Skip(56).Take(4).ToArray());life=U32(b,4);return outside!=0;
    }
    internal void Pcp(uint lifetime) {
        var b=Exchange(PcpRequest(route.Local,nonce,internalPort,externalPort,lifetime,ipProtocol));int port;IPAddress ip;uint life;
        if(!PcpReply(b,nonce,internalPort,ipProtocol,out port,out ip,out life)) throw new IOException("Réponse PCP non compatible.");
        if(lifetime==0) return;
        externalPort=port;Address=ip;Lifetime=life;method="PCP";
        ValidateLease();
    }
    internal static byte[] PmpRequest(int inside,int outside,uint lifetime) {return PmpRequest(inside,outside,lifetime,2);}
    internal static byte[] PmpRequest(int inside,int outside,uint lifetime,byte operation) {
        var q=new byte[12];q[1]=operation;Put16(q,4,inside);Put16(q,6,outside);Put32(q,8,lifetime);return q;
    }
    internal static bool PmpReply(byte[] b,int inside,out int outside,out uint life) {return PmpReply(b,inside,2,out outside,out life);}
    internal static bool PmpReply(byte[] b,int inside,byte operation,out int outside,out uint life) {
        outside=0;life=0;
        if(b.Length!=16 || b[0]!=0 || b[1]!=(byte)(128+operation) || U16(b,2)!=0 || U16(b,8)!=inside) return false;
        outside=U16(b,10);life=U32(b,12);return outside!=0;
    }
    void Pmp(uint lifetime) {
        if(lifetime!=0 && Address==null) {
            var a=Exchange(new byte[]{0,0});
            if(a.Length!=12 || a[0]!=0 || a[1]!=128 || U16(a,2)!=0) throw new IOException("Adresse de la box indisponible.");
            Address=new IPAddress(a.Skip(8).Take(4).ToArray());if(!Public(Address)) throw new IOException("La box est derrière un autre réseau.");
        }
        byte operation=(byte)(udp?1:2);var b=Exchange(PmpRequest(internalPort,externalPort,lifetime,operation));int port;uint life;
        if(!PmpReply(b,internalPort,operation,out port,out life)) throw new IOException("Réponse NAT-PMP non compatible.");
        if(lifetime==0) return;
        externalPort=port;Lifetime=life;method="PMP";ValidateLease();
    }
    void ValidateLease() {if(!Public(Address) || Lifetime<30 || Lifetime>7200) throw new IOException("La box ne propose pas d'accès temporaire compatible.");}
    internal static bool SafeUrl(Uri u,IPAddress router) {
        IPAddress ip; return u!=null && u.Scheme=="http" && u.UserInfo=="" && u.Fragment=="" &&
            IPAddress.TryParse(u.Host,out ip) && ip.Equals(router) && u.Port>0;
    }
    XmlDocument Http(Uri uri,string action=null,string body=null) {
        if(!SafeUrl(uri,route.Router)) throw new IOException("Adresse de configuration de la box refusée.");
        var request=(HttpWebRequest)WebRequest.Create(uri);request.Proxy=null;request.AllowAutoRedirect=false;
        request.Timeout=2500;request.ReadWriteTimeout=2500;
        if(action!=null) {
            request.Method="POST";request.ContentType="text/xml; charset=utf-8";request.Headers["SOAPAction"]="\""+service+"#"+action+"\"";
            var bytes=Encoding.UTF8.GetBytes("<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:"+action+" xmlns:u=\""+service+"\">"+body+"</u:"+action+"></s:Body></s:Envelope>");
            request.ContentLength=bytes.Length;using(var s=request.GetRequestStream()) s.Write(bytes,0,bytes.Length);
        }
        using(var response=(HttpWebResponse)request.GetResponse()) {
            if(response.StatusCode!=HttpStatusCode.OK || response.ContentLength>131072) throw new IOException("Réponse de la box non compatible.");
            var settings=new XmlReaderSettings{DtdProcessing=DtdProcessing.Prohibit,XmlResolver=null,MaxCharactersInDocument=131072};
            using(var reader=XmlReader.Create(response.GetResponseStream(),settings)) {var doc=new XmlDocument{XmlResolver=null};doc.Load(reader);return doc;}
        }
    }
    static string Value(XmlNode n,string name) {var x=n.SelectSingleNode(".//*[local-name()='"+name+"']");return x==null?null:x.InnerText;}
    Uri Discover() {
        using(var u=new UdpClient(new IPEndPoint(route.Local,0))) {
            u.Client.SetSocketOption(SocketOptionLevel.IP,SocketOptionName.MulticastInterface,route.Local.GetAddressBytes());
            u.Client.SetSocketOption(SocketOptionLevel.IP,SocketOptionName.MulticastTimeToLive,1);u.Client.ReceiveTimeout=300;
            var q=Encoding.ASCII.GetBytes("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n");
            u.Send(q,q.Length,new IPEndPoint(IPAddress.Parse("239.255.255.250"),1900));
            var timer=Stopwatch.StartNew();int packets=0;
            while(timer.ElapsedMilliseconds<2000 && packets<64) {
                try {IPEndPoint sender=null;var b=u.Receive(ref sender);packets++;
                    if(!sender.Address.Equals(route.Router) || b.Length>8192) continue;
                    string text=Encoding.ASCII.GetString(b);if(!text.StartsWith("HTTP/1.1 200",StringComparison.OrdinalIgnoreCase)) continue;
                    foreach(var line in text.Split('\n')) if(line.StartsWith("LOCATION:",StringComparison.OrdinalIgnoreCase)) {
                        Uri uri;if(Uri.TryCreate(line.Substring(9).Trim(),UriKind.Absolute,out uri) && SafeUrl(uri,route.Router)) return uri;
                    }
                } catch(SocketException) {}
            }
        }
        throw new IOException("La box ne répond pas à la préparation automatique.");
    }
    string MappingKey() {return "<NewRemoteHost></NewRemoteHost><NewExternalPort>"+externalPort+"</NewExternalPort><NewProtocol>"+(udp?"UDP":"TCP")+"</NewProtocol>";}
    void Upnp(uint lifetime) {
        if(control==null) {
            var url=Discover();var doc=Http(url);
            foreach(XmlNode node in doc.SelectNodes("//*[local-name()='service']")) {
                string type=Value(node,"serviceType");
                if(type!="urn:schemas-upnp-org:service:WANIPConnection:1" && type!="urn:schemas-upnp-org:service:WANIPConnection:2" && type!="urn:schemas-upnp-org:service:WANPPPConnection:1") continue;
                Uri candidate;if(Uri.TryCreate(url,Value(node,"controlURL"),out candidate) && SafeUrl(candidate,route.Router)) {service=type;control=candidate;break;}
            }
            if(control==null) throw new IOException("La box ne propose pas de connexion automatique compatible.");
            IPAddress ip;if(!IPAddress.TryParse(Value(Http(control,"GetExternalIPAddress",""),"NewExternalIPAddress"),out ip) || !Public(ip))
                throw new IOException("Votre box est derrière un autre réseau. La connexion directe n'est pas disponible ici.");
            Address=ip;
        }
        if(lifetime==0) {Http(control,"DeletePortMapping",MappingKey());return;}
        // Avoid replacing an existing mapping owned by another program.
        if(method!="UPNP") {
            try {Http(control,"GetSpecificPortMappingEntry",MappingKey());throw new IOException("Ce port est déjà utilisé sur la box. Réessayez pour en choisir un autre.");}
            catch(WebException ex) {
                // Only the SOAP 'NoSuchEntryInArray' response permits creation.
                var response=ex.Response; if(response==null) throw;
                using(response) using(var stream=response.GetResponseStream()) {
                    var settings=new XmlReaderSettings{DtdProcessing=DtdProcessing.Prohibit,XmlResolver=null,MaxCharactersInDocument=32768};
                    using(var reader=XmlReader.Create(stream,settings)) {var error=new XmlDocument{XmlResolver=null};error.Load(reader);if(Value(error,"errorCode")!="714") throw;}
                }
            }
        }
        Http(control,"AddPortMapping",MappingKey()+"<NewInternalPort>"+internalPort+"</NewInternalPort><NewInternalClient>"+route.Local+"</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>PartyBoard temporary session</NewPortMappingDescription><NewLeaseDuration>"+lifetime+"</NewLeaseDuration>");
        method="UPNP"; // Cleanup is required even if the router ignores the lease.
        var check=Http(control,"GetSpecificPortMappingEntry",MappingKey());
        uint granted;if(Value(check,"NewInternalClient")!=route.Local.ToString() || Value(check,"NewInternalPort")!=internalPort.ToString() || !uint.TryParse(Value(check,"NewLeaseDuration"),out granted)) throw new IOException("La box n'a pas confirmé la connexion temporaire.");
        Lifetime=granted;ValidateLease();
    }
    public void Open() {
        foreach(string candidate in new[]{"PCP","PMP","UPNP"}) {
            try {if(candidate=="PCP") Pcp(120);else if(candidate=="PMP") Pmp(120);else Upnp(120);return;}
            catch {if(method!=null) Dispose();method=null;Address=null;externalPort=requestedExternalPort;}
        }
        throw new IOException("La box n'a pas autorisé la connexion automatique. Vous pouvez essayer d'inverser les rôles : votre ami crée la partie. Aucun réglage manuel n'est nécessaire dans PartyBoard, mais certains réseaux ne permettent pas la connexion directe.");
    }
    public void Renew() {
        int beforePort=externalPort;var beforeAddress=Address;
        if(method=="PCP") Pcp(120);else if(method=="PMP") Pmp(120);else if(method=="UPNP") Upnp(120);else throw new IOException("Connexion temporaire fermée.");
        if(beforePort!=externalPort || !beforeAddress.Equals(Address)) throw new IOException("La connexion de la box a changé. Recréez une partie.");
    }
    public void Dispose() {try{if(method=="PCP") Pcp(0);else if(method=="PMP") Pmp(0);else if(method=="UPNP") Upnp(0);}catch{} finally{method=null;}}
}
}
