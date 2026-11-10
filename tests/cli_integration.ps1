[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$EvidenceDirectory
)
$ErrorActionPreference = 'Stop'
$cliExecutable = [IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $cliExecutable -PathType Leaf)) { throw "CLI missing: $cliExecutable" }
if (-not $EvidenceDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $EvidenceDirectory = Join-Path $PSScriptRoot "../artifacts/validation/p1/cli-$stamp"
}
$testDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
[IO.Directory]::CreateDirectory($testDirectory) | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)
$steps = New-Object 'System.Collections.Generic.List[object]'
$assertions = New-Object 'System.Collections.Generic.List[string]'
$fileHashes = New-Object 'System.Collections.Generic.List[object]'
$receipt = [ordered]@{
    startedAt = [DateTimeOffset]::Now.ToString('o')
    executable = $cliExecutable
    executableSha256 = (Get-FileHash -LiteralPath $cliExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
    evidenceDirectory = $testDirectory
    result = 'in-progress'
}
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $assertions.Add($Message)
}
function Get-Hash([string]$Path) {
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    $fileHashes.Add([ordered]@{path=$Path; sha256=$hash; observedAt=[DateTimeOffset]::Now.ToString('o')})
    return $hash
}
function Quote-Argument([string]$Value) {
    # CommandLineToArgvW-compatible quoting; no shell is involved.
    '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}
function Invoke-Cli([string]$Name, [string[]]$CliArguments, [int]$ExpectedExit = 0) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $cliExecutable
    $start.Arguments = (($CliArguments | ForEach-Object { Quote-Argument $_ }) -join ' ')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = $utf8
    $start.StandardErrorEncoding = $utf8
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw "Cannot start $Name" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw "$Name exceeded 30 second deadline"
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $exitCode = $process.ExitCode
    } finally { $process.Dispose() }
    $stdoutPath = Join-Path $testDirectory "$Name.stdout.json"
    $stderrPath = Join-Path $testDirectory "$Name.stderr.txt"
    [IO.File]::WriteAllText($stdoutPath,$stdout,$utf8)
    [IO.File]::WriteAllText($stderrPath,$stderr,$utf8)
    $steps.Add([ordered]@{name=$Name; arguments=$CliArguments; expectedExit=$ExpectedExit; exitCode=$exitCode; stdout=$stdoutPath; stderr=$stderrPath})
    Assert-True ($exitCode -eq $ExpectedExit) "$Name exit code is $ExpectedExit"
    $payload = $stdout | ConvertFrom-Json
    Assert-True ($payload.ok -eq ($ExpectedExit -eq 0)) "$Name JSON success matches exit code"
    return $payload
}
try {
    $emptyPath = Join-Path $testDirectory '空计划.3dgsdl.json'
    $empty = Invoke-Cli '01-empty' @('new','--out',$emptyPath)
    Assert-True ($empty.plan.resources.Count -eq 0 -and -not $empty.plan.internalFixture) 'Default new plan has no fake source results'
    $fixturePath = Join-Path $testDirectory '中文样本.3dgsdl.json'
    $created = Invoke-Cli '02-fixture' @('new','--fixture','--out',$fixturePath)
    Assert-True ($created.plan.schemaVersion -eq 1 -and $created.plan.internalFixture) 'Fixture schema and fixture marker are explicit'
    Assert-True (($created.plan.resources.id -join ',') -eq 'model,cameras,voxel') 'Stable fixture IDs are model,cameras,voxel'
    $fixtureHash = Get-Hash $fixturePath
    $opened = Invoke-Cli '03-reopen' @('inspect',$fixturePath)
    Assert-True (-not $opened.state.dirty -and $opened.state.undoCount -eq 0 -and $opened.state.redoCount -eq 0) 'Reopen starts clean with empty history'
    $editedPath = Join-Path $testDirectory '编辑后.3dgsdl.json'
    $outputDirectory = Join-Path $testDirectory '下载 中文目录'
    $edited = Invoke-Cli '04-edit-ordered' @('edit',$fixturePath,'--output-dir',$outputDirectory,'--rename','model','我的场景.sog','--include','cameras','true','--undo','1','--redo','1','--out',$editedPath)
    $model = $edited.plan.resources | Where-Object id -eq 'model'
    $cameras = $edited.plan.resources | Where-Object id -eq 'cameras'
    Assert-True ($edited.plan.outputDirectory -eq $outputDirectory -and $model.outputName -eq '我的场景.sog' -and $cameras.included) 'Ordered directory/name/include/undo/redo yields expected data'
    Assert-True ($edited.state.undoCount -eq 3 -and $edited.state.redoCount -eq 0 -and -not $edited.state.dirty) 'Save confirms snapshot without clearing session edit history'
    Assert-True ($edited.state.revision -eq ($opened.state.revision + 5)) 'Three edits and undo/redo publish five revisions'
    Assert-True ((Get-Hash $fixturePath) -eq $fixtureHash) 'Save As preserves input file bytes'
    $reopened = Invoke-Cli '05-edit-reopen' @('inspect',$editedPath)
    Assert-True (($reopened.plan | ConvertTo-Json -Depth 12 -Compress) -eq ($edited.plan | ConvertTo-Json -Depth 12 -Compress)) 'Saved and reopened plan values match exactly'
    Assert-True ($reopened.state.undoCount -eq 0 -and $reopened.state.redoCount -eq 0 -and -not $reopened.state.dirty) 'History is not serialized into plan'
    $fileText = [IO.File]::ReadAllText($editedPath,$utf8)
    Assert-True ($fileText -notmatch '"(undoCount|redoCount|sessionId|dirty)"') 'Persistent JSON contains no runtime history or dirty marker'

    $invalid = Invoke-Cli '06-invalid-after-valid' @('edit',$fixturePath,'--rename','model','合法.sog','--rename','model','CON.sog') 3
    Assert-True ($invalid.state.dirty -and (Get-Hash $fixturePath) -eq $fixtureHash) 'Later invalid edit does not publish earlier partial edits to file'
    $null = Invoke-Cli '07-invalid-path' @('edit',$fixturePath,'--output-dir','relative') 3
    $null = Invoke-Cli '08-unknown-resource' @('edit',$fixturePath,'--rename','missing','文件.sog') 3
    $null = Invoke-Cli '09-path-traversal' @('edit',$fixturePath,'--rename','model','..\越界.sog') 3
    Assert-True ((Get-Hash $fixturePath) -eq $fixtureHash) 'All invalid edits leave original bytes unchanged'

    $lock = [IO.File]::Open($fixturePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try {
        $locked = Invoke-Cli '10-save-locked' @('edit',$fixturePath,'--rename','model','不能发布.sog') 5
        Assert-True ($locked.state.dirty) 'Failed save preserves dirty session state'
        Assert-True ((Get-Hash $fixturePath) -eq $fixtureHash) 'Failed replace preserves locked original file'
    } finally { $lock.Dispose() }
    $null = Invoke-Cli '11-no-overwrite' @('new','--fixture','--out',$fixturePath) 5
    Assert-True ((Get-Hash $fixturePath) -eq $fixtureHash) 'New plan cannot overwrite an existing target implicitly'

    $futurePath = Join-Path $testDirectory '未来版本.3dgsdl.json'
    $futurePlan = [IO.File]::ReadAllText($fixturePath,$utf8) | ConvertFrom-Json
    $futurePlan.schemaVersion = 999
    [IO.File]::WriteAllText($futurePath,($futurePlan | ConvertTo-Json -Depth 12),$utf8)
    $futureHash = Get-Hash $futurePath
    $null = Invoke-Cli '12-future-schema' @('inspect',$futurePath) 4
    Assert-True ((Get-Hash $futurePath) -eq $futureHash) 'Future schema rejection does not change input file'
    $brokenPath = Join-Path $testDirectory '损坏.3dgsdl.json'
    [IO.File]::WriteAllText($brokenPath,'{"schemaVersion":1',$utf8)
    $null = Invoke-Cli '13-malformed-json' @('inspect',$brokenPath) 4
    $null = Invoke-Cli '14-usage' @('edit',$fixturePath,'--undo','-1') 2
    Assert-True ((Get-Hash $fixturePath) -eq $fixtureHash) 'Usage failure leaves original bytes unchanged'
    $replaced = Invoke-Cli '15-in-place' @('edit',$fixturePath,'--rename','model','原位保存.sog')
    $final = Invoke-Cli '16-in-place-reopen' @('inspect',$fixturePath)
    Assert-True (($final.plan.resources | Where-Object id -eq 'model').outputName -eq '原位保存.sog') 'Explicit in-place edit atomically saves and reopens'
    Assert-True (-not $replaced.state.dirty -and $final.state.undoCount -eq 0) 'Successful in-place save is clean and reopening clears history'
    $receipt.executableSha256After = Get-Hash $cliExecutable
    Assert-True ($receipt.executableSha256After -eq $receipt.executableSha256) 'All cases used the same executable content'
    $receipt.result = 'passed'
} catch {
    $receipt.result = 'failed'
    $receipt.error = $_.Exception.Message
    throw
} finally {
    $receipt.finishedAt = [DateTimeOffset]::Now.ToString('o')
    $receipt.steps = @($steps.ToArray())
    $receipt.assertions = @($assertions.ToArray())
    $receipt.fileHashes = @($fileHashes.ToArray())
    $receipt | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $testDirectory 'receipt.json') -Encoding utf8
    Write-Host "CLI integration receipt: $testDirectory"
}
