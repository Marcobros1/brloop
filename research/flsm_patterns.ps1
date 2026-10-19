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
"supported patterns:"
foreach ($p in $target.GetSupportedPatterns()) { "  " + $p.ProgrammaticName }
"supported attributes (sample):"
$acc = $target.GetCurrentPropertyValue([System.Windows.Automation.AutomationElement]::Identifiers.Patterns)
"legacy accessible check:"
try {
    $l = $target.GetCurrentPattern([System.Windows.Automation.AutomationElementIdentifiers]::LegacyIAccessiblePattern)
    "  obtained: $l"
} catch { "  failed: $_" }
