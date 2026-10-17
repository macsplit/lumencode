param(
    [string]$Qt = $env:QT5_PREFIX,
    [string]$Build = 'build-windows',
    [string]$Output = 'build-windows/package',
    [string]$Dotnet = 'dotnet',
    [switch]$EngineOnly
)
$ErrorActionPreference = 'Stop'
function Invoke-Checked([string]$Program, [string[]]$ToolArguments) {
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Program @ToolArguments 2>&1 | ForEach-Object { $_.ToString() }; $result = $LASTEXITCODE }
    finally { $ErrorActionPreference = $previousPreference }
    if ($result -ne 0) { throw "$Program exited with status $result" }
}
$repo = Split-Path $PSScriptRoot -Parent
if (-not $Qt) { $Qt = "$env:USERPROFILE\lumencode-tools\Qt\5.15.2\msvc2019_64" }
if (-not (Test-Path "$Qt\lib\cmake\Qt5\Qt5Config.cmake")) { throw 'Supply -Qt <Qt 5 MSVC x64 kit> or QT5_PREFIX.' }
if (-not [IO.Path]::IsPathRooted($Build)) { $Build = Join-Path $repo $Build }
if (-not [IO.Path]::IsPathRooted($Output)) { $Output = Join-Path $repo $Output }
$manifest = Get-Content "$repo\third_party\provenance.json" -Raw | ConvertFrom-Json
foreach ($record in $manifest.files) {
    if ((Get-FileHash (Join-Path $repo $record.path) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $record.sha256) {
        throw "Vendored source differs: $($record.path). Preserve Git line endings; do not regenerate grammars."
    }
}
Invoke-Checked 'cmake' @('-S', $repo, '-B', $Build, '-G', 'Visual Studio 17 2022', '-A', 'x64', '-DLUMENCODE_BUILD_QT_APP=OFF', '-DLUMENCODE_BUILD_CLI=ON', "-DCMAKE_PREFIX_PATH=$Qt")
Invoke-Checked 'cmake' @('--build', $Build, '--config', 'Release', '--parallel', '4')
$runtime = Join-Path $Output 'EngineRuntime'
New-Item -ItemType Directory -Force $runtime | Out-Null
Copy-Item "$Build\bin\Release\lumencode-engine.exe" $runtime -Force
Copy-Item "$Build\bin\Release\lumencode-cli.exe" $runtime -Force
Invoke-Checked "$Qt\bin\windeployqt.exe" @('--release', '--no-compiler-runtime', '--no-translations', '--no-opengl-sw', "$runtime\lumencode-cli.exe", "$runtime\lumencode-engine.exe")
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$redistVersion = (Get-Content "$vs\VC\Auxiliary\Build\Microsoft.VCRedistVersion.default.txt" -Raw).Trim()
$crt = "$vs\VC\Redist\MSVC\$redistVersion\x64\Microsoft.VC143.CRT"
if (-not (Test-Path $crt)) { throw "MSVC x64 runtime not found: $crt" }
Copy-Item "$crt\*.dll" $runtime -Force
# Keep the headless CLI available for parity tests without an interactive desktop.
New-Item -ItemType Directory -Force "$runtime\platforms" | Out-Null
Copy-Item "$Qt\plugins\platforms\qoffscreen.dll" "$runtime\platforms" -Force
$licenses = Join-Path $runtime 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
Copy-Item "$repo\LICENSE" "$licenses\LumenCode-GPL-3.0.txt" -Force
Copy-Item "$repo\THIRD_PARTY_NOTICES.md" $licenses -Force
Get-ChildItem "$Qt\LICENSE*" -File | Copy-Item -Destination $licenses -Force
Get-ChildItem "$repo\third_party" -Recurse -File | Where-Object { $_.Name -match '^(LICENSE|COPYING|COPYRIGHT)' } | ForEach-Object {
    $relative = $_.FullName.Substring((Join-Path $repo 'third_party').Length + 1)
    $destination = Join-Path "$licenses\third_party" $relative
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    Copy-Item $_.FullName $destination -Force
}
$sources = @{}
Get-ChildItem "$repo\engine" -Recurse -File | Where-Object { $_.Extension -in '.cpp','.h' } | ForEach-Object {
    $sources[$_.FullName.Substring($repo.Length+1).Replace('\','/')] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
@{ monorepo_commit = (git -C $repo rev-parse HEAD); engine_sources = $sources; third_party = $manifest; qt = (& "$Qt\bin\qmake.exe" -query QT_VERSION) } | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 "$runtime\source-provenance.json"
if (-not $EngineOnly) {
    Invoke-Checked $Dotnet @('publish', "$repo\apps\windows\LumenCode.Windows\LumenCode.Windows.csproj", '-c', 'Release', '-p:Platform=x64', '--self-contained', 'true', '-o', $Output)
}
Write-Output "Windows package: $Output"
