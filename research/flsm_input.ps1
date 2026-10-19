# Bring FLSM to front and send Tab+Space with real SendInput keystrokes.
param([int]$TargetPid = 21612)
$sig = @'
using System;
using System.Runtime.InteropServices;
public class Foreground {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    public static bool ToFront(IntPtr hWnd) {
        ShowWindow(hWnd, 9); // SW_RESTORE
        IntPtr fg = GetForegroundWindow();
        uint fgThread = GetWindowThreadProcessId(fg, IntPtr.Zero);
        uint curThread = GetCurrentThreadId();
        AttachThreadInput(curThread, fgThread, true);
        bool ok = SetForegroundWindow(hWnd);
        AttachThreadInput(curThread, fgThread, false);
        return ok;
    }
}
'@
Add-Type -TypeDefinition $sig

$proc = Get-Process -Id $TargetPid -ErrorAction Stop
"front: " + [Foreground]::ToFront($proc.MainWindowHandle)
Start-Sleep -Milliseconds 800

$keySig = @'
using System;
using System.Runtime.InteropServices;
public class KeySend {
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [DllImport("user32.dll", SetLastError = true)] public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
    public const uint INPUT_KEYBOARD = 1;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public static void Tap(ushort vk) {
        var down = new INPUT { type = INPUT_KEYBOARD, ki = new KEYBDINPUT { wVk = vk } };
        var up = new INPUT { type = INPUT_KEYBOARD, ki = new KEYBDINPUT { wVk = vk, dwFlags = KEYEVENTF_KEYUP } };
        SendInput(1, new INPUT[] { down }, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(60);
        SendInput(1, new INPUT[] { up }, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
Add-Type -TypeDefinition $keySig
[KeySend]::Tap(0x09)   # VK_TAB
Start-Sleep -Milliseconds 300
[KeySend]::Tap(0x20)   # VK_SPACE
Start-Sleep -Seconds 3
"done"
