# Enumerate UI Automation tree of a running window (observable behavior only).
param([string]$ProcName = "Final Lap Stream Maker")

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$proc = Get-Process -Name $ProcName -ErrorAction Stop | Select-Object -First 1
"PID: $($proc.Id)  Title: $($proc.MainWindowTitle)"

$root = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
if (-not $root) { "No main window handle"; exit 1 }

function Dump-Element($el, $depth) {
    $pt = $el.Current.ControlType.ProgrammaticName
    $name = $el.Current.Name
    $aid = $el.Current.AutomationId
    $cls = $el.Current.ClassName
    $val = ""
    try {
        $vp = $el.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        if ($vp) { $val = " value='" + $vp.Current.Value + "'" } catch {}
    } catch {}
    "{0}{1} [{2}] name='{3}' id='{4}' class='{5}'{6}" -f ("  " * $depth), $pt, $el.Current.ControlType.ProgrammaticName.Replace("ControlType.",""), $name, $aid, $cls, $val
    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $child = $walker.GetFirstChild($el)
    while ($child) { Dump-Element $child ($depth + 1); $child = $walker.GetNextSibling($child) }
}

Dump-Element $root 0
