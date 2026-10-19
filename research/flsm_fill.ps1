# Fill shell Open dialog via WM_SETTEXT + BM_CLICK (native messages, no focus needed).
param([int]$TargetPid = 21612, [string]$FilePath)
Add-Type -AssemblyName UIAutomationClient

$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Native {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, string lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int max);
    public static string Text(IntPtr h) { var sb = new StringBuilder(512); GetWindowText(h, sb, 512); return sb.ToString(); }
}
'@
Add-Type -TypeDefinition $sig

$proc = Get-Process -Id $TargetPid -ErrorAction Stop
$main = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)

function Find-ByCt($el, $ct) {
    $w = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $c = $w.GetFirstChild($el)
    while ($c) {
        if ($c.Current.ControlType -eq $ct) { return $c }
        $r = Find-ByCt $c $ct
        if ($r) { return $r }
        $c = $w.GetNextSibling($c)
    }
    return $null
}
function Find-ByName($el, $pattern, $ct) {
    $w = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $c = $w.GetFirstChild($el)
    while ($c) {
        if (($c.Current.Name -like $pattern) -and ($c.Current.ControlType -eq $ct)) { return $c }
        $r = Find-ByName $c $pattern $ct
        if ($r) { return $r }
        $c = $w.GetNextSibling($c)
    }
    return $null
}

$dialog = Find-ByName $main "Abrir" ([System.Windows.Automation.ControlType]::Window)
if (-not $dialog) { "dialog not found"; exit 1 }
"dialog ok"

$edit = Find-ByCt $dialog ([System.Windows.Automation.ControlType]::Edit)
if (-not $edit) { "edit not found"; exit 1 }
$editHwnd = [IntPtr]$edit.Current.NativeWindowHandle
"edit hwnd: $editHwnd  current text: '" + [Native]::Text($editHwnd) + "'"
[void][Native]::SendMessage($editHwnd, 0x000C, [IntPtr]::Zero, $FilePath)  # WM_SETTEXT
Start-Sleep -Milliseconds 400
"text after: '" + [Native]::Text($editHwnd) + "'"

$btn = Find-ByName $dialog "Abrir" ([System.Windows.Automation.ControlType]::Button)
if (-not $btn) { "Abrir not found"; exit 1 }
$btnHwnd = [IntPtr]$btn.Current.NativeWindowHandle
"btn hwnd: $btnHwnd"
[void][Native]::SendMessage($btnHwnd, 0x00F5, [IntPtr]1, [IntPtr]::Zero)  # BM_CLICK
Start-Sleep -Seconds 4

$dialog2 = Find-ByName $main "Abrir" ([System.Windows.Automation.ControlType]::Window)
if ($dialog2) { "dialog still open" } else { "dialog closed - check output" }
