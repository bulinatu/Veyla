# Measures how long each displayed frame stays on screen, from outside the app.
# Grabs a small region of the composed desktop at maximum speed, hashes it, and
# records the intervals between content changes. With FG 2x on a 240 Hz panel a
# perfectly paced stream should change every ~8.33 ms (two refreshes).
#
#   powershell -ExecutionPolicy Bypass -File tools\measure_presents.ps1 -Seconds 10
#
param(
    [int]$Seconds = 10,
    [int]$X = 1100,
    [int]$Y = 600,
    [int]$W = 256,
    [int]$H = 256
)

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class PresentProbe
{
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int w, int h);
    [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h,
                                                        IntPtr src, int sx, int sy, int rop);
    [DllImport("gdi32.dll")] static extern int GetDIBits(IntPtr dc, IntPtr bmp, uint start, uint lines,
                                                         byte[] bits, ref BITMAPINFO bi, uint usage);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
    [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
    [DllImport("kernel32.dll")] static extern bool QueryPerformanceCounter(out long v);
    [DllImport("kernel32.dll")] static extern bool QueryPerformanceFrequency(out long v);

    [StructLayout(LayoutKind.Sequential)]
    struct BITMAPINFO
    {
        public int biSize, biWidth, biHeight;
        public short biPlanes, biBitCount;
        public int biCompression, biSizeImage, biXPelsPerMeter, biYPelsPerMeter;
        public int biClrUsed, biClrImportant;
        public int c0, c1, c2;
    }

    static ulong Hash(byte[] data, int len)
    {
        ulong h = 1469598103934665603UL;
        for (int i = 0; i < len; i += 7)
        {
            h ^= data[i];
            h *= 1099511628211UL;
        }
        return h;
    }

    // Returns frame-change timestamps in milliseconds since the first change.
    public static double[] Run(int seconds, int x, int y, int w, int h, out int samples)
    {
        IntPtr screen = GetDC(IntPtr.Zero);
        IntPtr mem = CreateCompatibleDC(screen);
        IntPtr bmp = CreateCompatibleBitmap(screen, w, h);
        IntPtr old = SelectObject(mem, bmp);

        var bi = new BITMAPINFO();
        bi.biSize = 40;
        bi.biWidth = w;
        bi.biHeight = -h;       // top-down
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        var bits = new byte[w * h * 4];

        long freq;
        QueryPerformanceFrequency(out freq);
        double tickMs = 1000.0 / freq;

        var changes = new List<double>();
        long start, now;
        QueryPerformanceCounter(out start);
        ulong last = 0;
        bool haveLast = false;
        samples = 0;

        while (true)
        {
            QueryPerformanceCounter(out now);
            double elapsed = (now - start) * tickMs;
            if (elapsed >= seconds * 1000.0) break;

            if (!BitBlt(mem, 0, 0, w, h, screen, x, y, 0x00CC0020)) continue;   // SRCCOPY
            GetDIBits(mem, bmp, 0, (uint)h, bits, ref bi, 0);
            ulong hash = Hash(bits, bits.Length);
            samples++;
            if (haveLast && hash != last)
            {
                changes.Add(elapsed);
            }
            last = hash;
            haveLast = true;
        }

        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(IntPtr.Zero, screen);

        var result = new double[changes.Count];
        for (int i = 0; i < changes.Count; i++) result[i] = changes[i];
        return result;
    }
}
'@

$samples = 0
$changes = [PresentProbe]::Run($Seconds, $X, $Y, $W, $H, [ref]$samples)
"grabs=$samples changes=$($changes.Count) seconds=$Seconds"

# Per-second unique-frame counts: a spinning camera should raise this toward
# 120 (FG 2x). Idle scenes sit near 60 because generated frames equal the real
# one and hash the same.
$perSec = New-Object int[] $Seconds
foreach ($t in $changes) {
    $s = [int][Math]::Floor($t / 1000.0)
    if ($s -ge 0 -and $s -lt $Seconds) { $perSec[$s]++ }
}
"per-second unique frames: " + (($perSec | ForEach-Object { $_ }) -join ' ')

# Histogram only over the busiest seconds (the actual rotation).
$busy = @()
for ($s = 0; $s -lt $Seconds; $s++) {
    if ($perSec[$s] -ge 100) { $busy += $s }
}
$iv = @()
if ($busy.Count -gt 0) {
    for ($i = 1; $i -lt $changes.Count; $i++) {
        $s = [int][Math]::Floor($changes[$i] / 1000.0)
        if ($busy -contains $s) {
            $iv += [Math]::Round($changes[$i] - $changes[$i - 1], 3)
        }
    }
    "busy seconds: $($busy -join ',')"
}
if ($iv.Count -gt 0) {
    $s = $iv | Sort-Object
    $n = $s.Count
    "interval ms: min=$($s[0]) p50=$($s[[int]($n*0.5)]) p90=$($s[[int]($n*0.9)]) p99=$($s[[int]($n*0.99)]) max=$($s[-1]) avg=$([Math]::Round(($iv | Measure-Object -Average).Average,3)) fps=$([Math]::Round(1000.0 / (($iv | Measure-Object -Average).Average),2))"
    "--- histogram (rounded to 1 ms) ---"
    $iv | ForEach-Object { [Math]::Round($_) } | Group-Object | Sort-Object { [int]$_.Name } |
        ForEach-Object { "{0,4} ms : {1}" -f $_.Name, $_.Count }
}
