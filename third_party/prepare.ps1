[CmdletBinding()]
param(
    [string]$SkiaSource,
    [switch]$ReplaceLegacy
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json
$installed = Join-Path $PSScriptRoot 'installed'
$destination = Join-Path $installed 'libSkiaSharp.dll'
[IO.Directory]::CreateDirectory($installed) | Out-Null

function Test-LockedFile([string]$Path, [string]$Hash, [long]$Bytes) {
    (Test-Path -LiteralPath $Path -PathType Leaf) -and
        (Get-Item -LiteralPath $Path).Length -eq $Bytes -and
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Hash
}
foreach ($notice in $lock.skia.notices) {
    if (-not (Test-LockedFile (Join-Path $projectRoot $notice.path) $notice.sha256 $notice.bytes)) {
        throw "Changed or missing upstream notice: $($notice.path)"
    }
}
$existingHash = $null
if (Test-Path -LiteralPath $destination) {
    $existingHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($existingHash -ne $lock.skia.sha256) {
        if ($existingHash -ne $lock.skia.legacy_development_sha256) {
            throw 'Refusing to replace an unrecognized installed dependency.'
        }
        if (-not $ReplaceLegacy) {
            throw 'Legacy development DLL found. Run with -ReplaceLegacy to preserve a backup and install the locked official NuGet binary.'
        }
    }
}

$temporary = Join-Path $installed ('.skia-' + [Guid]::NewGuid().ToString('N') + '.tmp')
try {
    if (-not [string]::IsNullOrWhiteSpace($SkiaSource)) {
        $source = [IO.Path]::GetFullPath($SkiaSource)
        if (-not (Test-LockedFile $source $lock.skia.sha256 $lock.skia.bytes)) {
            throw 'The supplied -SkiaSource does not match the locked official native DLL.'
        }
        if ($existingHash -ne $lock.skia.sha256) { Copy-Item -LiteralPath $source -Destination $temporary }
    } elseif ($existingHash -ne $lock.skia.sha256) {
        $package = $lock.skia.package
        $cache = Join-Path $installed '.cache'
        [IO.Directory]::CreateDirectory($cache) | Out-Null
        $packagePath = Join-Path $cache ($package.id.ToLowerInvariant() + '.' + $package.version + '.nupkg')
        if (-not (Test-LockedFile $packagePath $package.sha256 $package.bytes)) {
            if (Test-Path -LiteralPath $packagePath) { throw 'Cached NuGet package differs from lock; remove that cache file and retry.' }
            $downloadPath = Join-Path $cache ([Guid]::NewGuid().ToString('N') + '.download')
            try {
                Invoke-WebRequest -Uri $package.url -OutFile $downloadPath -TimeoutSec 120
                if (-not (Test-LockedFile $downloadPath $package.sha256 $package.bytes)) {
                    throw 'Downloaded NuGet package differs from the package lock.'
                }
                Move-Item -LiteralPath $downloadPath -Destination $packagePath
            } finally {
                if (Test-Path -LiteralPath $downloadPath) { Remove-Item -LiteralPath $downloadPath }
            }
        }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [IO.Compression.ZipFile]::OpenRead($packagePath)
        try {
            $entries = @($zip.Entries | Where-Object { $_.FullName -ceq $package.entry })
            if ($entries.Count -ne 1 -or $entries[0].Length -ne $lock.skia.bytes) {
                throw 'NuGet package has an unexpected native DLL entry.'
            }
            # Extract only this exact entry to an owned path, never an archive path.
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[0], $temporary, $false)
        } finally { $zip.Dispose() }
    }
    if ($existingHash -ne $lock.skia.sha256) {
        if (-not (Test-LockedFile $temporary $lock.skia.sha256 $lock.skia.bytes)) {
            throw 'Prepared native DLL differs from lock; installed dependency was not changed.'
        }
        if ($existingHash) {
            $backup = Join-Path $installed ('libSkiaSharp.legacy-' + $existingHash + '.dll')
            if (Test-Path -LiteralPath $backup) {
                if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash.ToLowerInvariant() -ne $existingHash) {
                    throw 'Legacy backup path contains a different file.'
                }
            } else { Copy-Item -LiteralPath $destination -Destination $backup }
            # File.Replace keeps the existing DLL intact if publication fails.
            [IO.File]::Replace($temporary, $destination, [NullString]::Value)
        } else { [IO.File]::Move($temporary, $destination) }
    }
    if (-not (Test-LockedFile $destination $lock.skia.sha256 $lock.skia.bytes)) {
        throw 'Installed native DLL verification failed.'
    }
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
}
Write-Output 'Locked official SkiaSharp native DLL and upstream notices verified.'
