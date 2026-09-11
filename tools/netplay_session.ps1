# Shared vocabulary for reading what happened to a PartyBoard instance.
#
# Dot-source this; it defines functions and nothing else.
#
# Two rules shape everything here.
#
#   A termination is normal ONLY with explicit evidence that it was expected.
#   Absence of evidence is abnormal, never success. In particular a supervisor
#   exit code of 0 says nothing at all about the game's own exit code.
#
#   A crash signature must be stable across machines and runs, so it can never
#   contain a process id, an ASLR-dependent absolute address, a timestamp or a
#   handle. It is built from the exception, the module, the symbol, the overlay
#   and the owning process.
#
# NOTE ON DUPLICATION. tools/record_board_session.ps1 still carries its own copy
# of the classification logic below. The two agree today and were written from
# the same source. They are deliberately not merged yet: that script is the one
# the human recording session uses, and merging it cannot be validated without a
# human at the controller. Merge it the next time a recorded session passes.

# NTSTATUS values arrive as exit codes when a process dies of an exception the
# reporter could not intercept. 0xC0000374 (heap corruption) is the important
# one: it goes through __fastfail and no user-mode handler ever runs.
#
# The mask is [uint32]::MaxValue and NOT the literal 0xFFFFFFFF. Windows
# PowerShell 5.1 parses a hex literal that fits in 32 bits as [int], so
# 0xFFFFFFFF is -1: `$code -band 0xFFFFFFFF` returned $code unchanged, still
# negative, and the cast to [uint32] then threw "the value was too large or too
# small". The whole campaign died on the first peer that crashed - on exactly
# the case these functions exist to record, so a broken harness read as a broken
# game, which is the confusion the HARNESS_FAILURE classification exists to
# prevent. tools/record_board_session.ps1 had the correct form all along; this
# file did not, and nothing compared them.
function Test-IsNtStatus([int64]$code) {
    if ($code -eq 0) { return $false }
    # [int64] on both sides of the comparison, and never a bare hex literal:
    # 0xC0000000 is ALSO parsed as [int] in 5.1, so it is -1073741824, and
    # `$unsigned -le 0xCFFFFFFF` compared a positive number against a negative
    # one and was always false. The self-test below caught this second bug in
    # the same two lines the moment it was written.
    $unsigned = [int64]([uint32]::MaxValue -band $code)
    return ($unsigned -ge [int64]3221225472) -and ($unsigned -le [int64]3489660927)
}

# Named, so a result says STATUS_HEAP_CORRUPTION and not only 0xC0000374. The
# recorder had this table and the campaign did not, so every campaign result so
# far recorded a bare hex number - and the fingerprint built from it could not
# name the exception either. Same table as src/port/crash_report.cpp.
$script:ExceptionNames = @{
    3221225477 = 'EXCEPTION_ACCESS_VIOLATION'      # 0xC0000005
    3221225478 = 'EXCEPTION_IN_PAGE_ERROR'         # 0xC0000006
    3221225501 = 'EXCEPTION_ILLEGAL_INSTRUCTION'   # 0xC000001D
    3221225612 = 'EXCEPTION_ARRAY_BOUNDS_EXCEEDED' # 0xC000008C
    3221225620 = 'EXCEPTION_INT_DIVIDE_BY_ZERO'    # 0xC0000094
    3221225622 = 'EXCEPTION_PRIV_INSTRUCTION'      # 0xC0000096
    3221225725 = 'EXCEPTION_STACK_OVERFLOW'        # 0xC00000FD
    3221226356 = 'STATUS_HEAP_CORRUPTION'          # 0xC0000374
    3221226505 = 'STATUS_STACK_BUFFER_OVERRUN'     # 0xC0000409
    3221226994 = 'STATUS_FAIL_FAST_EXCEPTION'      # 0xC0000602
    3221225786 = 'STATUS_CONTROL_C_EXIT'           # 0xC000013A
}

