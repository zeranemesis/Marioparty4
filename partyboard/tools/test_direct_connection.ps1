<#
Read-only Windows LAN diagnostic. No MAP/AddPortMapping requests, no router
configuration, no firewall changes, no public IP lookup and no external service.
A positive discovery result does NOT prove port mapping or Internet reachability.
Run with -SelfTest to test parsers without touching the network.
#>
[CmdletBinding()]
param([switch]$SelfTest)

$ErrorActionPreference = 'Stop'

function Test-PrivateV4([System.Net.IPAddress]$Address) {
    if ($Address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) { return $false }
    $bytes = $Address.GetAddressBytes()
    return $bytes[0] -eq 10 -or ($bytes[0] -eq 172 -and $bytes[1] -ge 16 -and $bytes[1] -le 31) -or
        ($bytes[0] -eq 192 -and $bytes[1] -eq 168)
}

function Read-NatPmpDiscovery([byte[]]$Bytes) {
    if ($Bytes.Length -lt 8 -or $Bytes[0] -ne 0 -or $Bytes[1] -ne 128) { return 'InvalidReply' }
    $result = [int]$Bytes[2] * 256 + [int]$Bytes[3]
    if ($result -eq 1) { return 'UnsupportedVersion' }
    if ($result -ne 0) { return "GatewayError:$result" }
    if ($Bytes.Length -ne 12) { return 'InvalidReply' }
    # Do not print the reported WAN IP. A non-public address can indicate an
    # upstream NAT; it is not sufficient by itself to diagnose carrier-grade NAT.
    $octets = [byte[]]$Bytes[8..11]
    $address = [System.Net.IPAddress]::new($octets)
    if ((Test-PrivateV4 $address) -or
        ($octets[0] -eq 100 -and $octets[1] -ge 64 -and $octets[1] -le 127)) {
        return 'Supported:UpstreamNatPossible'
    }
    if ($octets[0] -eq 0 -or $octets[0] -eq 127 -or $octets[0] -ge 224 -or
        ($octets[0] -eq 169 -and $octets[1] -eq 254)) { return 'InvalidExternalAddress' }
    return 'Supported:MappingNotTested'
}

function Read-PcpAnnouncement([byte[]]$Bytes) {
    if ($Bytes.Length -lt 24 -or $Bytes[0] -ne 2 -or $Bytes[1] -ne 128) { return 'InvalidReply' }
    if ($Bytes[3] -eq 1) { return 'UnsupportedVersion' }
    if ($Bytes[3] -ne 0) { return "GatewayError:$($Bytes[3])" }
    return 'Supported:MappingNotTested'
}

function New-PcpAnnouncement([System.Net.IPAddress]$LocalAddress) {
    if ($LocalAddress.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) {
        throw 'The diagnostic currently supports IPv4 only.'
    }
    $request = [byte[]]::new(24)
    $request[0] = 2 # Version 2, ANNOUNCE opcode zero; no mapping requested.
    $request[18] = 255
    $request[19] = 255 # IPv4-mapped IPv6 client address (::ffff:a.b.c.d).
    [Array]::Copy($LocalAddress.GetAddressBytes(), 0, $request, 20, 4)
    return ,$request
}

function Invoke-GatewayProbe([string]$Protocol, [System.Net.IPAddress]$Gateway,
    [System.Net.IPAddress]$LocalAddress) {
    $socket = [System.Net.Sockets.UdpClient]::new(
        [System.Net.IPEndPoint]::new($LocalAddress, 0))
    try {
        $socket.Client.ReceiveTimeout = 1200
        # Connected UDP accepts replies only from this gateway and port.
        $socket.Connect($Gateway, 5351)
        $request = if ($Protocol -eq 'PCP') { New-PcpAnnouncement $LocalAddress } else { [byte[]]@(0, 0) }
        [void]$socket.Send($request, $request.Length)
        $sender = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
        $reply = $socket.Receive([ref]$sender)
        if ($Protocol -eq 'PCP') { return Read-PcpAnnouncement $reply }
        return Read-NatPmpDiscovery $reply
    } catch [System.Net.Sockets.SocketException] {
        # Silence, ICMP rejection and filtering are not proof of unsupported hardware.
        return 'NoReplyOrFiltered'
    } finally { $socket.Dispose() }
}

