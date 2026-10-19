# Real injected mouse click on FLSM button (SendInput), window foregrounded.
param([int]$TargetPid = 21612)
$sig = @'
using System;
using System.Runtime.InteropServices;
public class Mouse2 {
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
    public const uint INPUT_MOUSE = 0;
    public const uint INPUT_KEYBOARD = 1;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
    public static int Size() { return Marshal.SizeOf(typeof(INPUT)); }
    public static void AltTap() {
        var d = new INPUT { type = INPUT_KEYBOARD }; d.u.ki = new KEYBDINPUT { wVk = 0x12 };
        var u = new INPUT { type = INPUT_KEYBOARD }; u.u.ki = new KEYBDINPUT { wVk = 0x12, dwFlags = 0x0002 };
        SendInput(1, new INPUT[] { d }, Size());
        System.Threading.Thread.Sleep(50);
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
}
'@
Add-Type -TypeDefinition $sig
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$proc = Get-Process -Id $TargetPid -ErrorAction Stop
[void][Mouse2]::ShowWindow($proc.MainWindowHandle, 9)
[Mouse2]::AltTap()
Start-Sleep -Milliseconds 200
[void][Mouse2]::SetForegroundWindow($proc.MainWindowHandle)
$fg = [Mouse2]::GetForegroundWindow()
"fgIsTarget=$($fg -eq $proc.MainWindowHandle)"
if ($fg -ne $proc.MainWindowHandle) { "abort"; exit 1 }

$root = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$child = $walker.GetFirstChild($root)
$target = $null
while ($child) {
    if ($child.Current.Name -like "Click here or drag and drop*") { $target = $child; break }
    $child = $walker.GetNextSibling($child)
}
if (-not $target) { "button not found"; exit 1 }
$r = $target.Current.BoundingRectangle
$cx = [int]($r.X + $r.Width / 2)
$cy = [int]($r.Y + $r.Height / 2)
"clicking $cx,$cy"
[Mouse2]::ClickAt($cx, $cy)
Start-Sleep -Seconds 3

$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
$all = [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
foreach ($el in $all) { "window: class=$($el.Current.ClassName) name='$($el.Current.Name)'" }
