<#
.SYNOPSIS
Measures consecutive frames of the ray traced output that the bench wrote.

.DESCRIPTION
AURORA_RT_SEQUENCE=N makes the game write its final ray traced output -- after
the accumulation and the filter, what gets composited -- for N consecutive
frames once the test script arms the bench: rt_seq_000.pfm and on. The A/B pair
cannot say anything about an accumulation: it holds it off to compare two
settings on one frame.

Per frame, the grain over the whole frame and over the penumbra -- the pixels
the frame shows strictly between dark and lit -- as compare_raytracing_ab.ps1
defines it. Per pair of consecutive frames, the mean absolute change of
luminance and the share of pixels that change by more than 0.02: flicker, but
also whatever really moved, which a sequence from a scripted run always has.
Over the whole sequence, the standard deviation of each pixel through time,
averaged over the pixels that stay in penumbra in every frame.

Two runs never land on the same frames. Compare sequences taken at the same
scripted moment, and read a small difference as no difference.

.EXAMPLE
tools\measure_raytracing_sequence.ps1 -Directory work\seq\sequence
#>
param(
    [Parameter(Mandatory = $true)][string]$Directory
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Text;

public static class RtSeq {
    static string Token(BinaryReader r) {
        int c;
        do { c = r.ReadByte(); } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
        var sb = new StringBuilder();
        while (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            sb.Append((char)c);
            c = r.ReadByte();
        }
        return sb.ToString();
    }

    // Luminance straight from the file, top row first. Only the luminance is
    // kept: a sequence of full colour maps is several hundred megabytes.
    public static float[] LoadLuminance(string path, out int w, out int h) {
        using (var r = new BinaryReader(File.OpenRead(path))) {
            if (Token(r) != "PF") throw new InvalidDataException(path + " is not an RGB float map");
            w = int.Parse(Token(r));
            h = int.Parse(Token(r));
            double scale = double.Parse(Token(r), System.Globalization.CultureInfo.InvariantCulture);
            if (scale >= 0) throw new InvalidDataException(path + " is big-endian, which the bench never writes");
            var l = new float[w * h];
            for (int y = h - 1; y >= 0; --y)
                for (int x = 0; x < w; ++x) {
                    float cr = r.ReadSingle(), cg = r.ReadSingle(), cb = r.ReadSingle();
                    l[y * w + x] = 0.2126f * cr + 0.7152f * cg + 0.0722f * cb;
                }
            return l;
        }
    }

    public static double Mean(float[] l) {
        double s = 0;
        foreach (var v in l) s += v;
        return s / l.Length;
    }

    // As in compare_raytracing_ab.ps1: what a 3x3 box blur takes away.
    public static double Grain(float[] l, int w, int h, bool penumbraOnly, out long counted) {
        double sum = 0, sq = 0;
        long n = 0;
        for (int y = 1; y < h - 1; ++y)
            for (int x = 1; x < w - 1; ++x) {
                float v = l[y * w + x];
                if (penumbraOnly && (v <= 0.02f || v >= 0.98f)) continue;
                double box = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        box += l[(y + dy) * w + x + dx];
                double d = v - box / 9.0;
                sum += d; sq += d * d; ++n;
            }
        counted = n;
        if (n == 0) return 0.0;
        double mean = sum / n;
        return Math.Sqrt(Math.Max(sq / n - mean * mean, 0.0));
    }

    public static void Change(float[] a, float[] b, out double meanAbs, out double overThreshold) {
        double sum = 0;
        long over = 0;
        for (int p = 0; p < a.Length; ++p) {
            double d = Math.Abs(b[p] - a[p]);
            sum += d;
            if (d > 0.02) ++over;
        }
        meanAbs = sum / a.Length;
        overThreshold = (double)over / a.Length;
    }

    // Standard deviation of each pixel through the sequence, averaged over the
    // pixels strictly between dark and lit in every frame: fully lit and fully
    // shadowed pixels hold still whatever the accumulation does, and counting
    // them would only dilute the figure.
    public static double TemporalDeviation(float[][] frames, out long counted) {
        int count = frames[0].Length;
        int n = frames.Length;
        double total = 0;
        long used = 0;
        for (int p = 0; p < count; ++p) {
            double sum = 0, sq = 0;
            bool penumbra = true;
            for (int t = 0; t < n; ++t) {
                float v = frames[t][p];
                if (v <= 0.02f || v >= 0.98f) { penumbra = false; break; }
                sum += v; sq += (double)v * v;
            }
            if (!penumbra) continue;
            double mean = sum / n;
            total += Math.Sqrt(Math.Max(sq / n - mean * mean, 0.0));
            ++used;
        }
        counted = used;
        return used > 0 ? total / used : 0.0;
    }
}
"@

function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = $Values | Sort-Object
    $mid = [int][math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2 -eq 1) { return [double]$sorted[$mid] }
    return ([double]$sorted[$mid - 1] + [double]$sorted[$mid]) / 2.0
}

