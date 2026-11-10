[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/windows-release',
    [string]$CTestPath = 'ctest'
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
}
if (-not (Test-Path -LiteralPath (Join-Path $buildRoot 'CTestTestfile.cmake') -PathType Leaf)) {
    throw 'Configured CTest build directory not found. Run tools/build.ps1 first.'
}

# Keep a minimum inventory so removing a test registration cannot produce a
# successful but incomplete required check. Newly registered tests also run.
$requiredTests = @(
    'tasks.queue_ownership_restore',
    'workspace.strict_atomic_async',
    'plan.model_history_save_contracts',
    'text.editor_and_cjk',
    'transfer.source_http_files_cancel',
    'cli.plan_roundtrip',
    'ui.task_visual_matrix',
    'ui.component_state_matrix',
    'ui.page_visual_matrix',
    'storage.strict_atomic_async'
)
$excludedTest = 'graphics.skia_vulkan_worker'
$discoveryOutput = & $CTestPath --test-dir $buildRoot --show-only=json-v1
if ($LASTEXITCODE -ne 0) { throw 'CTest discovery failed' }
$discovery = ($discoveryOutput -join "`n") | ConvertFrom-Json
$registeredNames = @($discovery.tests | ForEach-Object { [string]$_.name })
if ($registeredNames.Count -eq 0 -or @($registeredNames | Sort-Object -Unique).Count -ne $registeredNames.Count) {
    throw 'CTest discovery is empty or contains duplicate test names'
}
foreach ($testName in @($requiredTests) + $excludedTest) {
    if ($registeredNames -cnotcontains $testName) { throw "Required test is not registered: $testName" }
}
$expectedNames = @($registeredNames | Where-Object { $_ -cne $excludedTest })

# A unique output path prevents a stale report from satisfying the check.
$runId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N')
$reportDirectory = Join-Path $projectRoot "artifacts/hosted-tests/$runId"
[IO.Directory]::CreateDirectory($reportDirectory) | Out-Null
$testReport = Join-Path $reportDirectory 'tests.xml'
$discoveryOutput | Set-Content -LiteralPath (Join-Path $reportDirectory 'discovery.json') -Encoding utf8
& $CTestPath --test-dir $buildRoot --output-on-failure --no-tests=error -E '^graphics[.]skia_vulkan_worker$' --output-junit $testReport
if ($LASTEXITCODE -ne 0) { throw 'Hosted tests failed' }
if (-not (Test-Path -LiteralPath $testReport -PathType Leaf)) { throw 'CTest did not produce a JUnit report' }

[xml]$results = Get-Content -LiteralPath $testReport -Raw
$suite = $results.DocumentElement
if ($suite.get_Name() -cne 'testsuite') { throw 'Unexpected CTest JUnit report structure' }
foreach ($attribute in @('tests', 'failures', 'disabled', 'skipped')) {
    if (-not $suite.HasAttribute($attribute) -or $suite.GetAttribute($attribute) -notmatch '^\d+$') {
        throw "Missing or invalid JUnit count: $attribute"
    }
}
if ([int]$suite.GetAttribute('tests') -ne $expectedNames.Count -or
    [int]$suite.GetAttribute('failures') -ne 0 -or
    [int]$suite.GetAttribute('disabled') -ne 0 -or
    [int]$suite.GetAttribute('skipped') -ne 0) {
    throw 'Incomplete or failed hosted test report'
}
if ($suite.HasAttribute('errors') -and $suite.GetAttribute('errors') -notmatch '^0+$') {
    throw 'JUnit report contains errors'
}
$cases = @($suite.SelectNodes('testcase'))
$actualNames = @($cases | ForEach-Object { $_.GetAttribute('name') })
if ($cases.Count -ne $expectedNames.Count -or
    @($actualNames | Sort-Object -Unique).Count -ne $actualNames.Count) {
    throw 'JUnit report contains missing or duplicate test cases'
}
foreach ($testName in $expectedNames) {
    if ($actualNames -cnotcontains $testName) { throw "Test result is missing: $testName" }
}
foreach ($case in $cases) {
    if ($case.GetAttribute('status') -cne 'run' -or $case.SelectNodes('failure|error|skipped').Count -ne 0) {
        throw "Test did not pass: $($case.GetAttribute('name'))"
    }
}
$summary = "Hosted tests: $($cases.Count) passed; only $excludedTest excluded. Vulkan presentation and real UI/IME checks require a supported Windows device."
Write-Host $summary
Write-Host "JUnit report: $testReport"
if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_STEP_SUMMARY)) {
    $summary | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Encoding utf8
}
