param([int]$TargetPid = 21612)
Add-Type -AssemblyName UIAutomationClient
$proc = Get-Process -Id $TargetPid -ErrorAction Stop
$root = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
function Walk($el, $depth) {
    if ($depth -gt 4) { return }
    $w = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $c = $w.GetFirstChild($el)
    while ($c) {
        "$("  " * $depth)class=$($c.Current.ClassName) name='$($c.Current.Name)' rect=$($c.Current.BoundingRectangle)"
        Walk $c ($depth + 1)
        $c = $w.GetNextSibling($c)
    }
}
Walk $root 0
"--- raw view descendants windows ---"
$raw = New-Object System.Windows.Automation.TreeWalker([System.Windows.Automation.TreeViewCondition]::RawViewCondition)
function WalkRaw($el, $depth) {
    if ($depth -gt 3) { return }
    $w = [System.Windows.Automation.TreeWalker]::RawViewWalker
    $c = $w.GetFirstChild($el)
    while ($c) {
        "$("  " * $depth)class=$($c.Current.ClassName) name='$($c.Current.Name)'"
        WalkRaw $c ($depth + 1)
        $c = $w.GetNextSibling($c)
    }
}
WalkRaw $root 0
