# Renders the installer window to a PNG (PrintWindow, works behind other windows), then closes it.
# usage: powershell -File installerpreview.ps1 <out.png> <setup.exe> [cropBottomPx]
param([string]$Out, [string]$Exe, [int]$CropBottom = 0)
Add-Type @"
using System; using System.Runtime.InteropServices;
public class GtWin {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName System.Drawing
[GtWin]::SetProcessDPIAware() | Out-Null
$p = Start-Process $Exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$r = New-Object GtWin+RECT
[GtWin]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap $r.R, $r.B
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc(); [GtWin]::PrintWindow($p.MainWindowHandle, $dc, 3) | Out-Null; $g.ReleaseHdc($dc)
if ($CropBottom -gt 0) {
  $bmp = $bmp.Clone((New-Object System.Drawing.Rectangle 0, ($r.B - $CropBottom), $r.R, $CropBottom), $bmp.PixelFormat)
}
$bmp.Save($Out)
Stop-Process -Id $p.Id
