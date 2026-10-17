param(
    [string]$App = 'build-windows/package/LumenCode.Windows.exe',
    [Parameter(Mandatory=$true)][string]$Output,
    [switch]$DesktopInput
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (-not [IO.Path]::IsPathRooted($App)) { $App = Join-Path $repo $App }
if (-not [IO.Path]::IsPathRooted($Output)) { $Output = Join-Path $repo $Output }
if (-not (Test-Path $App)) { throw "Application not found: $App" }
New-Item -ItemType Directory -Force $Output | Out-Null
$name = 'LumenCode-GUI-Audit-' + [guid]::NewGuid().ToString('N')
$start = Get-Date
$arguments = '--gui-audit "' + $repo + '" "' + $Output + '"'
if ($DesktopInput) { $arguments += ' --desktop-input' }
$action = New-ScheduledTaskAction -Execute $App -Argument $arguments -WorkingDirectory (Split-Path $App)
$principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 4)
try {
    Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings | Out-Null
    Start-ScheduledTask -TaskName $name
    Start-Sleep -Seconds 2
    $deadline = (Get-Date).AddMinutes(4)
    while ((Get-ScheduledTask -TaskName $name).State -eq 'Running' -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
    $task = Get-ScheduledTaskInfo -TaskName $name
    $report = Join-Path $Output 'gui.json'
    if (-not (Test-Path $report) -or (Get-Item $report).LastWriteTime -lt $start) { throw "No new GUI report. Task exit: $($task.LastTaskResult)" }
    $result = Get-Content $report -Raw | ConvertFrom-Json
    $result | ConvertTo-Json -Depth 10
    if (-not $result.passed) { throw 'GUI assertions failed; inspect gui.json' }
} finally {
    Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
}