function Format-ExitCode([int64]$code) {
    $unsigned = [int64]([uint32]::MaxValue -band $code)
    $text = ('0x{0:X8} ({1})' -f $unsigned, $code)
    # [int64], never [int]: these values are above 2^31 and PowerShell stores
    # the table keys as Long, so an [int] cast both overflows and misses.
    $name = $script:ExceptionNames[[int64]$unsigned]
    if ($name) { return "$text $name" }
    return $text
}

# Proven rather than assumed, because the bug above was invisible until a real
# peer crashed. Runs on import: it costs microseconds and it is the difference
# between a harness that records a crash and one that dies of it.
foreach ($case in @(
    @{ Code = -1073741819; Text = '0xC0000005'; Nt = $true; Name = 'EXCEPTION_ACCESS_VIOLATION' },
    @{ Code = -1073740940; Text = '0xC0000374'; Nt = $true; Name = 'STATUS_HEAP_CORRUPTION' },
    @{ Code = -1073741571; Text = '0xC00000FD'; Nt = $true; Name = 'EXCEPTION_STACK_OVERFLOW' },
    @{ Code = 0;           Text = '0x00000000'; Nt = $false },
    @{ Code = 1;           Text = '0x00000001'; Nt = $false },
    @{ Code = 2;           Text = '0x00000002'; Nt = $false })) {
    $formatted = Format-ExitCode $case.Code
    if ($formatted -notlike ($case.Text + '*')) {
        throw "Exit code formatting is broken: $($case.Code) formatted as '$formatted', expected $($case.Text)."
    }
    if ((Test-IsNtStatus $case.Code) -ne $case.Nt) {
        throw "NTSTATUS detection is broken for $($case.Code) ($($case.Text))."
    }
    if ($case.ContainsKey('Name') -and $formatted -notmatch [regex]::Escape($case.Name)) {
        throw "Exit code $($case.Text) did not come out named $($case.Name): '$formatted'. The table is wrong."
    }
}

# Windows Application event log record for a process that faulted, used when the
# in-process reporter could not run.
function Get-FaultRecord([int]$processId, [datetime]$from, [datetime]$to) {
    $window = @{ LogName = 'Application'; ProviderName = 'Application Error'; StartTime = $from.AddSeconds(-5) }
    try { $events = Get-WinEvent -FilterHashtable $window -MaxEvents 40 -ErrorAction Stop }
    catch { return $null }
    foreach ($event in $events) {
        if ($event.Message -notmatch 'partyboard\.exe') { continue }
        if ($event.TimeCreated -lt $from.AddSeconds(-5)) { continue }
        if ($to -ne $null -and $event.TimeCreated -gt $to.AddSeconds(30)) { continue }
        $code = [regex]::Match($event.Message, 'code d.exception\s*:?\s*(0x[0-9a-fA-F]+)|xception code:?\s*(0x[0-9a-fA-F]+)')
        $module = [regex]::Match($event.Message, 'odule d.fectueux[^:]*:\s*([^\r\n,]+)|aulting module name:\s*([^\r\n,]+)')
        $offset = [regex]::Match($event.Message, 'ffset[^:]*:\s*(0x[0-9a-fA-F]+)')
        return @{
            Time = $event.TimeCreated
            Code = if ($code.Success) { ($code.Groups[1].Value + $code.Groups[2].Value) } else { 'unknown' }
            Module = if ($module.Success) { ($module.Groups[1].Value + $module.Groups[2].Value).Trim() } else { 'unknown' }
            Offset = if ($offset.Success) { $offset.Groups[1].Value } else { 'unknown' }
        }
    }
    return $null
}

