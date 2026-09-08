# Compares two audit sample files float by float. The audit writes five little-endian
# float32 per pixel, so any mismatch is reported against the mean magnitude of the signal
# rather than as a raw count.
param([Parameter(Mandatory)][string]$Left, [Parameter(Mandatory)][string]$Right)
$a = [IO.File]::ReadAllBytes($Left)
$b = [IO.File]::ReadAllBytes($Right)
if ($a.Length -ne $b.Length) { throw "Sample counts differ: $($a.Length) vs $($b.Length)" }
$n = $a.Length / 4
$differing = 0; $maxAbs = 0.0; $sumAbs = 0.0; $sumSquaredDelta = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $x = [BitConverter]::ToSingle($a, $i * 4)
    $y = [BitConverter]::ToSingle($b, $i * 4)
    $sumAbs += [Math]::Abs($x)
    if ($x -ne $y) {
        $differing++
        $d = [Math]::Abs($x - $y)
        $sumSquaredDelta += $d * $d
        if ($d -gt $maxAbs) { $maxAbs = $d }
    }
}
[pscustomobject]@{
    File          = Split-Path $Left -Leaf
    Floats        = $n
    Differing     = $differing
    DifferingPct  = [Math]::Round(100.0 * $differing / $n, 4)
    MaxAbsDelta   = $maxAbs
    RmsDelta      = [Math]::Sqrt($sumSquaredDelta / $n)
    MeanMagnitude = $sumAbs / $n
}
