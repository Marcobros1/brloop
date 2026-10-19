# Fill the open-file dialog with the given path (step 2).
param([string]$FilePath)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$deadline = (Get-Date).AddSeconds(15)
$dialog = $null
while ((Get-Date) -lt $deadline -and -not $dialog) {
    $dlgProc = Get-Process -Name "Final Lap Stream Maker" -ErrorAction SilentlyContinue
    if ($dlgProc) {
        foreach ($w in $dlgProc.MainWindowHandle) {}
        # Find dialog by enumerating windows of the process via UIA
        $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $dlgProc.Id)
        $all = [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
        foreach ($el in $all) {
            if ($el.Current.ClassName -eq "#32770") { $dialog = $el; break }
        }
    }
    if (-not $dialog) { Start-Sleep -Milliseconds 500 }
}
if (-not $dialog) { "dialog not found"; exit 1 }
"dialog found: " + $dialog.Current.Name

# Find the file-name edit box (usually AutomationId '1148' in ComboBox/Edit)
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
function Find-Edits($el, $list) {
    $ct = $el.Current.ControlType
    if ($ct -eq [System.Windows.Automation.ControlType]::Edit -or $ct -eq [System.Windows.Automation.ControlType]::ComboBox) {
        $list += $el
    }
    $c = $walker.GetFirstChild($el)
    while ($c) { $list = Find-Edits $c $list; $c = $walker.GetNextSibling($c) }
    return $list
}
$edits = Find-Edits $dialog @()
foreach ($e in $edits) { "candidate: " + $e.Current.ControlType.ProgrammaticName + " id=" + $e.Current.AutomationId + " name=" + $e.Current.Name }

$edit = $edits | Where-Object { $_.Current.AutomationId -eq "1148" } | Select-Object -First 1
if (-not $edit) { $edit = $edits | Select-Object -First 1 }
if (-not $edit) { "no edit box"; exit 1 }

$vp = $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
$vp.SetValue($FilePath)
Start-Sleep -Milliseconds 500

# Click the Open button (AutomationId '1' typically)
function Find-Buttons($el, $list) {
    if ($el.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) { $list += $el }
    $c = $walker.GetFirstChild($el)
    while ($c) { $list = Find-Buttons $c $list; $c = $walker.GetNextSibling($c) }
    return $list
}
$buttons = Find-Buttons $dialog @()
foreach ($b in $buttons) { "button: id=" + $b.Current.AutomationId + " name=" + $b.Current.Name }
$open = $buttons | Where-Object { $_.Current.AutomationId -eq "1" } | Select-Object -First 1
if (-not $open) { $open = $buttons | Where-Object { $_.Current.Name -match "Open|Abrir" } | Select-Object -First 1 }
if ($open) {
    $ip = $open.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $ip.Invoke()
    "clicked open"
} else {
    "open button not found"
}
