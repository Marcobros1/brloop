# T0: run one real FLSM conversion end to end (open dialog + save dialog + result).
# Fills native dialogs with WM_SETTEXT/EM_SETTEXT via SendMessage; if verification
# fails, falls back to real injected input (this run was approved to take focus).
param(
    [string]$InputFile,
    [string]$OutputFile,
    [string]$ExePath = "$env:USERPROFILE\Desktop\MKWIIMUSIC\Final Lap Stream Maker.exe"
)
$ErrorActionPreference = 'Stop'
if (-not $InputFile)  { $InputFile  = Join-Path $env:LOCALAPPDATA 'Temp\opencode\flsm_exp\tKD_N.brstm' }
if (-not $OutputFile) { $OutputFile = Join-Path $env:LOCALAPPDATA 'Temp\opencode\flsm_exp\tKD_N_flsm_out.brstm' }

$sig = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class Snd {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, string lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr hWnd, EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern int GetClassName(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public InputUnion U; }
    [StructLayout(LayoutKind.Explicit)] public struct InputUnion {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
    }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    public const uint INPUT_KEYBOARD = 1;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public const uint KEYEVENTF_UNICODE = 0x0004;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
    public const byte VK_MENU = 0x12;
    public const byte VK_RETURN = 0x0D;
    public static string Cls(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
    public static string Txt(IntPtr h) { var sb = new StringBuilder(2048); GetWindowText(h, sb, 2048); return sb.ToString(); }
    public static List<IntPtr> Descendants(IntPtr root) {
        var list = new List<IntPtr>();
        EnumProc cb = (h, l) => { list.Add(h); return true; };
        EnumChildWindows(root, cb, IntPtr.Zero);
        return list;
    }
    public static IntPtr FindByClass(IEnumerable<IntPtr> hs, string cls) {
        foreach (var h in hs) if (Cls(h) == cls) return h;
        return IntPtr.Zero;
    }
    public static IntPtr FindButton(IEnumerable<IntPtr> hs, string pattern) {
        foreach (var h in hs) {
            if (Cls(h) == "Button" && System.Text.RegularExpressions.Regex.IsMatch(Txt(h), pattern,
                System.Text.RegularExpressions.RegexOptions.IgnoreCase)) return h;
        }
        return IntPtr.Zero;
    }
    // filename edit = Edit whose parent is the filename combobox (not the search box)
    public static IntPtr FindFileNameEdit(IEnumerable<IntPtr> hs) {
        IntPtr fallback = IntPtr.Zero;
        foreach (var h in hs) {
            if (Cls(h) != "Edit") continue;
            var p = GetParent(h);
            string pc = p == IntPtr.Zero ? "" : Cls(p);
            if (pc.StartsWith("ComboBox")) return h;
            if (fallback == IntPtr.Zero && !pc.Contains("DUIView") && !pc.Contains("Search")) fallback = h;
        }
        return fallback;
    }
    public static bool FillEdit(IntPtr edit, string text) {
        SendMessage(edit, 0x00B1, (IntPtr)0, (IntPtr)(-1));          // EM_SETSEL all
        SendMessage(edit, 0x000C, IntPtr.Zero, text);                // WM_SETTEXT
        return Txt(edit) == text;
    }
    public static void TypeUnicode(string s) {
        var inputs = new List<INPUT>();
        foreach (char c in s) {
            inputs.Add(new INPUT { type = INPUT_KEYBOARD, U = new InputUnion { ki = new KEYBDINPUT { wVk = 0, wScan = c, dwFlags = KEYEVENTF_UNICODE } } });
            inputs.Add(new INPUT { type = INPUT_KEYBOARD, U = new InputUnion { ki = new KEYBDINPUT { wVk = 0, wScan = c, dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP } } });
        }
        var arr = inputs.ToArray();
        SendInput((uint)arr.Length, arr, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void PressEnter() {
        keybd_event(VK_RETURN, 0, 0, UIntPtr.Zero);
        keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    }
    public static void ClickCenter(IntPtr hwnd) {
        RECT r; GetWindowRect(hwnd, out r);
        SetCursorPos((r.Left + r.Right) / 2, (r.Top + r.Bottom) / 2);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(60);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, UIntPtr.Zero);
    }
    public static void Foreground(IntPtr hwnd) {
        keybd_event(VK_MENU, 0, 0, UIntPtr.Zero);   // Alt down: unlocks foreground
        SetForegroundWindow(hwnd);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    }
}
'@
Add-Type -TypeDefinition $sig

function Find-Dialog([int]$pidF, [IntPtr]$mainH) {
    $found = [IntPtr]::Zero
    $cb = [Snd+EnumProc]{ param($h, $l)
        if ([Snd]::Cls($h) -eq "#32770") {
            [uint32]$p = 0
            [void][Snd]::GetWindowThreadProcessId($h, [ref]$p)
            if ($p -eq $pidF) { $script:found = $h; return $false }
        }
        return $true }
    [void][Snd]::EnumWindows($cb, [IntPtr]::Zero)
    if ($found -eq [IntPtr]::Zero) {
        $kids = [Snd]::Descendants($mainH)
        foreach ($h in $kids) { if ([Snd]::Cls($h) -eq "#32770") { $found = $h; break } }
    }
    return $found
}

# --- 0) prerequisites -------------------------------------------------------
if (-not (Test-Path $InputFile)) {
    $src = Join-Path (if ($env:MKW_COLLECTION) { $env:MKW_COLLECTION } else { Join-Path $env:APPDATA 'CT-MKWII\Mods\Metal Kart Wii Retro Rewind' }) 'tKD_N.brstm'
    Copy-Item $src $InputFile -Force
}
if (Test-Path $OutputFile) { Remove-Item $OutputFile -Force }

$proc = Get-Process -Name 'Final Lap Stream Maker' -ErrorAction SilentlyContinue
if (-not $proc) {
    Start-Process $ExePath | Out-Null
    Start-Sleep -Seconds 3
    $proc = Get-Process -Name 'Final Lap Stream Maker'
}
$proc = $proc | Select-Object -First 1
$mainH = [IntPtr]$proc.MainWindowHandle
"FLSM pid=$($proc.Id) main=$mainH"
$targetPid = $proc.Id

# --- 1) click main button (real input, proven to work) ----------------------
$btn = [IntPtr]::Zero
$cb = [Snd+EnumProc]{ param($h, $l)
    if ([Snd]::Cls($h) -like 'WindowsForms10.BUTTON*') { $script:btn = $h; return $false }
    return $true }
[void][Snd]::EnumChildWindows($mainH, $cb, [IntPtr]::Zero)
if ($btn -eq [IntPtr]::Zero) { "FAIL: main button not found"; exit 1 }
"button: '$([Snd]::Txt($btn))'"
[Snd]::Foreground($mainH)
Start-Sleep -Milliseconds 300
[Snd]::ClickCenter($btn)

$dlg = [IntPtr]::Zero
for ($i = 0; $i -lt 20 -and $dlg -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 300
    $dlg = Find-Dialog $targetPid $mainH
}
if ($dlg -eq [IntPtr]::Zero) { "FAIL: open dialog did not appear"; exit 2 }
"open dialog: '$([Snd]::Txt($dlg))'"

# --- 2) fill open dialog ----------------------------------------------------
$kids = [Snd]::Descendants($dlg)
$edit = [Snd]::FindFileNameEdit($kids)
if ($edit -eq [IntPtr]::Zero) { "FAIL: no filename edit"; exit 3 }
$okFill = [Snd]::FillEdit($edit, $InputFile)
"fill via WM_SETTEXT ok=$okFill text=''$([Snd]::Txt($edit))''"
if (-not $okFill) {  # fallback: real typing
    [Snd]::Foreground($edit)
    Start-Sleep -Milliseconds 200
    [Snd]::TypeUnicode($InputFile)
    Start-Sleep -Milliseconds 300
    "after typing text=''$([Snd]::Txt($edit))''"
}
$openBtn = [Snd]::FindButton($kids, '^(Abrir|Open)$')
if ($openBtn -eq [IntPtr]::Zero) { $openBtn = [Snd]::FindButton($kids, 'Abrir|Open') }
if ($openBtn -eq [IntPtr]::Zero) { "FAIL: Abrir button not found"; exit 4 }
"clicking: '$([Snd]::Txt($openBtn))'"
[void][Snd]::SendMessage($openBtn, 0x00F5, [IntPtr]1, [IntPtr]::Zero)   # BM_CLICK

# dialog must close (file valid) and a save dialog must appear
$closed = $false
for ($i = 0; $i -lt 20 -and -not $closed; $i++) {
    Start-Sleep -Milliseconds 300
    if ((Find-Dialog $targetPid $mainH) -eq [IntPtr]::Zero) { $closed = $true }
}
if (-not $closed) {
    "open dialog still open - fallback: Enter (default button accepts filename)"
    [Snd]::Foreground($edit); Start-Sleep -Milliseconds 150; [Snd]::PressEnter()
    for ($i = 0; $i -lt 20; $i++) { Start-Sleep -Milliseconds 300; if ((Find-Dialog $targetPid $mainH) -eq [IntPtr]::Zero) { break } }
}
"open dialog closed"

# --- 3) fill save dialog ----------------------------------------------------
$sdlg = [IntPtr]::Zero
for ($i = 0; $i -lt 30 -and $sdlg -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 300
    $sdlg = Find-Dialog $targetPid $mainH
}
if ($sdlg -eq [IntPtr]::Zero) { "FAIL: save dialog did not appear"; exit 5 }
"save dialog: '$([Snd]::Txt($sdlg))'"

$skids = [Snd]::Descendants($sdlg)
$sedit = [Snd]::FindFileNameEdit($skids)
if ($sedit -eq [IntPtr]::Zero) { "FAIL: no save filename edit"; exit 6 }
$okFill2 = [Snd]::FillEdit($sedit, $OutputFile)
"fill via WM_SETTEXT ok=$okFill2 text=''$([Snd]::Txt($sedit))''"
if (-not $okFill2) {
    [Snd]::Foreground($sedit)
    Start-Sleep -Milliseconds 200
    [Snd]::TypeUnicode($OutputFile)
    Start-Sleep -Milliseconds 300
    "after typing text=''$([Snd]::Txt($sedit))''"
}
$saveBtn = [Snd]::FindButton($skids, '^(Guardar|Save|Aceptar)$')
if ($saveBtn -eq [IntPtr]::Zero) { $saveBtn = [Snd]::FindButton($skids, 'Guardar|Save') }
if ($saveBtn -eq [IntPtr]::Zero) { "FAIL: Guardar button not found"; exit 7 }
"clicking: '$([Snd]::Txt($saveBtn))'"
[void][Snd]::SendMessage($saveBtn, 0x00F5, [IntPtr]1, [IntPtr]::Zero)

# --- 4) wait for conversion + result ---------------------------------------
$done = $false
for ($i = 0; $i -lt 50 -and -not $done; $i++) {
    Start-Sleep -Milliseconds 400
    if (Test-Path $OutputFile) { $done = $true }
}
if (-not $done) { "FAIL: output file never appeared"; exit 8 }
"output created: $OutputFile"

# any result/progress message box? capture then dismiss it
Start-Sleep -Milliseconds 800
$msg = Find-Dialog $targetPid $mainH
if ($msg -ne [IntPtr]::Zero) {
    "result box: '$([Snd]::Txt($msg))'"
    $mkids = [Snd]::Descendants($msg)
    $texts = $mkids | ForEach-Object { [Snd]::Txt($_) } | Where-Object { $_ } | Select-Object -First 10
    "  texts: $($texts -join ' | ')"
    $okBtn = [Snd]::FindButton($mkids, 'Aceptar|OK|Yes|Sí')
    if ($okBtn -ne [IntPtr]::Zero) { [void][Snd]::SendMessage($okBtn, 0x00F5, [IntPtr]1, [IntPtr]::Zero); "  dismissed" }
} else { "no message box pending" }

Get-Item $OutputFile | Select-Object FullName, Length, LastWriteTime | Format-List
"OK: T0 conversion finished"
