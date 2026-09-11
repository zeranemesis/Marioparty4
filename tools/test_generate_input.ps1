# Exercises tools/generate_input.py.
#
# The generator's whole premise is that a produced file is indistinguishable
# from a recorded one. If that is wrong the campaign spends machine-nights
# driving the game with input the engine silently drops, and every run comes
# back green having tested nothing. So the first and most important case here
# is a byte-identical round-trip through a REAL recording: read a 16743-frame
# human session, write it back, and require the same bytes.
#
# The second is determinism. A crash found by a monkey is only worth having if
# the same seed reproduces it, so same seed must mean same file, forever.
#
# The third is the safety mask. START opens the pause menu, from which a run can
# quit to the title screen; a monkey that quits reports a clean exit having
# tested nothing. Real human recordings DO contain START - walk.txt does - which
# is exactly why the mask cannot be inferred from a recording and has to be
# asserted here.
#
#   tools\test_generate_input.ps1
#
# Exit codes: 0 pass, 1 fail, 2 python or the recordings are missing.

$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$tool = Join-Path $PSScriptRoot 'generate_input.py'
$work = Join-Path $projectPath 'work/test-generate-input'

$python = $null
foreach ($candidate in 'python3', 'python', 'py') {
    $found = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($found) { $python = $found.Source; break }
}
if (-not $python) { Write-Output 'No python interpreter found.'; exit 2 }
if (-not (Test-Path -LiteralPath $tool)) { Write-Output "Missing $tool"; exit 2 }

$reference = Join-Path $projectPath 'work/netplay-recordings/walk.txt'
if (-not (Test-Path -LiteralPath $reference)) {
    Write-Output "Missing the reference recording $reference; run tools\verify_replays.ps1."
    exit 2
}

if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Path $work -Force | Out-Null

$failures = New-Object Collections.Generic.List[string]
function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Output ("  ok    {0}" -f $name) }
    else { Write-Output ("  FAIL  {0} - {1}" -f $name, $detail); $failures.Add($name) }
}
function Run([string[]]$arguments) {
    # ErrorActionPreference is restored around the call, not dropped for the
    # file. In Windows PowerShell 5.1, redirecting a native command's stderr
    # wraps each line in an ErrorRecord, and under 'Stop' that terminates the
    # script - so testing a tool's REFUSAL path would kill the test that is
    # checking the refusal. Which is what happened.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $python $tool @arguments 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    return @{ Text = ($output -join "`n"); Exit = $code }
}
# Line endings are not part of the content: the tool writes LF and git may hand
# back CRLF.
function ContentHash([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $text = [Text.Encoding]::ASCII.GetString($bytes) -replace "`r`n", "`n"
    $sha = [Security.Cryptography.SHA256]::Create()
    return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($text))).Replace('-', '')
}

Write-Output 'generate_input'

