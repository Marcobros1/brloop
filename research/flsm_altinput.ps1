# Alt trick to foreground + Tab until FLSM button focused, then real Space keystroke.
param([int]$TargetPid = 21612)
$sig = @'
using System;
using System.Runtime.InteropServices;
public class Fg {
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public MOUSEKEYBDHARDWAREINPUT u; }
    [StructLayout(LayoutKind.Explicit)]
    public struct MOUSEKEYBDHARDWAREINPUT {
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public MOUSEINPUT mi;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
    public const uint INPUT_KEYBOARD = 1;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public static void AltTap() {
        var down = new INPUT { type = INPUT_KEYBOARD };
        down.u.ki = new KEYBDINPUT { wVk = 0x12 };
        var up = new INPUT { type = INPUT_KEYBOARD };
        up.u.ki = new KEYBDINPUT { wVk = 0x12, dwFlags = KEYEVENTF_KEYUP };
        SendInput(1, new INPUT[] { down }, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(50);
        SendInput(1, new INPUT[] { up }, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void KeyTap(ushort vk) {
        var down = new INPUT { type = INPUT_KEYBOARD };
        down.u.ki = new KEYBDINPUT { wVk = vk };
        var up = new INPUT { type = INPUT_KEYBOARD };
        up.u.ki = new KEYBDINPUT { wVk = vk, dwFlags = KEYEVENTF_KEYUP };
        SendInput(1, new INPUT[] { down }, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(60);
        SendInput(1, new INPUT[] { up }, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
Add-Type -TypeDefinition $sig
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$proc = Get-Process -Id $TargetPid -ErrorAction Stop
[Fg]::ShowWindow($proc.MainWindowHandle, 9) | Out-Null
[Fg]::AltTap()
Start-Sleep -Milliseconds 200
$ok = [Fg]::SetForegroundWindow($proc.MainWindowHandle)
$fg = [Fg]::GetForegroundWindow()
"set=$ok fgIsTarget=$($fg -eq $proc.MainWindowHandle)"
if ($fg -ne $proc.MainWindowHandle) { "not foreground - aborting keystrokes"; exit 1 }
Start-Sleep -Milliseconds 500

for ($i = 0; $i -lt 8; $i++) {
    $focused = [System.Windows.Automation.AutomationElement]::FocusedElement
    $name = if ($focused) { $focused.Current.Name } else { "(none)" }
    "focus[$i]: $name"
    if ($name -like "Click here or drag and drop*") { break }
    [Fg]::KeyTap(0x09)
    Start-Sleep -Milliseconds 250
}
$focused = [System.Windows.Automation.AutomationElement]::FocusedElement
if ($focused -and $focused.Current.Name -like "Click here or drag and drop*") {
    [Fg]::KeyTap(0x20)
    "space sent"
} else {
    "button never focused"
}
Start-Sleep -Seconds 3

$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
$all = [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
foreach ($el in $all) { "window: class=$($el.Current.ClassName) name='$($el.Current.Name)'" }