# Reads one peer's live state file, which the game rewrites every few frames and
# flushes the moment a shutdown is requested. This is the only in-process
# evidence left when a termination cannot be intercepted.
function Read-LiveState([string]$path) {
    $state = @{
        ShutdownIntent = 'UNKNOWN'; Frame = 0; GameContext = 'unknown'; Overlay = 'unknown'
        Hash = 'unknown'; HashFrame = 0; Mismatch = 'unknown'; RngSync = 'unknown'
        Repaired = 'unknown'; SendErrors = 'unknown'
        # -1, never 0: a run that never reached a board has no turn, and calling
        # that "turn 0" would make it indistinguishable from a run on turn 0.
        Turn = -1; MaxTurn = -1; Board = -1
    }
    if (-not (Test-Path -LiteralPath $path)) { return $state }
    $text = Get-Content -LiteralPath $path -Raw
    $m = [regex]::Match($text, 'shutdown_intent=(\S+)');            if ($m.Success) { $state.ShutdownIntent = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'simulation_frame=(\d+)');           if ($m.Success) { $state.Frame = [int]$m.Groups[1].Value }
    $m = [regex]::Match($text, 'game_context=(-?\d+) overlay=(-?\d+)')
    if ($m.Success) { $state.GameContext = $m.Groups[1].Value; $state.Overlay = $m.Groups[2].Value }
    $m = [regex]::Match($text, 'last_state_hash=([0-9a-f]+) at_frame=(\d+)')
    if ($m.Success) { $state.Hash = $m.Groups[1].Value; $state.HashFrame = [int]$m.Groups[2].Value }
    $m = [regex]::Match($text, 'mismatch=(\d+)');                   if ($m.Success) { $state.Mismatch = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'rng_sync=(\d+)');                   if ($m.Success) { $state.RngSync = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'repaired=(\d+)');                   if ($m.Success) { $state.Repaired = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'send_errors=(\d+)');                if ($m.Success) { $state.SendErrors = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'board=(-?\d+) turn=(-?\d+) max_turn=(-?\d+)')
    if ($m.Success) {
        $state.Board = [int]$m.Groups[1].Value
        $state.Turn = [int]$m.Groups[2].Value
        $state.MaxTurn = [int]$m.Groups[3].Value
    }
    return $state
}

# Classification, most specific cause first. Mirrors record_board_session.ps1.
function Get-PeerClassification($peer) {
    if ($peer.Fault -or $peer.CrashReports.Count -gt 0 -or (Test-IsNtStatus $peer.ExitCode)) {
        return 'PROCESS_CRASH'
    }
    if ($peer.DesyncReports.Count -gt 0) { return 'NETPLAY_DESYNC' }
    if ($peer.ClosedBySupervisor) { return 'SUPERVISOR_TERMINATED' }
    if ($peer.LiveState.ShutdownIntent -eq 'USER_REQUESTED_EXIT') { return 'USER_REQUESTED_EXIT' }
    if ($peer.LiveState.ShutdownIntent -eq 'NORMAL_GAME_EXIT') { return 'NORMAL_GAME_EXIT' }
    # Exit code 0 with no recorded intent: most likely the window was closed, but
    # nothing proves it, so it stays abnormal rather than being assumed.
    return 'UNKNOWN_ABNORMAL_EXIT'
}

# ---------------------------------------------------------------------------
# Crash fingerprint
#
# Groups identical crashes and separates different ones. Deliberately built
# from symbol names rather than module-relative offsets wherever symbols are
# available: an offset changes with every build, and a signature that changes
# with every build cannot group anything. The offsets are still recorded beside
# the fingerprint as evidence.
# ---------------------------------------------------------------------------

