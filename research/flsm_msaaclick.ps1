# MSAA default action on the FLSM button (no focus required).
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
$legacy = $target.GetCurrentPattern([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern)
"state: " + $legacy.Current.State
"default action: '" + $legacy.Current.DefaultAction + "'"
$legacy.DoDefaultAction()
Start-Sleep -Seconds 3
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
$all = [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
foreach ($el in $all) { "window: class=$($el.Current.ClassName) name='$($el.Current.Name)'" }
