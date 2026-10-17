param(
    [Parameter(Mandatory=$true)][string]$App,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference = 'Stop'
# Runs only as the interactive desktop user, alongside the optional input audit.
# UI Automation targets the exact executable and report path owned by that run.
$report = Join-Path $Output 'input-focus.json'
try {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class LumenCodeAuditForeground {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
'@
    $deadline = (Get-Date).AddSeconds(20)
    $appProcess = $null
    do {
        $auditProcesses = @(Get-CimInstance Win32_Process -Filter "name='LumenCode.Windows.exe'" | Where-Object {
            $_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq $App -and
            $_.CommandLine.Contains('--gui-audit') -and $_.CommandLine.Contains('"' + $Output + '"')
        })
        if ($auditProcesses.Count -gt 1) { throw 'Ambiguous audit processes; no focus change attempted.' }
        if ($auditProcesses.Count -eq 1) {
            $appProcess = Get-Process -Id $auditProcesses[0].ProcessId -ErrorAction SilentlyContinue
            if ($appProcess -and $appProcess.MainWindowHandle -ne 0) {
                $window = [Windows.Automation.AutomationElement]::FromHandle($appProcess.MainWindowHandle)
                $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty, 'Search files and symbols')
                $search = $window.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
                if ($search) {
                    $search.SetFocus()
                    # Activation can be dispatched after UIA SetFocus returns.
                    $activationDeadline = (Get-Date).AddSeconds(2)
                    while ([LumenCodeAuditForeground]::GetForegroundWindow() -ne $appProcess.MainWindowHandle -and (Get-Date) -lt $activationDeadline) {
                        Start-Sleep -Milliseconds 50
                    }
                    if ([LumenCodeAuditForeground]::GetForegroundWindow() -ne $appProcess.MainWindowHandle) {
                        throw 'UI Automation focus returned without activating the audit window.'
                    }
                    [ordered]@{passed=$true;method='UI Automation SetFocus on audit search';process=$appProcess.Id} |
                        ConvertTo-Json | Set-Content -Encoding UTF8 $report
                    exit 0
                }
            }
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)
    throw 'The owned audit search control was not available before the focus deadline.'
} catch {
    [ordered]@{passed=$false;failure=$_.ToString()} | ConvertTo-Json | Set-Content -Encoding UTF8 $report
    exit 1
}
