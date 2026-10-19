# Drive FLSM entirely with SendMessage (no focus stealing by our process):
# 1) BM_CLICK on main button  2) wait for Open dialog (child of main window)
# 3) WM_SETTEXT on filename edit  4) BM_CLICK 'Abrir'  5) report output files.
# NOTE: the OS may still briefly activate the dialog; run when the desktop is free.
param([int]$TargetPid = 21612, [string]$FilePath)
$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Snd {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, string lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr hWnd, EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern int GetClassName(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int max);
    public static string Cls(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
    public static string Txt(IntPtr h) { var sb = new StringBuilder(1024); GetWindowText(h, sb, 1024); return sb.ToString(); }
}
'@
Add-Type -TypeDefinition $sig
Add-Type -AssemblyName UIAutomationClient

$proc = Get-Process -Id $TargetPid -ErrorAction Stop
$mainHwnd = $proc.MainWindowHandle

function Find-DescendantByClass($rootHwnd, $cls) {
    $found = [IntPtr]::Zero
    $cb = [Snd+EnumProc]{
        param($h, $l)
        if ([Snd]::Cls($h) -eq $cls) { $script:found = $h; return $false }
        return $true
    }
    [void][Snd]::EnumChildWindows($rootHwnd, $cb, [IntPtr]::Zero)
    return $script:found
}

# 1) find main button hwnd and BM_CLICK it
$btnHwnd = [IntPtr]::Zero
$cb = [Snd+EnumProc]{
    param($h, $l)
    if ([Snd]::Cls($h) -like "WindowsForms10.BUTTON*") { $script:btnHwnd = $h; return $false }
    return $true
}
[void][Snd]::EnumChildWindows($mainHwnd, $cb, [IntPtr]::Zero)
if ($btnHwnd -eq [IntPtr]::Zero) { "main button not found"; exit 1 }
"button text: " + [Snd]::Txt($btnHwnd)
[void][Snd]::SendMessage($btnHwnd, 0x00F5, [IntPtr]1, [IntPtr]::Zero)  # BM_CLICK
Start-Sleep -Milliseconds 1500

# 2) locate Open dialog among descendants (native shell dialog is a child of main)
$dlg = [IntPtr]::Zero
$cb2 = [Snd+EnumProc]{
    param($h, $l)
    if ([Snd]::Cls($h) -eq "#32770") { $script:dlg = $h; return $false }
    return $true
}
[void][Snd]::EnumChildWindows($mainHwnd, $cb2, [IntPtr]::Zero)
if ($dlg -eq [IntPtr]::Zero) { "dialog did not open (BM_CLICK unsupported by WinForms?)"; exit 2 }
"dialog: " + [Snd]::Txt($dlg)

# 3) dump native children (diagnostic) and pick the filename Edit
$children = @()
$cb3 = [Snd+EnumProc]{
    param($h, $l)
    $script:children += [pscustomobject]@{ Hwnd = $h; Cls = [Snd]::Cls($h); Txt = [Snd]::Txt($h) }
    return $true
}
[void][Snd]::EnumChildWindows($dlg, $cb3, [IntPtr]::Zero)
$children | ForEach-Object { "  child: cls=$($_.Cls) text='$($_.Txt)'" }

$edits = $children | Where-Object { $_.Cls -eq "Edit" }
$fileEdit = $edits | Where-Object { $_.Txt -match "\.brstm|\.bcstm|\.bfstm" } | Select-Object -First 1
if (-not $fileEdit) { $fileEdit = $edits | Select-Object -Last 1 }
if (-not $fileEdit) { "no Edit child found"; exit 3 }
"using edit: '" + $fileEdit.Txt + "'"
[void][Snd]::SendMessage($fileEdit.Hwnd, 0x000C, [IntPtr]::Zero, $FilePath)  # WM_SETTEXT
Start-Sleep -Milliseconds 600

# 4) BM_CLICK the 'Abrir' button
$openBtn = $children | Where-Object { $_.Cls -eq "Button" -and $_.Txt -match "Abrir|Open" } | Select-Object -First 1
if (-not $openBtn) { "open button not found"; exit 4 }
[void][Snd]::SendMessage($openBtn.Hwnd, 0x00F5, [IntPtr]1, [IntPtr]::Zero)
Start-Sleep -Seconds 4

# 5) report: dialog closed? windows? output files next to input?
$dlg2 = [IntPtr]::Zero
$cb4 = [Snd+EnumProc]{
    param($h, $l)
    if ([Snd]::Cls($h) -eq "#32770") { $script:dlg2 = $h; return $false }
    return $true
}
[void][Snd]::EnumChildWindows($mainHwnd, $cb4, [IntPtr]::Zero)
if ($dlg2 -eq [IntPtr]::Zero) { "dialog closed" } else { "dialog still open: " + [Snd]::Txt($dlg2) }
$dir = Split-Path $FilePath
Get-ChildItem $dir | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
