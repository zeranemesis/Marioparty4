# Checks that a new object's group field is set before anything can skip it.
#
# D15. omAddMember writes object->group only inside
#
#     if (group_ptr->num_objs != group_ptr->max_objs) { ... }
#
# and omAddObjEx writes it only in the other branch of
#
#     if (group >= 0) { omAddMember(...); } else { object->group = group; }
#
# so an object created with group >= 0, on a manager whose groups have no
# capacity, comes out of the pool with that field never assigned - holding
# whatever the recycled slot held. On 2026-09-12 two machines held 480 and 389
# there, the canonical hash saw the difference, and the session stopped inside
# m422Dll. omDelMember would then have indexed objman->group[480] on an array of
# ten and written through it.
#
# WHAT THIS TEST PROVES, AND WHAT IT DOES NOT. It reads the source and requires
# that omAddObjEx assign object->group before the branch that can skip the
# assignment. That is a guard against the defect returning, and against anyone
# moving the initialisation back inside a conditional. It is NOT a behavioural
# test: it does not run the allocator, and it cannot tell you that a real
# session no longer diverges. That proof is replaying m422Dll after the fix and
# requiring the divergence to be gone, which needs a route into the minigame
# and is listed in the register.
#
#   tools\test_object_group_init.ps1
#
# Exit codes: 0 the field is initialised unconditionally, 1 it is not,
# 2 the source could not be read.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectPath = Split-Path $PSScriptRoot -Parent
$source = Join-Path $projectPath 'src/game/objmain.c'
if (-not (Test-Path -LiteralPath $source)) {
    Write-Output "object group init: $source introuvable"
    exit 2
}

$text = [IO.File]::ReadAllText($source)

# The function body, from its signature to the first closing brace in column 1.
$signature = [regex]::Match($text, 'omObjData \*omAddObjEx\([^)]*\)\s*\r?\n\{')
if (-not $signature.Success) {
    Write-Output 'object group init: omAddObjEx introuvable dans objmain.c'
    exit 2
}
$rest = $text.Substring($signature.Index + $signature.Length)
$end = [regex]::Match($rest, '(?m)^\}')
if (-not $end.Success) {
    Write-Output 'object group init: fin de omAddObjEx introuvable'
    exit 2
}
$body = $rest.Substring(0, $end.Index)

$failures = New-Object 'Collections.Generic.List[string]'
function Check([bool]$condition, [string]$name) {
    if ($condition) { Write-Output "  PASS  $name" }
    else { Write-Output "  FAIL  $name"; $failures.Add($name) }
}

$branch = [regex]::Match($body, 'if\s*\(\s*group\s*>=\s*0\s*\)')
Check $branch.Success 'omAddObjEx contient toujours la branche if (group >= 0)'
if (-not $branch.Success) {
    Write-Output ''
    Write-Output 'object group init: FAIL (structure inattendue)'
    exit 1
}

$before = $body.Substring(0, $branch.Index)
$assignment = [regex]::Match($before, 'object->group\s*=')
Check $assignment.Success "object->group est affecte avant la branche qui peut l'ignorer"

# And unconditionally: an assignment nested in a conditional before the branch
# would satisfy the test above while leaving the same hole open.
if ($assignment.Success) {
    $line = ($before.Substring(0, $assignment.Index) -split "`n")[-1]
    $depthBefore = ($before.Substring(0, $assignment.Index).ToCharArray() | Where-Object { $_ -eq '{' }).Count -
                   ($before.Substring(0, $assignment.Index).ToCharArray() | Where-Object { $_ -eq '}' }).Count
    Check ($depthBefore -eq 0) "l'affectation est au niveau de la fonction, pas dans un if"
    Check (-not ($line -match '\bif\b')) "l'affectation n'est pas sur une ligne conditionnelle"
}

# The other half of the mechanism, so a future reader knows why this matters.
$member = [regex]::Match($text, 'void omAddMember\([^)]*\)\s*\r?\n\{')
Check $member.Success 'omAddMember existe toujours'

Write-Output ''
if ($failures.Count -gt 0) {
    Write-Output ('object group init: FAIL ({0})' -f $failures.Count)
    Write-Output 'Un objet cree avec un groupe plein sortira du pool avec object->group'
    Write-Output 'non initialise : hachage canonique divergent, et index hors bornes'
    Write-Output 'dans omDelMember.'
    exit 1
}
Write-Output 'object group init: PASS'
exit 0
