#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Version,
    [string]$BuildDirectory = 'build/windows-release'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows) { throw 'Packaging requires Windows x64.' }
if ($Version -notmatch '^(?<base>\d+\.\d+\.\d+)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$') {
    throw 'Specify -Version as a semantic version, for example 0.1.0-beta.1 (without a v prefix).'
}
$baseVersion = $Matches.base
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else { [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory)) }
$releaseRoot = Join-Path $projectRoot "artifacts/releases/$Version"
$packageName = "3DGSDownloader-$Version-windows-x64"
$utf8 = [Text.UTF8Encoding]::new($false)

function Get-Hash([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Get-ProjectFile([string]$RelativePath) {
    if ([IO.Path]::IsPathRooted($RelativePath)) { throw 'Source manifest contains an absolute path.' }
    $resolved = [IO.Path]::GetFullPath((Join-Path $projectRoot $RelativePath))
    if (-not $resolved.StartsWith($projectRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Source manifest path leaves the repository.'
    }
    return $resolved
}
function Assert-X64Pe([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw "Not a PE file: $Path" }
        $stream.Position = 60
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 64 -or $peOffset -gt $stream.Length - 26) { throw "Invalid PE header: $Path" }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw "Not an AMD64 PE: $Path" }
        $stream.Position = $peOffset + 24
        if ($reader.ReadUInt16() -ne 0x20b) { throw "Not a PE32+ binary: $Path" }
    } finally { $reader.Dispose() }
}

# The output is always inside this repository. Never replace or recursively
# remove an existing release; reject reparse points along the output path.
$outputParent = $projectRoot
foreach ($segment in @('artifacts', 'releases', $Version)) {
    $outputParent = Join-Path $outputParent $segment
    if (Test-Path -LiteralPath $outputParent) {
        $item = Get-Item -LiteralPath $outputParent -Force
        if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Release output must use ordinary directories inside the repository.'
        }
    }
}
if (Test-Path -LiteralPath $releaseRoot) { throw "Release directory already exists; no files were overwritten: $releaseRoot" }
$cachePath = Join-Path $buildRoot 'CMakeCache.txt'
$cache = @{}
foreach ($line in Get-Content -LiteralPath $cachePath) {
    if ($line -match '^([^#/:][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
}
if ($cache['CMAKE_BUILD_TYPE'] -ne 'Release' -or $cache['CMAKE_PROJECT_NAME'] -ne '3DGSDownloader') {
    throw 'Select a Release build of 3DGSDownloader.'
}
if ([IO.Path]::GetFullPath($cache['CMAKE_HOME_DIRECTORY']) -ne $projectRoot) { throw 'Build belongs to another source directory.' }
if ($cache['CMAKE_PROJECT_VERSION'] -ne $baseVersion) { throw 'Package version must match the CMake project version.' }
Push-Location -LiteralPath $projectRoot
try {
    $sourceCommit = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $sourceCommit -notmatch '^[0-9a-f]{40}$') { throw 'Cannot identify the source commit.' }
    $changes = @(& git status --porcelain --untracked-files=normal)
    if ($LASTEXITCODE -ne 0 -or $changes.Count -ne 0) { throw 'Commit all source changes before packaging a release.' }
} finally { Pop-Location }

