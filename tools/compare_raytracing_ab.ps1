<#
.SYNOPSIS
Compares the two buffers the ray tracing A/B bench writes on one frame.

.DESCRIPTION
AURORA_RT_AB="name=value" makes the game trace its dump frame twice -- A with
the normal settings, B with one of them changed -- and write both as raw float
maps, rt_ab_a.pfm and rt_ab_b.pfm, in its working directory. Same frame, same
acceleration structures, same sampling pattern: the two buffers differ by the
setting and by nothing else, which two scripted runs of the game never do.

Every comparison this project made before the bench compared two runs that
landed on different frames, and several questions stayed open for that reason
alone. Run a null test first -- B set to the value A already has -- and expect
zero differing pixels: anything else means the bench itself leaks state.

Grain is reported twice. Over the whole frame, flat regions -- background, full
light, full shadow -- outnumber the penumbra and dilute any change in its
noise: 4 shadow rays against 12 moved whole-frame grain by 0.03 percent on a
title frame while 50 589 pixels differed. The penumbra figure counts only the
pixels A shows strictly between dark and lit, the same pixels for A and B.

.EXAMPLE
tools\compare_raytracing_ab.ps1 -A work\ab\rt_ab_a.pfm -B work\ab\rt_ab_b.pfm
#>
param(
    [Parameter(Mandatory = $true)][string]$A,
    [Parameter(Mandatory = $true)][string]$B
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Text;

public static class RtAb {
    public sealed class Map { public int Width; public int Height; public float[] Rgb; }

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

    public static Map Load(string path) {
        using (var r = new BinaryReader(File.OpenRead(path))) {
            if (Token(r) != "PF") throw new InvalidDataException(path + " is not an RGB float map");
            int w = int.Parse(Token(r));
            int h = int.Parse(Token(r));
            double scale = double.Parse(Token(r), System.Globalization.CultureInfo.InvariantCulture);
            if (scale >= 0) throw new InvalidDataException(path + " is big-endian, which the bench never writes");
            var m = new Map { Width = w, Height = h, Rgb = new float[w * h * 3] };
            // Stored bottom-up; flipped so that y = 0 is the top row.
            for (int y = h - 1; y >= 0; --y)
                for (int i = 0; i < w * 3; ++i)
                    m.Rgb[y * w * 3 + i] = r.ReadSingle();
            return m;
        }
    }

    public static float[] Luminance(Map m) {
        var l = new float[m.Width * m.Height];
        for (int p = 0; p < l.Length; ++p)
            l[p] = 0.2126f * m.Rgb[p * 3] + 0.7152f * m.Rgb[p * 3 + 1] + 0.0722f * m.Rgb[p * 3 + 2];
        return l;
    }

    public static double Mean(float[] l) {
        double s = 0;
        foreach (var v in l) s += v;
        return s / l.Length;
    }

    // What a 3x3 box blur takes away: the single-pixel grain a denoiser has to
    // remove. A real edge survives the blur and is not counted. With a mask,
    // only the pixels the reference shows strictly between dark and lit count.
    public static double Grain(float[] l, int w, int h, float[] mask, out long counted) {
        double sum = 0, sq = 0;
        long n = 0;
        for (int y = 1; y < h - 1; ++y)
            for (int x = 1; x < w - 1; ++x) {
                if (mask != null) {
                    float v = mask[y * w + x];
                    if (v <= 0.02f || v >= 0.98f) continue;
                }
                double box = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        box += l[(y + dy) * w + x + dx];
                double r = l[y * w + x] - box / 9.0;
                sum += r; sq += r * r; ++n;
            }
        counted = n;
        if (n == 0) return 0.0;
        double mean = sum / n;
        return Math.Sqrt(Math.Max(sq / n - mean * mean, 0.0));
    }

    // The sharpest steps present: the 99.5th percentile of the one-pixel gradient.
    public static double Sharpness(float[] l, int w, int h) {
        var g = new float[(w - 1) * (h - 1)];
        int k = 0;
        for (int y = 0; y < h - 1; ++y)
            for (int x = 0; x < w - 1; ++x)
                g[k++] = Math.Abs(l[y * w + x + 1] - l[y * w + x]) + Math.Abs(l[(y + 1) * w + x] - l[y * w + x]);
        Array.Sort(g);
        return g[(int)(0.995 * (g.Length - 1))];
    }

    // Exact comparison over all three channels: a null test must find nothing.
    // The mean over differing pixels is the effect size where there is one; the
    // mean over the whole frame mostly measures how much of it was untouched.
    public static long[] Differences(Map a, Map b, out double meanAbs, out double meanAbsDiffering, out double maxAbs) {
        long pixels = 0;
        double sum = 0, sumDiffering = 0, max = 0;
        int count = a.Width * a.Height;
        for (int p = 0; p < count; ++p) {
            bool differs = false;
            double pixelSum = 0;
            for (int c = 0; c < 3; ++c) {
                double d = Math.Abs(a.Rgb[p * 3 + c] - b.Rgb[p * 3 + c]);
                if (d > 0) differs = true;
                pixelSum += d;
                if (d > max) max = d;
            }
            sum += pixelSum;
            if (differs) { ++pixels; sumDiffering += pixelSum / 3.0; }
        }
        meanAbs = sum / (count * 3.0);
        meanAbsDiffering = pixels > 0 ? sumDiffering / pixels : 0.0;
        maxAbs = max;
        return new long[] { pixels, count };
    }

    // A buffer with a single value everywhere: no primary ray hit anything, or
    // every pixel was masked. Two of those compare equal and prove nothing.
    public static bool Uniform(float[] l) {
        for (int p = 1; p < l.Length; ++p)
            if (l[p] != l[0]) return false;
        return true;
    }
}
"@

$mapA = [RtAb]::Load((Resolve-Path $A).Path)
$mapB = [RtAb]::Load((Resolve-Path $B).Path)
if ($mapA.Width -ne $mapB.Width -or $mapA.Height -ne $mapB.Height) {
    throw "A is $($mapA.Width)x$($mapA.Height) and B is $($mapB.Width)x$($mapB.Height): not two traces of one frame"
}
$w = $mapA.Width
$h = $mapA.Height
$lumA = [RtAb]::Luminance($mapA)
$lumB = [RtAb]::Luminance($mapB)
$uniform = [RtAb]::Uniform($lumA) -and [RtAb]::Uniform($lumB)

$all = [long]0
$penumbra = [long]0
$grainA = [RtAb]::Grain($lumA, $w, $h, $null, [ref]$all)
$grainB = [RtAb]::Grain($lumB, $w, $h, $null, [ref]$all)
$penumbraA = [RtAb]::Grain($lumA, $w, $h, $lumA, [ref]$penumbra)
$penumbraB = [RtAb]::Grain($lumB, $w, $h, $lumA, [ref]$penumbra)

$meanAbs = 0.0
$meanDiffering = 0.0
$maxAbs = 0.0
$diff = [RtAb]::Differences($mapA, $mapB, [ref]$meanAbs, [ref]$meanDiffering, [ref]$maxAbs)

$rows = @(
    [pscustomobject]@{ mesure = 'luminance moyenne'; A = [RtAb]::Mean($lumA); B = [RtAb]::Mean($lumB) },
    [pscustomobject]@{ mesure = 'grain (image)'; A = $grainA; B = $grainB },
    [pscustomobject]@{ mesure = 'grain (penombre)'; A = $penumbraA; B = $penumbraB },
    [pscustomobject]@{ mesure = 'nettete'; A = [RtAb]::Sharpness($lumA, $w, $h); B = [RtAb]::Sharpness($lumB, $w, $h) }
)
Write-Host "A/B sur une meme frame, ${w}x${h}; penombre : $penumbra pixels"
foreach ($row in $rows) {
    $change = if ($row.A -ne 0) { 100.0 * ($row.B - $row.A) / $row.A } else { 0.0 }
    Write-Host ("  {0,-18} A {1,9:F5}   B {2,9:F5}   {3,8:F2} %" -f $row.mesure, $row.A, $row.B, $change)
}
Write-Host ("  pixels differents  {0} sur {1}; ecart moyen {2:F6} sur l'image, {3:F5} sur les pixels differents, maximal {4:F5}" -f $diff[0], $diff[1], $meanAbs, $meanDiffering, $maxAbs)
if ($uniform) {
    Write-Host "  ATTENTION : A et B sont uniformes, aucun rayon n'a rien touche. Cette paire ne prouve rien."
}

[pscustomobject]@{
    Width = $w; Height = $h; PenumbraPixels = $penumbra
    MeanA = $rows[0].A; MeanB = $rows[0].B
    GrainA = $grainA; GrainB = $grainB
    PenumbraGrainA = $penumbraA; PenumbraGrainB = $penumbraB
    SharpnessA = $rows[3].A; SharpnessB = $rows[3].B
    DifferingPixels = $diff[0]; Pixels = $diff[1]
    MeanAbsDifference = $meanAbs; MeanAbsDifferenceOverDiffering = $meanDiffering
    MaxAbsDifference = $maxAbs
    Uniform = $uniform
}
