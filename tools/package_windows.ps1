param(
    [string]$Package = 'build-windows/package',
    [string]$OutputDirectory = 'build-windows/distribution',
    [string]$Name = 'LumenCode-Windows-x64-development'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (-not [IO.Path]::IsPathRooted($Package)) { $Package = Join-Path $repo $Package }
if (-not [IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory = Join-Path $repo $OutputDirectory }
$Package = [IO.Path]::GetFullPath($Package)
foreach ($required in @('LumenCode.Windows.exe','LumenCode.Windows.pri','MainWindow.xbf','App.xbf','EngineRuntime/lumencode-engine.exe','EngineRuntime/licenses/LumenCode-GPL-3.0.txt')) {
    if (-not (Test-Path (Join-Path $Package $required) -PathType Leaf)) { throw "Missing package input: $required" }
}
if ($Name -notmatch '^[A-Za-z0-9._-]+$') { throw 'Name must be a plain archive filename' }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$archive = Join-Path $OutputDirectory ($Name + '.zip')
$checksum = $archive + '.sha256'
if ((Test-Path $archive) -or (Test-Path $checksum)) { throw 'Archive or checksum already exists; choose a new Name' }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('LumenCode portable ' + [guid]::NewGuid().ToString('N'))
try {
    $staged = Join-Path $temporary 'LumenCode'
    New-Item -ItemType Directory $staged | Out-Null
    Copy-Item (Join-Path $Package '*') $staged -Recurse
    @'
LumenCode Windows development build

Extract this entire ZIP into a writable folder, then run LumenCode.Windows.exe.
Keep EngineRuntime and all other files beside the executable. No installer or
separate .NET/Qt installation is intended. This build is unsigned.

Open a source folder to begin. Large source previews have Previous/Next pages;
Copy copies the full loaded preview. Files beyond the preview limit can be
opened in your editor. Analysis always uses the shared engine and full file.

Licences and source provenance are in EngineRuntime. Obtain corresponding
source from https://github.com/macsplit/lumencode at the recorded source commit.
'@ | Set-Content -Encoding UTF8 (Join-Path $staged 'README.txt')
    $sourceCommit = (& git -C $repo rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source commit' }
    $dirty = [bool](& git -C $repo status --porcelain)
    $files = @(Get-ChildItem $staged -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($staged.Length + 1).Replace('\','/');sha256=(Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant();bytes=$_.Length}
    })
    [ordered]@{sourceCommit=$sourceCommit;sourceWorktreeDirty=$dirty;architecture='x64';signed=$false;files=$files} |
        ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $staged 'package-manifest.json')
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($temporary, $archive, [IO.Compression.CompressionLevel]::Optimal, $false)
    $digest = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    "$digest  $Name.zip" | Set-Content -Encoding ASCII $checksum
    [ordered]@{archive=$archive;sha256=$digest;sourceCommit=$sourceCommit;sourceWorktreeDirty=$dirty;files=$files.Count;signed=$false} | ConvertTo-Json
} finally {
    if (Test-Path $temporary) { Remove-Item $temporary -Recurse -Force }
}
