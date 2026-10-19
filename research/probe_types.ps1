Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$t = [type]::GetType("System.Windows.Automation.LegacyIAccessiblePattern, UIAutomationClient")
"LegacyIAccessiblePattern in UIAutomationClient: " + ($t -ne $null)
$t2 = [type]::GetType("System.Windows.Automation.LegacyIAccessiblePattern, UIAutomationTypes")
"LegacyIAccessiblePattern in UIAutomationTypes: " + ($t2 -ne $null)
"InvokePattern: " + ([type]::GetType("System.Windows.Automation.InvokePattern, UIAutomationClient") -ne $null)