function Test-IgdAdvertisement([string]$Reply) {
    return $Reply -match '^HTTP/1\.[01] 200\b' -and
        $Reply -match '(?im)^(ST|USN):[^\r\n]*urn:schemas-upnp-org:device:InternetGatewayDevice:[12](?:\s|$)'
}

function Find-UpnpGateway([System.Net.IPAddress]$Gateway, [System.Net.IPAddress]$LocalAddress) {
    $socket = [System.Net.Sockets.UdpClient]::new(
        [System.Net.IPEndPoint]::new($LocalAddress, 0))
    try {
        $socket.Client.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::IP,
            [System.Net.Sockets.SocketOptionName]::MulticastInterface, $LocalAddress.GetAddressBytes())
        $socket.Client.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::IP,
            [System.Net.Sockets.SocketOptionName]::MulticastTimeToLive, 1)
        $socket.Client.ReceiveTimeout = 300
        $request = [System.Text.Encoding]::ASCII.GetBytes(
            "M-SEARCH * HTTP/1.1`r`nHOST: 239.255.255.250:1900`r`nMAN: `"ssdp:discover`"`r`nMX: 1`r`nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1`r`n`r`n")
        [void]$socket.Send($request, $request.Length,
            [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Parse('239.255.255.250'), 1900))
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        while ($timer.ElapsedMilliseconds -lt 1800) {
            try {
                $sender = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
                $reply = $socket.Receive([ref]$sender)
                if ($sender.Address.Equals($Gateway) -and $reply.Length -le 8192 -and
                    (Test-IgdAdvertisement ([System.Text.Encoding]::ASCII.GetString($reply)))) {
                    # Never follow LOCATION URLs from untrusted discovery replies.
                    return 'Advertised:MappingNotTested'
                }
            } catch [System.Net.Sockets.SocketException] {
                if ($_.Exception.SocketErrorCode -ne [System.Net.Sockets.SocketError]::TimedOut) {
                    return 'NoReplyOrFiltered'
                }
            }
        }
        return 'NoReplyOrFiltered'
    } finally { $socket.Dispose() }
}

if ($SelfTest) {
    $cases = @(
        ((Read-NatPmpDiscovery ([byte[]]@())) -eq 'InvalidReply'),
        ((Read-NatPmpDiscovery ([byte[]]@(0,128,0,0,0,0,0,1,8,8,8,8))) -eq 'Supported:MappingNotTested'),
        ((Read-NatPmpDiscovery ([byte[]]@(0,128,0,0,0,0,0,1,100,64,0,1))) -eq 'Supported:UpstreamNatPossible'),
        ((Read-NatPmpDiscovery ([byte[]]@(0,128,0,0,0,0,0,1,192,168,1,1))) -eq 'Supported:UpstreamNatPossible'),
        ((Read-NatPmpDiscovery ([byte[]]@(0,128,0,0,0,0,0,1,0,0,0,0))) -eq 'InvalidExternalAddress'),
        ((Read-NatPmpDiscovery ([byte[]]@(0,128,0,1,0,0,0,0))) -eq 'UnsupportedVersion'),
        ((Read-NatPmpDiscovery ([byte[]]@(0,129,0,0,0,0,0,1,8,8,8,8))) -eq 'InvalidReply'),
        ((Read-PcpAnnouncement ([byte[]]@())) -eq 'InvalidReply'),
        (Test-IgdAdvertisement "HTTP/1.1 200 OK`r`nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1`r`n"),
        (-not (Test-IgdAdvertisement "HTTP/1.1 200 OK`r`nST: printer`r`n")),
        (-not (Test-PrivateV4 ([System.Net.IPAddress]::Parse('100.64.0.1')))),
        (Test-PrivateV4 ([System.Net.IPAddress]::Parse('192.168.1.1')))
    )
    $request = New-PcpAnnouncement ([System.Net.IPAddress]::Parse('192.168.1.2'))
    $cases += ($request.Length -eq 24 -and $request[0] -eq 2 -and $request[1] -eq 0 -and
        $request[18] -eq 255 -and $request[19] -eq 255 -and $request[23] -eq 2)
    $response = [byte[]]::new(24)
    $response[0] = 2; $response[1] = 128
    $cases += ((Read-PcpAnnouncement $response) -eq 'Supported:MappingNotTested')
    $response[3] = 2
    $cases += ((Read-PcpAnnouncement $response) -eq 'GatewayError:2')
    if ($cases -contains $false) { throw 'Direct connection diagnostic self-test failed.' }
    Write-Output "PASS: $($cases.Count) parser checks; no network requests sent."
    exit 0
}

try {
    $interfaces = @(Get-NetIPInterface -AddressFamily IPv4 | Where-Object ConnectionState -eq Connected)
    $routes = @(Get-NetRoute -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' |
        ForEach-Object {
            $route = $_
            $interface = $interfaces | Where-Object InterfaceIndex -eq $route.InterfaceIndex | Select-Object -First 1
            if ($interface) {
                [pscustomobject]@{ Index = $route.InterfaceIndex; Gateway = $route.NextHop;
                    Alias = $interface.InterfaceAlias; Metric = $route.RouteMetric + $interface.InterfaceMetric }
            }
        } | Sort-Object Metric)
    if (!$routes.Count) { throw 'No active IPv4 default route found.' }
    $selected = $routes[0]
    $adapter = Get-NetAdapter | Where-Object InterfaceIndex -eq $selected.Index | Select-Object -First 1
    if (!$adapter.HardwareInterface -or $selected.Alias -match '(?i)vpn|nord|lynx|wireguard|tailscale|tunnel|proton|wintun') {
        [pscustomobject]@{
            Status = 'Stopped:VpnOrVirtualRoute'
            Interface = $selected.Alias
            Message = 'La route prioritaire est virtuelle ou VPN. Aucun test de box ni ouverture de port effectue. Desactivez le VPN vous-meme si vous souhaitez tester la connexion directe, puis relancez.'
            RouterModified = $false
            ExternalServiceUsed = $false
        } | ConvertTo-Json
        exit 2
    }
    if ($routes.Count -gt 1 -and $routes[1].Metric -eq $selected.Metric) {
        throw 'Several default routes have the same priority; select the intended network in Windows first.'
    }
    $gateway = [System.Net.IPAddress]::Parse($selected.Gateway)
    if (!(Test-PrivateV4 $gateway)) { throw 'Gateway is not a private LAN address; refusing automatic probing.' }
    $addresses = @(Get-NetIPAddress -InterfaceIndex $selected.Index -AddressFamily IPv4 |
        Where-Object { $_.AddressState -eq 'Preferred' -and !$_.SkipAsSource })
    if ($addresses.Count -ne 1) { throw 'Cannot unambiguously select the local IPv4 address.' }
    $local = [System.Net.IPAddress]::Parse($addresses[0].IPAddress)
    [pscustomobject]@{
        Status = 'DiscoveryOnly'
        Interface = $selected.Alias
        PCP = Invoke-GatewayProbe 'PCP' $gateway $local
        NAT_PMP = Invoke-GatewayProbe 'NAT-PMP' $gateway $local
        UPnP = Find-UpnpGateway $gateway $local
        RouterModified = $false
        ExternalServiceUsed = $false
        Message = 'La decouverte ne prouve ni une ouverture de port possible ni une connexion Internet fonctionnelle.'
    } | ConvertTo-Json
} catch {
    Write-Error "Diagnostic interrompu sans modification de la box : $($_.Exception.Message)"
    exit 1
}
