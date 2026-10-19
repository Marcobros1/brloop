# Fill the FLSM open dialog and press "Abrir".
param([int]$TargetPid = 21612, [string]$FilePath)
Add-Type -AssemblyName UIAutomationClient

$proc = Get-Process -Id $TargetPid -ErrorAction Stop
$main = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker

function Find-ByName($el, $pattern, $ct) {
    $w = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $c = $w.GetFirstChild($el)
    while ($c) {
        if (($c.Current.Name -like $pattern) -and ($c.Current.ControlType -eq $ct)) { return $c }
        $r = Find-ByName $c $pattern $ct
        if ($r) { return $r }
        $c = $w.GetNextSibling($c)
    }
    return $null
}

$dialog = Find-ByName $main "Abrir" ([System.Windows.Automation.ControlType]::Window)
if (-not $dialog) { "dialog not found"; exit 1 }
"dialog found"

$edit = $null
$w = $walker
$c = $w.GetFirstChild($dialog)
function Find-Control($el, $ct) {
    $w = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $c = $w.GetFirstChild($el)
    while ($c) {
        if ($c.Current.ControlType -eq $ct) { return $c }
        $r = Find-Control $c $ct
        if ($r) { return $r }
        $c = $w.GetNextSibling($c)
    }
    return $null
}
$edit = Find-Control $dialog ([System.Windows.Automation.ControlType]::Edit)
if (-not $edit) { "edit not found"; exit 1 }
$vp = $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
$vp.SetValue($FilePath)
"value set: " + $vp.Current.Value
Start-Sleep -Milliseconds 400

$btn = Find-ByName $dialog "Abrir" ([System.Windows.Automation.ControlType]::Button)
if (-not $btn) { "Abrir button not found"; exit 1 }
try {
    $ip = $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $ip.Invoke()
    "invoked Abrir"
} catch {
    "invoke failed: $_"
    exit 1
}
Start-Sleep -Seconds 3
"--- windows after ---"
$main2 = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
$dlg = Find-ByName $main2 "Abrir" ([System.Windows.Automation.ControlType]::Window)
if ($dlg) { "dialog still open" } else { "dialog closed" }