# 1. THE case: a real recording survives read-then-write unchanged.
$roundTrip = Join-Path $work 'roundtrip.txt'
$r = Run @('concat', $reference, '-o', $roundTrip)
Check 'a real recording round-trips without error' ($r.Exit -eq 0) $r.Text
if (Test-Path -LiteralPath $roundTrip) {
    Check 'and comes back byte-identical' ((ContentHash $reference) -eq (ContentHash $roundTrip)) `
        'the tool does not reproduce the engine format exactly'
}

# 2. Determinism.
$a = Join-Path $work 'seed7-a.txt'; $b = Join-Path $work 'seed7-b.txt'; $c = Join-Path $work 'seed8.txt'
Run @('monkey', '--frames', '3000', '--seed', '7', '-o', $a) | Out-Null
Run @('monkey', '--frames', '3000', '--seed', '7', '-o', $b) | Out-Null
Run @('monkey', '--frames', '3000', '--seed', '8', '-o', $c) | Out-Null
Check 'the same seed gives the same file' ((ContentHash $a) -eq (ContentHash $b)) 'monkey is not reproducible'
Check 'a different seed gives a different file' ((ContentHash $a) -ne (ContentHash $c)) 'the seed is ignored'

# 3. The safety mask, asserted against every generated row rather than sampled.
$startSeen = $false
$startFrames = 0
$rows = 0
foreach ($line in [IO.File]::ReadAllLines($a)) {
    $parts = $line.Split(' ')
    if ($parts.Count -ne 9) { continue }
    $rows++
    if (([int]$parts[2] -band 0x1000) -ne 0) { $startSeen = $true; $startFrames++ }
}
# START is now ALLOWED, and the earlier assertion that it never appears was
# based on reasoning the code contradicts: quitting a board returns to the menu
# that called it (pause.c -> BoardKill -> omOvlReturnEx), never to the title and
# never out of the program. Banning it removed the only way an unattended run
# could leave a board and reach another one.
#
# What still has to hold is that it stays RARE. A monkey that pauses constantly
# spends the run in menus instead of playing, so the bound is checked rather
# than the presence.
$startFraction = if ($rows -gt 0) { $startFrames / ($rows / 2.0) } else { 0 }
Check 'START is present, so a run can leave a board' ($startSeen) `
    'without it an unattended run can never reach a second board'
Check 'and stays rare enough not to live in menus' ($startFraction -lt 0.10) `
    ("START on {0:p1} of frames" -f $startFraction)
Check 'the monkey wrote two rows per frame' ($rows -eq 6000) "wrote $rows rows for 3000 frames"

# 3b. And the mask is a real constraint, not a coincidence: the reference
#     recording DOES contain START, so a test that only looked at recordings
#     would never notice the mask was missing.
$describe = Run @('describe', $reference)
Check 'the reference recording does contain START' ($describe.Text -match 'START') `
    'then this test proves less than it claims'

# 4. Navigator: a script produces exactly the frames it asks for.
$script = Join-Path $work 'nav.txt'
[IO.File]::WriteAllLines($script, @(
    '# 120 + (4+30) + 60 = 214 frames',
    'wait 120',
    'press A 4 30',
    'hold LEFT 60'))
$nav = Join-Path $work 'nav-out.txt'
$r = Run @('navigator', '--script', $script, '-o', $nav)
Check 'the navigator runs a script' ($r.Exit -eq 0) $r.Text
if (Test-Path -LiteralPath $nav) {
    $navRows = @([IO.File]::ReadAllLines($nav) | Where-Object { $_.Trim() }).Count
    Check 'and produces exactly the frames the script asks for' ($navRows -eq 214 * 2) `
        "expected $(214 * 2) rows, got $navRows"
}

# 5. Concat renumbers rather than repeating frame indices.
$joined = Join-Path $work 'joined.txt'
Run @('concat', $nav, $a, '-o', $joined) | Out-Null
if (Test-Path -LiteralPath $joined) {
    $lines = [IO.File]::ReadAllLines($joined) | Where-Object { $_.Trim() }
    $lastFrame = [int]($lines[-1].Split(' ')[0])
    Check 'concat renumbers the frames continuously' ($lastFrame -eq (214 + 3000 - 1)) `
        "last frame index is $lastFrame, expected $(214 + 3000 - 1)"
}

# 6. Slice: the approach prefix comes out of a recording we already have, and
#    the boundary cases are refused rather than silently clamped.
$prefix = Join-Path $work 'prefix.txt'
$r = Run @('slice', $reference, '--end', '5385', '-o', $prefix)
Check 'slice cuts a prefix out of a real recording' ($r.Exit -eq 0) $r.Text
if (Test-Path -LiteralPath $prefix) {
    $sliceRows = @([IO.File]::ReadAllLines($prefix) | Where-Object { $_.Trim() }).Count
    Check 'and gives exactly the frames asked for' ($sliceRows -eq 5385 * 2) `
        "expected $(5385 * 2) rows, got $sliceRows"
    # The slice must be the SAME bytes as the head of the original, or a prefix
    # silently stops being the approach it was cut from.
    $head = [IO.File]::ReadAllLines($reference) | Select-Object -First (5385 * 2)
    $cut = [IO.File]::ReadAllLines($prefix)
    $same = $true
    for ($i = 0; $i -lt $cut.Count; $i++) { if ($cut[$i] -ne $head[$i]) { $same = $false; break } }
    Check 'and is byte-for-byte the head of the original' $same 'the slice altered the input'
}
$r = Run @('slice', $reference, '--end', '9999999', '-o', (Join-Path $work 'past-end.txt'))
Check 'slicing past the end is refused' ($r.Exit -ne 0) 'a slice past the end was accepted'
$r = Run @('slice', $reference, '--start', '100', '--end', '100', '-o', (Join-Path $work 'empty-range.txt'))
Check 'an empty range is refused' ($r.Exit -ne 0) 'an empty slice was accepted'

# 7. An unreadable file is reported, not silently treated as empty input.
$empty = Join-Path $work 'empty.txt'
[IO.File]::WriteAllText($empty, "")
$r = Run @('describe', $empty)
Check 'an empty file is refused' ($r.Exit -eq 2) "exit $($r.Exit)"

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue

Write-Output ''
if ($failures.Count -gt 0) {
    Write-Output ('generate_input: FAIL ({0})' -f ($failures -join ', '))
    exit 1
}
Write-Output 'generate_input: PASS'
exit 0
