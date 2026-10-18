param(
    [Parameter(Mandatory=$true)][string]$App,
    [Parameter(Mandatory=$true)][string]$Repository,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference = 'Stop'
# Child process only: remove developer discovery paths without changing the
# user's environment. This cannot prove independence from installed components.
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
foreach ($name in @('DOTNET_ROOT','DOTNET_ROOT_X64','DOTNET_ROOT(x86)','QTDIR','QT5_PREFIX','QT_PLUGIN_PATH','QML2_IMPORT_PATH','PYTHONPATH','PYTHONHOME')) {
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
$arguments = '--gui-audit "' + $Repository + '" "' + $Output + '"'
$process = Start-Process -FilePath $App -ArgumentList $arguments -WorkingDirectory (Split-Path $App) -Wait -PassThru
exit $process.ExitCode