$files = @(Get-ChildItem (Join-Path (Resolve-Path $Directory).Path 'rt_seq_*.pfm') | Sort-Object Name)
if ($files.Count -lt 2) { throw "$Directory holds $($files.Count) frame(s); a sequence needs at least two" }

$frames = New-Object 'System.Collections.Generic.List[float[]]'
$w = 0
$h = 0
foreach ($file in $files) {
    $fw = 0
    $fh = 0
    $frames.Add([RtSeq]::LoadLuminance($file.FullName, [ref]$fw, [ref]$fh))
    if ($w -eq 0) { $w = $fw; $h = $fh }
    elseif ($fw -ne $w -or $fh -ne $h) { throw "$($file.Name) is ${fw}x${fh}, the first frame ${w}x${h}" }
}

$grains = @()
$penumbraGrains = @()
$counted = [long]0
Write-Host "Sequence de $($frames.Count) frames, ${w}x${h}"
Write-Host "  frame   luminance   grain (image)   grain (penombre)   pixels en penombre"
for ($t = 0; $t -lt $frames.Count; ++$t) {
    $grain = [RtSeq]::Grain($frames[$t], $w, $h, $false, [ref]$counted)
    $penumbra = [RtSeq]::Grain($frames[$t], $w, $h, $true, [ref]$counted)
    $grains += $grain
    $penumbraGrains += $penumbra
    Write-Host ("  {0,5}   {1,9:F5}   {2,13:F5}   {3,16:F5}   {4,18}" -f $t, [RtSeq]::Mean($frames[$t]), $grain, $penumbra, $counted)
}

$changes = @()
$shares = @()
Write-Host "  paire   ecart moyen   part au-dela de 0,02"
for ($t = 1; $t -lt $frames.Count; ++$t) {
    $meanAbs = 0.0
    $share = 0.0
    [RtSeq]::Change($frames[$t - 1], $frames[$t], [ref]$meanAbs, [ref]$share)
    $changes += $meanAbs
    $shares += $share
    Write-Host ("  {0,2}-{1,-2}   {2,11:F6}   {3,19:P3}" -f ($t - 1), $t, $meanAbs, $share)
}

$stable = [long]0
$deviation = [RtSeq]::TemporalDeviation($frames.ToArray(), [ref]$stable)
$summary = [pscustomobject]@{
    Frames = $frames.Count; Width = $w; Height = $h
    GrainMedian = Get-Median $grains
    PenumbraGrainMedian = Get-Median $penumbraGrains
    ChangeMedian = Get-Median $changes
    ShareOverThresholdMedian = Get-Median $shares
    TemporalDeviation = $deviation
    PenumbraPixelsThroughout = $stable
}
Write-Host ("  medianes : grain {0:F5}, grain de penombre {1:F5}, ecart d'une frame a l'autre {2:F6}, part au-dela de 0,02 {3:P3}" -f $summary.GrainMedian, $summary.PenumbraGrainMedian, $summary.ChangeMedian, $summary.ShareOverThresholdMedian)
Write-Host ("  ecart type temporel en penombre : {0:F5} sur {1} pixels restes en penombre" -f $deviation, $stable)
$summary
