# Full FLSM drive: click button, wait for dialog, type path, press Enter, report results.
param([int]$TargetPid = 21612, [string]$FilePath)
$sig = @'
using System;
using System.Runtime.InteropServices;
public class Drive {
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public INPUTUNION u; }
    [StructLayout(LayoutKind.Explicit)]
    public struct INPUTUNION {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern short VkKeyScanW(char ch);
    public const uint INPUT_MOUSE = 0;
    public const uint INPUT_KEYBOARD = 1;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public static int Size() { return Marshal.SizeOf(typeof(INPUT)); }
    public static void AltTap() {
        var d = new INPUT { type = INPUT_KEYBOARD }; d.u.ki = new KEYBDINPUT { wVk = 0x12 };
        var u = new INPUT { type = INPUT_KEYBOARD }; u.u.ki = new KEYBDINPUT { wVk = 0x12, dwFlags = KEYEVENTF_KEYUP };
        SendInput(1, new INPUT[] { d }, Size());
        System.Threading.Thread.Sleep(50);
        SendInput(1, new INPUT[] { u }, Size());
    }
    public static void KeyTap(ushort vk) {
        var d = new INPUT { type = INPUT_KEYBOARD }; d.u.ki = new KEYBDINPUT { wVk = vk };
        var u = new INPUT { type = INPUT_KEYBOARD }; u.u.ki = new KEYBDINPUT { wVk = vk, dwFlags = KEYEVENTF_KEYUP };
        SendInput(1, new INPUT[] { d }, Size());
        System.Threading.Thread.Sleep(40);
        SendInput(1, new INPUT[] { u }, Size());
    }
    public static void ClickAt(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(150);
        var d = new INPUT { type = INPUT_MOUSE }; d.u.mi = new MOUSEINPUT { dwFlags = MOUSEEVENTF_LEFTDOWN };
        var u = new INPUT { type = INPUT_MOUSE }; u.u.mi = new MOUSEINPUT { dwFlags = MOUSEEVENTF_LEFTUP };
        SendInput(1, new INPUT[] { d }, Size());
        System.Threading.Thread.Sleep(80);
        SendInput(1, new INPUT[] { u }, Size());
    }
    public static void CtrlA() {
        var cd = new INPUT { type = INPUT_KEYBOARD }; cd.u.ki = new KEYBDINPUT { wVk = 0x11 };
        var ad = new INPUT { type = INPUT_KEYBOARD }; ad.u.ki = new KEYBDINPUT { wVk = 0x41 };
        var au = new INPUT { type = INPUT_KEYBOARD }; au.u.ki = new KEYBDINPUT { wVk = 0x41, dwFlags = KEYEVENTF_KEYUP };
        var cu = new INPUT { type = INPUT_KEYBOARD }; cu.u.ki = new KEYBDINPUT { wVk = 0x11, dwFlags = KEYEVENTF_KEYUP };
        SendInput(1, new INPUT[] { cd }, Size());
        System.Threading.Thread.Sleep(50);
        SendInput(1, new INPUT[] { ad }, Size());
        System.Threading.Thread.Sleep(50);
        SendInput(1, new INPUT[] { au }, Size());
        System.Threading.Thread.Sleep(50);
        SendInput(1, new INPUT[] { cu }, Size());
    }
    public static void TypeString(string s) {
        foreach (char ch in s) {
            short v = VkKeyScanW(ch);
            if (v == -1) continue;
            ushort vk = (ushort)(v & 0xFF);
            bool shift = (v & 0x100) != 0;
            if (shift) {
                var sd = new INPUT { type = INPUT_KEYBOARD }; sd.u.ki = new KEYBDINPUT { wVk = 0x10 };
                SendInput(1, new INPUT[] { sd }, Size());
            }
            var d = new INPUT { type = INPUT_KEYBOARD }; d.u.ki = new KEYBDINPUT { wVk = vk };
            var u = new INPUT { type = INPUT_KEYBOARD }; u.u.ki = new KEYBDINPUT { wVk = vk, dwFlags = KEYEVENTF_KEYUP };
            SendInput(1, new INPUT[] { d }, Size());
            System.Threading.Thread.Sleep(25);
            SendInput(1, new INPUT[] { u }, Size());
            if (shift) {
                var su = new INPUT { type = INPUT_KEYBOARD }; su.u.ki = new KEYBDINPUT { wVk = 0x10, dwFlags = KEYEVENTF_KEYUP };
                SendInput(1, new INPUT[] { su }, Size());
            }
            System.Threading.Thread.Sleep(20);
        }
    }
}
'@
Add-Type -TypeDefinition $sig
Add-Type -AssemblyName UIAutomationClient

