Add-Type -AssemblyName UIAutomationClient
$proc = Get-Process -Name "Final Lap Stream Maker" -ErrorAction SilentlyContinue
if (-not $proc) { "FLSM not running"; exit }
foreach ($p in $proc) {
    "PID $($p.Id) title='$($p.MainWindowTitle)' main=$($p.MainWindowHandle)"
    $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)
    $all = [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
    foreach ($el in $all) { "  window: class=$($el.Current.ClassName) name='$($el.Current.Name)'" }
}