function Get-CrashReportFacts([string]$reportPath) {
    $facts = @{
        ExceptionName = 'unknown'; ExceptionCode = 'unknown'; Operation = 'unknown'
        Module = 'unknown'; ModuleOffset = 'unknown'; StackVerdict = ''
        Frames = @(); TopSymbol = 'unknown'; TopSource = 'unknown'
        GameContext = 'unknown'; Overlay = 'unknown'; Frame = 0
        FramesSinceTransition = 'unknown'; LiveProcess = 'unknown'
        AudioThread = $false; BuildRevision = 'unknown'
        TerminationReason = ''; CoroutineOwner = ''
    }
    if (-not (Test-Path -LiteralPath $reportPath)) { return $facts }
    $text = Get-Content -LiteralPath $reportPath -Raw
    # An empty report is itself a fact: the reporter was reached but could not
    # finish. Saying so beats throwing a hundred null-argument errors and then
    # returning a signature that looks like an ordinary unknown fault.
    if ([string]::IsNullOrEmpty($text)) {
        $facts.TerminationReason = 'EMPTY_REPORT'
        return $facts
    }

    $m = [regex]::Match($text, 'build_revision=(\S+)');      if ($m.Success) { $facts.BuildRevision = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'exception_name=(\S+)');      if ($m.Success) { $facts.ExceptionName = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'exit_code_hex=(\S+)');       if ($m.Success) { $facts.ExceptionCode = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'access_violation operation=(\w+)')
    if ($m.Success) { $facts.Operation = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'faulting_module=([^\r\n]+)')
    if ($m.Success) { $facts.Module = [IO.Path]::GetFileName($m.Groups[1].Value.Trim()) }
    $m = [regex]::Match($text, 'faulting_offset=(0x[0-9a-fA-F]+)')
    if ($m.Success) { $facts.ModuleOffset = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'coroutine_stack_verdict=([^\r\n]+)')
    if ($m.Success) { $facts.StackVerdict = $m.Groups[1].Value.Trim() }
    # A coroutine stack overflow caught on its guard page has no exception record
    # at all: it is reported through [TERMINATION] instead, and the whole
    # signature has to be built from there.
    $m = [regex]::Match($text, 'reason=(\S+)')
    if ($m.Success) { $facts.TerminationReason = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'coroutine stack of ([^:,]+)')
    if ($m.Success) { $facts.CoroutineOwner = $m.Groups[1].Value.Trim() }
    $m = [regex]::Match($text, 'game_context=(-?\d+) overlay=(-?\d+)')
    if ($m.Success) { $facts.GameContext = $m.Groups[1].Value; $facts.Overlay = $m.Groups[2].Value }
    $m = [regex]::Match($text, 'simulation_frame=(\d+)');    if ($m.Success) { $facts.Frame = [int]$m.Groups[1].Value }
    $m = [regex]::Match($text, 'frames_since_transition=(\d+)')
    if ($m.Success) { $facts.FramesSinceTransition = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'STACK live process=(\S+)')
    if ($m.Success) { $facts.LiveProcess = $m.Groups[1].Value }

    # Frames, in order. Symbol and source file where the resolver had them.
    $frames = @()
    foreach ($line in [regex]::Matches($text, 'FRAME\s+\d+\s+0x[0-9a-f]+\s+(\S+)\+(0x[0-9a-fA-F]+)(?:\s+(\S+?)\+0x[0-9a-fA-F]+)?(?:\s+\[([^\]]+)\])?')) {
        $frames += @{
            Module = $line.Groups[1].Value
            Offset = $line.Groups[2].Value
            Symbol = if ($line.Groups[3].Success) { $line.Groups[3].Value } else { '' }
            Source = if ($line.Groups[4].Success) { $line.Groups[4].Value } else { '' }
        }
    }
    $facts.Frames = $frames
    if ($frames.Count -gt 0) {
        $facts.TopSymbol = if ($frames[0].Symbol) { $frames[0].Symbol } else { "$($frames[0].Module)$($frames[0].Offset)" }
        if ($frames[0].Source) {
            # file:line, with the absolute path removed: the path is personal and
            # differs between machines, the file and line do not.
            $source = $frames[0].Source -replace '.*[\\/]', ''
            $facts.TopSource = $source
        }
    }
    foreach ($frame in $frames) {
        if ($frame.Symbol -match 'salAudioThreadFunc|snd_handle_irq') { $facts.AudioThread = $true }
    }
    return $facts
}