$proc = Get-Process -Id $TargetPid -ErrorAction Stop

# 1. Foreground + click the main button
[void][Drive]::ShowWindow($proc.MainWindowHandle, 9)
[Drive]::AltTap()
Start-Sleep -Milliseconds 200
[void][Drive]::SetForegroundWindow($proc.MainWindowHandle)
$main = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$child = $walker.GetFirstChild($main)
$btn = $null
while ($child) {
    if ($child.Current.Name -like "Click here or drag and drop*") { $btn = $child; break }
    $child = $walker.GetNextSibling($child)
}
if (-not $btn) { "main button not found"; exit 1 }
$r = $btn.Current.BoundingRectangle
[Drive]::ClickAt([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
"clicked main button"

# 2. Wait for dialog (#32770) among descendants of main window
$dialog = $null
$deadline = (Get-Date).AddSeconds(10)
while ((Get-Date) -lt $deadline -and -not $dialog) {
    Start-Sleep -Milliseconds 300
    function Find-Win($el) {
        $w = [System.Windows.Automation.TreeWalker]::RawViewWalker
        $c = $w.GetFirstChild($el)
        while ($c) {
            if ($c.Current.ClassName -eq "#32770") { return $c }
            $r = Find-Win $c
            if ($r) { return $r }
            $c = $w.GetNextSibling($c)
        }
        return $null
    }
    $dialog = Find-Win $main
}
if (-not $dialog) { "dialog never appeared"; exit 1 }
"dialog appeared: " + $dialog.Current.Name
Start-Sleep -Milliseconds 800

# 3. Ensure dialog foreground, focus filename edit, type path, Enter
$dlgHwnd = $dialog.Current.NativeWindowHandle
if ($dlgHwnd) { [void][Drive]::SetForegroundWindow([IntPtr]$dlgHwnd) }
Start-Sleep -Milliseconds 300
function Find-EditEl($el) {
    $w = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $c = $w.GetFirstChild($el)
    while ($c) {
        if ($c.Current.ControlType -eq [System.Windows.Automation.ControlType]::Edit) { return $c }
        $r = Find-EditEl $c
        if ($r) { return $r }
        $c = $w.GetNextSibling($c)
    }
    return $null
}
$editEl = Find-EditEl $dialog
if ($editEl) {
    try { $editEl.SetFocus() } catch { "setfocus failed: $_" }
    Start-Sleep -Milliseconds 300
}
[Drive]::CtrlA()
Start-Sleep -Milliseconds 100
[Drive]::TypeString($FilePath)
Start-Sleep -Milliseconds 300
[Drive]::KeyTap(0x0D) # Enter
"typed path and Enter"
Start-Sleep -Seconds 4

# 4. Report windows now
function Find-Win2($el) {
    $w = [System.Windows.Automation.TreeWalker]::RawViewWalker
    $c = $w.GetFirstChild($el)
    while ($c) {
        if ($c.Current.ClassName -eq "#32770") { return $c }
        $r = Find-Win2 $c
        if ($r) { return $r }
        $c = $w.GetNextSibling($c)
    }
    return $null
}
$dlg2 = Find-Win2 $main
if ($dlg2) { "dialog still open: " + $dlg2.Current.Name } else { "dialog closed" }
