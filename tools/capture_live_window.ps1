param([string]$Path=(Join-Path $PSScriptRoot '../artifacts/editor-live-desktop.png'),[string]$ProcessName='Jester',[switch]$NoActivate)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
if(-not ('JesterWindowCapture' -as [type])) {
    Add-Type -IgnoreWarnings @'
using System;
using System.Runtime.InteropServices;
public static class JesterWindowCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window,out Rect rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
}
'@
}
$jesterExpectedExecutable=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../build/bin/profile/$ProcessName.exe"))
$jesterProcess=Get-Process $ProcessName | Where-Object { $_.Path -eq $jesterExpectedExecutable -and $_.MainWindowHandle -ne [IntPtr]::Zero } | Sort-Object StartTime -Descending | Select-Object -First 1
if(-not $jesterProcess) { throw 'No live Jester window from this workspace.' }
$jesterPreviousDpi=[JesterWindowCapture]::SetThreadDpiAwarenessContext([IntPtr](-4))
try {
    if(-not $NoActivate) { [JesterWindowCapture]::SetForegroundWindow($jesterProcess.MainWindowHandle) | Out-Null }
    Start-Sleep -Milliseconds 350
    $jesterWindowRect=New-Object JesterWindowCapture+Rect
    if(-not [JesterWindowCapture]::GetWindowRect($jesterProcess.MainWindowHandle,[ref]$jesterWindowRect)) { throw 'Cannot read window bounds.' }
    $jesterScreen=[System.Windows.Forms.SystemInformation]::VirtualScreen
    $jesterLeft=[Math]::Max($jesterWindowRect.Left,$jesterScreen.Left)
    $jesterTop=[Math]::Max($jesterWindowRect.Top,$jesterScreen.Top)
    $jesterWidth=[Math]::Min($jesterWindowRect.Right,$jesterScreen.Right)-$jesterLeft
    $jesterHeight=[Math]::Min($jesterWindowRect.Bottom,$jesterScreen.Bottom)-$jesterTop
    $jesterBitmap=[System.Drawing.Bitmap]::new($jesterWidth,$jesterHeight)
    $jesterGraphics=[System.Drawing.Graphics]::FromImage($jesterBitmap)
    try {
        $jesterGraphics.CopyFromScreen($jesterLeft,$jesterTop,0,0,$jesterBitmap.Size)
        $jesterBitmap.Save([IO.Path]::GetFullPath($Path),[System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $jesterGraphics.Dispose();$jesterBitmap.Dispose() }
    [pscustomobject]@{ProcessId=$jesterProcess.Id;Width=$jesterWidth;Height=$jesterHeight;Path=[IO.Path]::GetFullPath($Path)} | ConvertTo-Json -Compress
} finally { [JesterWindowCapture]::SetThreadDpiAwarenessContext($jesterPreviousDpi) | Out-Null }