# `extra` may carry evidence gathered outside the crash report, which is what
# lets a bare access violation be named as a use-after-free:
#   AudioLifetimeViolation  the audio lifetime detector fired in this run
#   MemoryCorruption        the HuMem sweep fired in this run
function Get-CrashFingerprint($facts, $extra) {
    if ($null -eq $extra) { $extra = @{} }

    $class = 'UNKNOWN_FAULT'
    if ($facts.TerminationReason -eq 'EMPTY_REPORT') {
        return 'EMPTY_CRASH_REPORT:unknown:reporter_did_not_finish'
    }
    if ($facts.TerminationReason -eq 'COROUTINE_STACK_OVERFLOW') {
        $class = 'STACK_OVERFLOW'
    } elseif ($facts.TerminationReason -eq 'MEMORY_CORRUPTION_DETECTED') {
        $class = 'MEMORY_CORRUPTION'
    } elseif ($facts.ExceptionCode -match '0xC0000374') {
        $class = 'HEAP_CORRUPTION'
    } elseif ($facts.StackVerdict -match 'guard page|GUARD PAGE|stack of') {
        $class = 'STACK_OVERFLOW'
    } elseif ($extra.ContainsKey('MemoryCorruption') -and $extra.MemoryCorruption) {
        $class = 'MEMORY_CORRUPTION'
    } elseif ($facts.AudioThread -and $extra.ContainsKey('AudioLifetimeViolation') -and $extra.AudioLifetimeViolation) {
        $class = 'AUDIO_UAF'
    } elseif ($facts.AudioThread) {
        $class = 'AUDIO_FAULT'
    } elseif ($facts.ExceptionName -eq 'EXCEPTION_ACCESS_VIOLATION') {
        $class = if ($facts.Operation -eq 'write') { 'ACCESS_VIOLATION_WRITE' } else { 'ACCESS_VIOLATION_READ' }
    } elseif ($facts.ExceptionName -ne 'unknown') {
        $class = $facts.ExceptionName -replace '^EXCEPTION_', ''
    }

    # Context: the overlay identity the game was in. game_context keeps its value
    # across an unload, which is exactly what makes it usable here.
    $context = if ($facts.GameContext -ne 'unknown') { "overlay$($facts.GameContext)" } else { 'overlay?' }

    # Site: the owning HuPrc process for a stack overflow, the faulting symbol
    # otherwise. Source file and line when the resolver had them, because they
    # survive a rebuild where an offset does not.
    $site = switch ($class) {
        'STACK_OVERFLOW' {
            # The overflowing process names the defect. The process that merely
            # happened to be live is a fallback, because it is often a bystander.
            if ($facts.CoroutineOwner) { $facts.CoroutineOwner }
            elseif ($facts.TopSymbol -ne 'unknown') { "$($facts.TopSymbol)@$($facts.TopSource)" }
            else { $facts.LiveProcess }
        }
        default {
            if ($facts.TopSource -ne 'unknown') { "$($facts.TopSymbol)@$($facts.TopSource)" }
            else { "$($facts.Module)+$($facts.ModuleOffset)" }
        }
    }

    return ("{0}:{1}:{2}" -f $class, $context, $site)
}

# Desync signature, same rules: no pid, no address, no timestamp.
function Get-DesyncFingerprint([string]$desyncLogPath) {
    if (-not (Test-Path -LiteralPath $desyncLogPath)) { return 'DESYNC:unknown' }
    $text = Get-Content -LiteralPath $desyncLogPath -Raw
    $frame = 'unknown'; $subsystem = 'unknown'; $field = 'unknown'
    $m = [regex]::Match($text, 'frame[= ](\d+)');       if ($m.Success) { $frame = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'category=(\w+)');       if ($m.Success) { $subsystem = $m.Groups[1].Value }
    $m = [regex]::Match($text, 'field=(\S+)');          if ($m.Success) { $field = $m.Groups[1].Value }
    # The frame is deliberately NOT part of the signature: the same defect can
    # surface a few frames apart. It is recorded as evidence instead.
    return ("DESYNC:{0}:{1}" -f $subsystem, $field)
}
