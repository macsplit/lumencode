param(
    [Parameter(Mandatory=$true)][string]$Archive,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (-not [IO.Path]::IsPathRooted($Archive)) { $Archive = Join-Path $repo $Archive }
if (-not [IO.Path]::IsPathRooted($Output)) { $Output = Join-Path $repo $Output }
New-Item -ItemType Directory -Force $Output | Out-Null
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('LumenCode extracted package ' + [guid]::NewGuid().ToString('N'))
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive, $temporary)
    $package = Join-Path $temporary 'LumenCode'
    $manifest = Get-Content (Join-Path $package 'package-manifest.json') -Raw | ConvertFrom-Json
    foreach ($file in $manifest.files) {
        $path = [IO.Path]::GetFullPath((Join-Path $package $file.path))
        if (-not $path.StartsWith($package + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escapes the package' }
        if ((Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256 -or (Get-Item $path).Length -ne $file.bytes) { throw "Package hash mismatch: $($file.path)" }
    }
    if (@(Get-ChildItem $package -File -Recurse).Count -ne $manifest.files.Count + 1) { throw 'Unexpected files in portable package' }
    & "$PSScriptRoot/run_windows_gui_audit.ps1" -App "$package/LumenCode.Windows.exe" -Output $Output -CleanEnvironment
    $gui = Get-Content (Join-Path $Output 'gui.json') -Raw | ConvertFrom-Json
    [ordered]@{
        passed=[bool]$gui.passed
        archiveSha256=(Get-FileHash $Archive -Algorithm SHA256).Hash.ToLowerInvariant()
        filesVerified=$manifest.files.Count
        sourceCommit=$manifest.sourceCommit
        sourceWorktreeDirty=$manifest.sourceWorktreeDirty
        guiChecks=$gui.checks.Count
        executableSignature=(Get-AuthenticodeSignature "$package/LumenCode.Windows.exe").Status.ToString()
        limitations='Fresh extraction and child process without developer discovery paths on the existing VM. Installed system components remain; not clean-machine, installer, signing or SmartScreen acceptance.'
    } | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Output 'portable.json')
} finally {
    if (Test-Path $temporary) { Remove-Item $temporary -Recurse -Force }
}
