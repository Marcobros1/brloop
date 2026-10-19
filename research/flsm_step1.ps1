# Invoke the FLSM main button and report new windows (step 1).
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

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
$p = $target.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
"invoking..."
$p.Invoke()
Start-Sleep -Seconds 2
"--- top-level windows now ---"
Get-Process | Where-Object { $_.MainWindowTitle -ne "" } | Select-Object Id, ProcessName, MainWindowTitle | Format-Table -AutoSize
