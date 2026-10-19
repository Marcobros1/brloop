# Send LBUTTON down/up directly to the FLSM button child window.
Add-Type -AssemblyName UIAutomationClient

$proc = Get-Process -Name "Final Lap Stream Maker" | Select-Object -First 1
$hMain = $proc.MainWindowHandle

$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Win32Poke {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr hWnd, EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern int GetClassName(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public static IntPtr FindButton(IntPtr main) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(main, (h, l) => {
            var cls = new StringBuilder(256);
            GetClassName(h, cls, 256);
            if (cls.ToString().StartsWith("WindowsForms10.BUTTON")) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static string Text(IntPtr h) {
        var sb = new StringBuilder(512); GetWindowText(h, sb, 512); return sb.ToString();
    }
    public static bool ClickCenter(IntPtr h) {
        RECT r;
        if (!GetWindowRect(h, out r)) return false;
        int cx = (r.Left + r.Right) / 2 - r.Left;
        int cy = (r.Top + r.Bottom) / 2 - r.Top;
        IntPtr lp = (IntPtr)((cy << 16) | (cx & 0xFFFF));
        bool ok = PostMessage(h, 0x0201, (IntPtr)1, lp);   // WM_LBUTTONDOWN MK_LBUTTON
        System.Threading.Thread.Sleep(80);
        ok &= PostMessage(h, 0x0202, IntPtr.Zero, lp);     // WM_LBUTTONUP
        return ok;
    }
}
'@
Add-Type -TypeDefinition $sig
$btn = [Win32Poke]::FindButton($hMain)
if ($btn -eq [IntPtr]::Zero) { "button hwnd not found"; exit 1 }
"text: " + [Win32Poke]::Text($btn)
"posted: " + [Win32Poke]::ClickCenter($btn)
Start-Sleep -Seconds 3
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
$all = [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
foreach ($el in $all) { "window: class=$($el.Current.ClassName) name='$($el.Current.Name)'" }
