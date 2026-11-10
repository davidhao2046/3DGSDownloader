[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$VisualStudioRoot,
    [string]$BuildDirectory,
    [switch]$Verify,
    [switch]$Fresh,
    [switch]$Clean,
    [switch]$ConfigureOnly
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$preset = 'windows-' + $Configuration.ToLowerInvariant()
$customBuildDirectory = -not [string]::IsNullOrWhiteSpace($BuildDirectory)
if ($customBuildDirectory) {
    if ($BuildDirectory.IndexOfAny([char[]]@([char]0, [char]10, [char]13, [char]34)) -ge 0) { throw 'Invalid build directory' }
    $buildRoot = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
        [IO.Path]::GetFullPath($BuildDirectory)
    } else {
        [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
    }
    if ($buildRoot.TrimEnd('\','/') -eq $projectRoot.TrimEnd('\','/') -or
        $buildRoot.TrimEnd('\','/') -eq [IO.Path]::GetPathRoot($buildRoot).TrimEnd('\','/')) {
        throw 'Build directory must be a dedicated directory, not the source or drive root'
    }
} else {
    $buildRoot = Join-Path $projectRoot "build/$preset"
}
if ([string]::IsNullOrWhiteSpace($VisualStudioRoot)) {
    $installerRoot = ${env:ProgramFiles(x86)}
    if ([string]::IsNullOrWhiteSpace($installerRoot)) {
        throw 'Visual Studio discovery requires Windows x64. Specify -VisualStudioRoot for an existing installation.'
    }
    $vswhere = Join-Path $installerRoot 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio Installer discovery tool (vswhere.exe) was not found. Install Visual Studio with Desktop development with C++, or specify -VisualStudioRoot.'
    }
    $installations = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath)
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio discovery failed. Specify -VisualStudioRoot to select an installation.' }
    $VisualStudioRoot = $installations | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($VisualStudioRoot)) {
        throw 'No Visual Studio installation with MSVC x64 and C++ CMake tools was found. Add these components in Visual Studio Installer, or specify -VisualStudioRoot.'
    }
}
$vsRoot = [IO.Path]::GetFullPath($VisualStudioRoot)
if ($vsRoot.Contains('"') -or $vsRoot.Contains("`n") -or $vsRoot.Contains("`r")) { throw 'Invalid Visual Studio path' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) { throw "Missing MSVC x64 setup: $vcvars. Install the MSVC x64 build tools or check -VisualStudioRoot." }
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$ninja = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
foreach ($tool in @($cmake,$ctest,$ninja)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Missing build tool: $tool. Install C++ CMake tools for Windows in Visual Studio Installer." }
}
$stamp = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('yyyyMMdd-HHmmss-fff')
$logDir = Join-Path $projectRoot "artifacts\build\$($Configuration.ToLowerInvariant())-$stamp"
[IO.Directory]::CreateDirectory($logDir) | Out-Null
$receipt = [ordered]@{
    startedAt = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('o')
    configuration = $Configuration
    source = $projectRoot
    buildDirectory = $buildRoot
    visualStudioRoot = $vsRoot
    requestedTests = [bool]$Verify
    steps = @()
    result = 'in-progress'
}
function Save-Receipt {
    $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $logDir 'receipt.json') -Encoding utf8
}
function Get-SourceManifest {
    $paths = @('CMakeLists.txt','CMakePresets.json','graphics/CMakeLists.txt','graphics/provenance.json',
        'third_party/dependencies.json','third_party/skia/source-lock.json','third_party/vulkan/source-lock.json',
        'tools/build.ps1') | ForEach-Object { Join-Path $projectRoot $_ }
    foreach ($relative in @('apps','core','ui','resources','tests','cmake','infra/storage','infra/net','adapters','graphics/include','graphics/src','graphics/tests')) {
        $directory = Join-Path $projectRoot $relative
        if (Test-Path -LiteralPath $directory -PathType Container) {
            $paths += Get-ChildItem -LiteralPath $directory -File -Recurse | Where-Object { $_.FullName -notmatch '[\\/]build[\\/]' } | Select-Object -ExpandProperty FullName
        }
    }
    @($paths | Sort-Object -Unique | ForEach-Object {
        if (Test-Path -LiteralPath $_ -PathType Leaf) {
            [ordered]@{path=$_.Substring($projectRoot.Length+1).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant()}
        }
    })
}
function Invoke-Logged([string]$Name, [string]$Executable, [string[]]$Arguments) {
    $log = Join-Path $logDir "$Name.log"
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $log | Write-Host
    $code = $LASTEXITCODE
    $receipt.steps += [ordered]@{name=$Name; executable=$Executable; arguments=$Arguments; exitCode=$code; log=$log}
    Save-Receipt
    if ($code -ne 0) { throw "$Name failed with exit code $code (see $log)" }
}
try {
    # Read the developer environment into this PowerShell process. No file
    # operation is delegated across shells, and no global environment is changed.
    # Start with CALL so cmd receives a command instead of stripping the outer
    # quotes from an executable path that may contain spaces (for example VS
    # under Program Files). Capture the environment without printing its values.
    $lines = & $env:ComSpec /d /s /c "call `"$vcvars`" >nul && set" 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'MSVC environment setup failed' }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') }
    }
    $env:VSLANG = '1033'
    # CMake's MSVC include-prefix probe decodes using the console code page.
    # Match the UTF-8 compiler output, including a Chinese-only MSVC install.
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
    $OutputEncoding = [Console]::OutputEncoding
    $env:PATH = (Split-Path -Parent $cmake) + ';' + (Split-Path -Parent $ninja) + ';' + $env:PATH
    $compiler = (Get-Command cl.exe -ErrorAction Stop).Source
    $receipt.toolchain = [ordered]@{
        compiler = $compiler
        compilerFileVersion = (Get-Item -LiteralPath $compiler).VersionInfo.FileVersion
        cmake = (& $cmake --version | Select-Object -First 1)
        ninja = (& $ninja --version | Select-Object -First 1)
        vcToolsVersion = $env:VCToolsVersion
        windowsSdkVersion = $env:WindowsSDKVersion
        windowsSdkDir = $env:WindowsSdkDir
        architecture = $env:VSCMD_ARG_TGT_ARCH
        osVersion = [Environment]::OSVersion.VersionString
        compilerSha256 = (Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') { throw 'Compiler environment is not x64' }
    $receipt.sourceManifest = Get-SourceManifest
    Save-Receipt
    Push-Location -LiteralPath $projectRoot
    try {
        $configureArguments = @('--preset',$preset)
        if ($customBuildDirectory) { $configureArguments += @('-B',$buildRoot) }
        if ($Fresh) { $configureArguments += '--fresh' }
        Invoke-Logged 'configure' $cmake $configureArguments
        if (-not $ConfigureOnly) {
            $buildArguments = @('--build','--preset',$preset)
            if ($customBuildDirectory) { $buildArguments = @('--build',$buildRoot,'--config',$Configuration) }
            if ($Clean) { $buildArguments += '--clean-first' }
            Invoke-Logged 'build' $cmake $buildArguments
            $binDirectory = Join-Path $buildRoot 'bin'
            $receipt.artifacts = @(Get-ChildItem -LiteralPath $binDirectory -File | Where-Object { $_.Extension -in @('.exe','.dll') } | ForEach-Object {
                $artifactPath = $_.FullName
                if ($artifactPath.StartsWith($projectRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                    $artifactPath = $artifactPath.Substring($projectRoot.Length+1).Replace('\','/')
                }
                [ordered]@{path=$artifactPath; bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
            })
            if ($Verify) {
                $testArguments = @('--preset',$preset)
                if ($customBuildDirectory) {
                    $testArguments = @('--test-dir',$buildRoot,'-C',$Configuration,'--output-on-failure','--no-tests=error')
                }
                $testArguments += @('--output-junit',(Join-Path $logDir 'tests.xml'))
                Invoke-Logged 'ctest' $ctest $testArguments
                [xml]$testResults = Get-Content -LiteralPath (Join-Path $logDir 'tests.xml') -Raw
                if ([int]$testResults.testsuite.skipped -gt 0 -or [int]$testResults.testsuite.disabled -gt 0) {
                    throw 'Verification contains skipped/disabled tests; this is not a full pass'
                }
            }
        }
    } finally { Pop-Location }
    $receipt.sourceManifestAfter = Get-SourceManifest
    $receipt.sourceUnchangedDuringBuild = ($receipt.sourceManifest | ConvertTo-Json -Depth 4 -Compress) -eq ($receipt.sourceManifestAfter | ConvertTo-Json -Depth 4 -Compress)
    if (-not $receipt.sourceUnchangedDuringBuild) { throw 'Source changed during build; rerun to obtain an unambiguous verification receipt' }
    $receipt.result = 'passed'
} catch {
    $receipt.result = 'failed'
    $receipt.error = $_.Exception.Message
    throw
} finally {
    $receipt.finishedAt = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('o')
    Save-Receipt
    Write-Host "Build receipt: $logDir"
}

