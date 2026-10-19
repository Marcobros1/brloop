Add-Type -AssemblyName UIAutomationClient
# All visible top-level windows
$root = [System.Windows.Automation.AutomationElement]::RootElement
$all = $root.FindAll([System.Windows.Automation.TreeScope]::Children,
    [System.Windows.Automation.Condition]::TrueCondition)
foreach ($el in $all) {
    $n = $el.Current.Name
    if ($n -ne "") { "pid=$($el.Current.ProcessId) class=$($el.Current.ClassName) name='$n'" }
}
