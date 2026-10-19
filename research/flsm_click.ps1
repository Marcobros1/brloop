# Click the FLSM main button via synthesized mouse click at its bounding rect.
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -PresentationCore

$proc = Get-Process -Name "Final Lap Stream Maker" | Select-Object -First 1
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
"rect: $($r.X),$($r.Y) $($r.Width)x$($r.Height)"
$cx = [int]($r.X + $r.Width / 2)
$cy = [int]($r.Y + $r.Height / 2)

$signature = @'
using System;
using System.Runtime.InteropServices;
public class MouseOps {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, int dwExtraInfo);
    public const uint LEFTDOWN = 0x0002;
    public const uint LEFTUP = 0x0004;
    public static void Click(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(100);
        mouse_event(LEFTDOWN, 0, 0, 0, 0);
        System.Threading.Thread.Sleep(50);
        mouse_event(LEFTUP, 0, 0, 0, 0);
    }
}
'@
Add-Type -TypeDefinition $signature
[void][MouseOps]::SetForegroundWindow($proc.MainWindowHandle)
Start-Sleep -Milliseconds 500
[MouseOps]::Click($cx, $cy)
Start-Sleep -Seconds 2
"--- windows of FLSM process ---"
Add-Type -AssemblyName UIAutomationClient
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
$all = [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
foreach ($el in $all) { "window: class=$($el.Current.ClassName) name='$($el.Current.Name)'" }