# A clean checkout alone does not prove that the executable is current. Match
# both the source manifest and all packaged binaries to a successful build.
$receipts = @()
$receiptRoot = Join-Path $projectRoot 'artifacts/build'
if (Test-Path -LiteralPath $receiptRoot -PathType Container) {
    foreach ($file in Get-ChildItem -LiteralPath $receiptRoot -Filter receipt.json -File -Recurse) {
        try {
            $candidate = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
            if ($candidate.configuration -eq 'Release' -and $candidate.result -eq 'passed' -and
                $candidate.sourceUnchangedDuringBuild -eq $true -and
                [IO.Path]::GetFullPath($candidate.buildDirectory) -eq $buildRoot) { $receipts += $candidate }
        } catch { continue }
    }
}
$receipt = $receipts | Sort-Object finishedAt -Descending | Select-Object -First 1
if ($null -eq $receipt) { throw 'No successful build receipt for this directory. Run tools/build.ps1 first.' }
if ([IO.Path]::GetFullPath($receipt.source) -ne $projectRoot -or @($receipt.sourceManifestAfter).Count -eq 0) {
    throw 'Build receipt does not identify this source tree.'
}
$toolsetVersion = $receipt.toolchain.vcToolsVersion.TrimEnd('\', '/')
if ($toolsetVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'Build receipt lacks the MSVC runtime version requirement.' }
foreach ($entry in $receipt.sourceManifestAfter) {
    if ((Get-Hash (Get-ProjectFile $entry.path)) -ne $entry.sha256) { throw "Source changed since the verified build: $($entry.path)" }
}
$bin = Join-Path $buildRoot 'bin'
$binaryNames = @('3DGSDownloader.exe', '3DGSDownloaderCli.exe', 'libSkiaSharp.dll')
foreach ($name in $binaryNames) {
    $binary = Join-Path $bin $name
    Assert-X64Pe $binary
    $matchingArtifacts = @($receipt.artifacts | Where-Object {
        $artifactPath = if ([IO.Path]::IsPathRooted($_.path)) { [IO.Path]::GetFullPath($_.path) } else { Get-ProjectFile $_.path }
        $artifactPath -eq $binary
    })
    if ($matchingArtifacts.Count -ne 1 -or (Get-Hash $binary) -ne $matchingArtifacts[0].sha256) {
        throw "Binary does not match the successful build receipt: $name"
    }
}
$dependency = Get-Content -LiteralPath (Join-Path $projectRoot 'third_party/dependencies.json') -Raw | ConvertFrom-Json
if ((Get-Hash (Join-Path $bin 'libSkiaSharp.dll')) -ne $dependency.skia.sha256) { throw 'Skia DLL differs from the dependency lock.' }
foreach ($notice in $dependency.skia.notices) {
    $noticePath = Get-ProjectFile $notice.path
    if ((Get-Hash $noticePath) -ne $notice.sha256 -or (Get-Item -LiteralPath $noticePath).Length -ne $notice.bytes) {
        throw "SkiaSharp notice differs from its package lock: $($notice.path)"
    }
}
foreach ($vendor in @('skia', 'vulkan')) {
    $vendorRoot = Join-Path $projectRoot "third_party/$vendor"
    $lock = Get-Content -LiteralPath (Join-Path $vendorRoot 'source-lock.json') -Raw | ConvertFrom-Json
    foreach ($entry in $lock.files) {
        if ((Get-Hash (Join-Path $vendorRoot $entry.path)) -ne $entry.sha256) { throw "Changed locked dependency: $vendor/$($entry.path)" }
    }
}

# Explicit file list: never copy the build directory or a user's workspace.
$payload = [ordered]@{
    '3DGSDownloader.exe' = (Join-Path $bin '3DGSDownloader.exe')
    '3DGSDownloaderCli.exe' = (Join-Path $bin '3DGSDownloaderCli.exe')
    'libSkiaSharp.dll' = (Join-Path $bin 'libSkiaSharp.dll')
    'LICENSE' = (Join-Path $projectRoot 'LICENSE')
    'licenses/Skia-LICENSE.txt' = (Join-Path $projectRoot 'third_party/skia/LICENSE')
    'licenses/SkiaSharp-LICENSE.txt' = (Join-Path $projectRoot 'third_party/skia/LICENSE-SkiaSharp.txt')
    'licenses/SkiaSharp-THIRD-PARTY-NOTICES.txt' = (Join-Path $projectRoot 'third_party/skia/THIRD-PARTY-NOTICES.txt')
    'licenses/Vulkan-Headers-LICENSE.md' = (Join-Path $projectRoot 'third_party/vulkan/LICENSE.md')
    'licenses/Apache-2.0.txt' = (Join-Path $projectRoot 'third_party/vulkan/LICENSES/Apache-2.0.txt')
    'licenses/MIT.txt' = (Join-Path $projectRoot 'third_party/vulkan/LICENSES/MIT.txt')
}
foreach ($source in $payload.Values) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing release input: $source" }
}
$stage = Join-Path $releaseRoot 'staging'
$expanded = Join-Path $releaseRoot 'verification'
[IO.Directory]::CreateDirectory($stage) | Out-Null
foreach ($entry in $payload.GetEnumerator()) {
    $destination = Join-Path $stage $entry.Key
    [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
    [IO.File]::Copy($entry.Value, $destination, $false)
}
$readme = [IO.File]::ReadAllText((Join-Path $projectRoot 'packaging/README.txt'))
$readme = $readme.Replace('{{VERSION}}', $Version).Replace('{{SOURCE_COMMIT}}', $sourceCommit).Replace('{{MSVC_TOOLSET_VERSION}}', $toolsetVersion)
[IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $readme, $utf8)
$manifest = [ordered]@{
    product = '3DGSDownloader'
    version = $Version
    architecture = 'windows-x64'
    sourceCommit = $sourceCommit
    configuration = 'Release'
    msvcToolsetVersion = $toolsetVersion
    buildIncludedCTest = [bool]$receipt.requestedTests
    files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/'); bytes = $_.Length; sha256 = Get-Hash $_.FullName }
    })
}
[IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), ($manifest | ConvertTo-Json -Depth 6) + "`n", $utf8)
$expected = @($manifest.files.path) + 'manifest.json'
$zipPath = Join-Path $releaseRoot "$packageName.zip"
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $false)
$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $entries = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    if (@(Compare-Object ($expected | Sort-Object) ($entries | Sort-Object)).Count -ne 0) { throw 'ZIP contains unexpected or missing files.' }
} finally { $archive.Dispose() }
[IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $expanded)
foreach ($entry in $manifest.files) {
    if ((Get-Hash (Join-Path $expanded $entry.path)) -ne $entry.sha256) { throw "ZIP roundtrip hash mismatch: $($entry.path)" }
}
if ((Get-Hash (Join-Path $expanded 'manifest.json')) -ne (Get-Hash (Join-Path $stage 'manifest.json'))) { throw 'ZIP manifest roundtrip failed.' }
$helpOutput = @(& (Join-Path $expanded '3DGSDownloaderCli.exe') --help 2>&1)
if ($LASTEXITCODE -ne 0 -or $helpOutput.Count -eq 0) { throw 'Extracted CLI --help smoke test failed.' }
[IO.File]::WriteAllText((Join-Path $releaseRoot 'SHA256SUMS'), "$(Get-Hash $zipPath)  $packageName.zip`n", $utf8)
[IO.File]::WriteAllText((Join-Path $releaseRoot 'verification.json'), ([ordered]@{
    zip = "$packageName.zip"; files = $expected.Count; roundtripHashes = 'passed'; extractedCliHelp = 'passed'
    guiInteraction = 'not run by packaging script'; sourceCommit = $sourceCommit
} | ConvertTo-Json) + "`n", $utf8)
Write-Output "Package: $zipPath"
Write-Output "Checksums: $(Join-Path $releaseRoot 'SHA256SUMS')"
Write-Output "Verified $($expected.Count) ZIP entries and extracted CLI --help. GUI interaction requires separate validation."
