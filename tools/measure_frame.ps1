# Captures the primary screen (which includes the FG child window) and prints
# objective picture metrics used by docs/25-final-day-plan.md:
#   mean luma, luma std, mean saturation, circular mean hue,
#   Laplacian variance on luma (sharpness proxy).
#
# The maths runs in C# (Add-Type) on a 640x360 downscale; the PowerShell layer
# only prints.
#
# Examples:
#   powershell -ExecutionPolicy Bypass -File tools\measure_frame.ps1 -Out build\shots\neutral.png
#   powershell -ExecutionPolicy Bypass -File tools\measure_frame.ps1 -Compare a.png b.png
param(
    [string]$Out,
    [string[]]$Compare,
    [int]$Width = 1280,
    [int]$Height = 720
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$source = @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Windows.Forms;

public static class FrameMetrics
{
    public static void Capture(string path, int w, int h)
    {
        var b = Screen.PrimaryScreen.Bounds;
        using (var full = new Bitmap(b.Width, b.Height, PixelFormat.Format32bppArgb))
        {
            using (var g = Graphics.FromImage(full))
            {
                g.CopyFromScreen(b.Left, b.Top, 0, 0, full.Size, CopyPixelOperation.SourceCopy);
            }
            full.Save(path, ImageFormat.Png);
        }
    }

    public static Bitmap Downscale(Bitmap src, int w, int h)
    {
        var dst = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(dst))
        {
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.DrawImage(src, 0, 0, w, h);
        }
        return dst;
    }

    // mean luma, luma std, mean saturation, circular mean hue, Laplacian var
    public static double[] Metrics(string path, int w, int h)
    {
        using (var src = new Bitmap(path))
        using (var bmp = (w <= 0 || h <= 0) ? new Bitmap(src) : Downscale(src, w, h))
        {
            w = bmp.Width;
            h = bmp.Height;
            var rect = new Rectangle(0, 0, w, h);
            var data = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            int stride = data.Stride;
            var bytes = new byte[stride * h];
            Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
            bmp.UnlockBits(data);

            var lum = new double[w * h];
            double sumL = 0, sumL2 = 0, sumS = 0, hx = 0, hy = 0;

            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    int i = y * stride + x * 4;
                    double bb = bytes[i] / 255.0;
                    double gg = bytes[i + 1] / 255.0;
                    double rr = bytes[i + 2] / 255.0;
                    double l = 0.2126 * rr + 0.7152 * gg + 0.0722 * bb;
                    lum[y * w + x] = l;
                    sumL += l;
                    sumL2 += l * l;

                    double mx = Math.Max(rr, Math.Max(gg, bb));
                    double mn = Math.Min(rr, Math.Min(gg, bb));
                    double s = mx > 0 ? (mx - mn) / mx : 0;
                    sumS += s;
                    if (s > 0.05 && mx > mn)
                    {
                        double d = mx - mn;
                        double hh;
                        if (mx == rr) hh = ((gg - bb) / d) % 6;
                        else if (mx == gg) hh = ((bb - rr) / d) + 2;
                        else hh = ((rr - gg) / d) + 4;
                        hh *= 60;
                        if (hh < 0) hh += 360;
                        double rad = hh * Math.PI / 180;
                        hx += Math.Cos(rad) * s;
                        hy += Math.Sin(rad) * s;
                    }
                }
            }

            double n = w * h;
            double mean = sumL / n;
            double std = Math.Sqrt(Math.Max(0, sumL2 / n - mean * mean));
            double meanSat = sumS / n;

            double lapSum = 0, lap2 = 0;
            int count = 0;
            for (int y = 1; y < h - 1; y++)
            {
                for (int x = 1; x < w - 1; x++)
                {
                    double v = 4 * lum[y * w + x]
                               - lum[y * w + x - 1] - lum[y * w + x + 1]
                               - lum[(y - 1) * w + x] - lum[(y + 1) * w + x];
                    lapSum += v;
                    lap2 += v * v;
                    count++;
                }
            }
            double lapMean = lapSum / count;
            double lapVar = lap2 / count - lapMean * lapMean;

            double hue = (hx != 0 || hy != 0) ? Math.Atan2(hy, hx) * 180 / Math.PI : -1;
            if (hue < 0) hue += 360;
            return new double[] { mean, std, meanSat, hue, lapVar };
        }
    }
}
'@

if (-not ('FrameMetrics' -as [type])) {
    Add-Type -TypeDefinition $source -ReferencedAssemblies System.Drawing,System.Windows.Forms
}

function Show-Metrics([string]$path, [int]$w, [int]$h) {
    $m = [FrameMetrics]::Metrics($path, $w, $h)
    [pscustomobject]@{
        Path     = (Resolve-Path $path).Path
        MeanLuma = [Math]::Round($m[0], 5)
        LumaStd  = [Math]::Round($m[1], 5)
        MeanSat  = [Math]::Round($m[2], 5)
        HueDeg   = [Math]::Round($m[3], 2)
        LapVar   = [Math]::Round($m[4], 7)
    }
}

if ($Out) {
    $dir = Split-Path -Parent $Out
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [FrameMetrics]::Capture((Join-Path (Get-Location) $Out), $Width, $Height)
    Show-Metrics $Out $Width $Height | Format-List
}

if ($Compare -and $Compare.Count -ge 2) {
    $a = [FrameMetrics]::Metrics($Compare[0], $Width, $Height)
    $b = [FrameMetrics]::Metrics($Compare[1], $Width, $Height)
    $dLuma = if ($a[0] -ne 0) { ($b[0] - $a[0]) / $a[0] * 100.0 } else { 0 }
    $dLap = if ($a[4] -ne 0) { ($b[4] - $a[4]) / $a[4] * 100.0 } else { 0 }
    [pscustomobject]@{
        A            = (Resolve-Path $Compare[0]).Path
        B            = (Resolve-Path $Compare[1]).Path
        MeanLumaA    = [Math]::Round($a[0], 5)
        MeanLumaB    = [Math]::Round($b[0], 5)
        DeltaLumaPct = [Math]::Round($dLuma, 2)
        LapVarA      = [Math]::Round($a[4], 7)
        LapVarB      = [Math]::Round($b[4], 7)
        DeltaLapPct  = [Math]::Round($dLap, 2)
        MeanSatA     = [Math]::Round($a[2], 5)
        MeanSatB     = [Math]::Round($b[2], 5)
        HueA         = [Math]::Round($a[3], 2)
        HueB         = [Math]::Round($b[3], 2)
    } | Format-List
}
