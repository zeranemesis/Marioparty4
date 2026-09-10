param([string]$OutputDirectory = 'build\aexp\RelWithDebInfo')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -prerelease -products '*' -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$output = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    [IO.Path]::GetFullPath($OutputDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
}
$source = Join-Path $PSScriptRoot 'online'
New-Item -ItemType Directory -Path $output -Force | Out-Null
& $csc /nologo /target:winexe /platform:x64 /optimize+ /warnaserror+ /langversion:latest "/out:$output\PartyBoardOnline.exe" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Xml.dll /reference:Microsoft.CSharp.dll "$source\Connection.cs" "$source\Gateway.cs" "$source\Program.cs" "$source\Lobby.cs" "$source\LobbyForm.cs" "$source\Tests.cs" "$source\Report.cs" "$source\UpdateService.cs"
if ($LASTEXITCODE -ne 0) { throw 'Online companion compilation failed.' }
Copy-Item -LiteralPath "$source\PartyBoardOnline.exe.config" -Destination $output -Force

