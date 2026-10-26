param([string[]]$Suites=@('Core','Physics','MvpCore','MuJoCo','Media','Reliability','Settings'))
$ErrorActionPreference='Stop'
$jesterRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
New-Item -ItemType Directory -Force -Path (Join-Path $jesterRoot 'artifacts') | Out-Null
$jesterResults=@()
$jesterPreviousTimeout=$env:QTEST_FUNCTION_TIMEOUT
Push-Location -LiteralPath $jesterRoot
try {
    # Same limit as the baseline reliability suite; imported whole-body
    # experiments deliberately run longer than QtTest's default five minutes.
    $env:QTEST_FUNCTION_TIMEOUT='1200000'
    foreach($jesterSuite in $Suites) {
        $jesterTarget="Jester${jesterSuite}Tests"
        $jesterExecutable=Join-Path $jesterRoot "build/bin/profile/$jesterTarget.exe"
        if(-not (Test-Path -LiteralPath $jesterExecutable)) { throw "Build $jesterTarget first." }
        $jesterLog=Join-Path $jesterRoot "artifacts/$($jesterSuite.ToLower())-tests.txt"
        $ErrorActionPreference='Continue'
        try { & $jesterExecutable -o "$jesterLog,txt" *> "artifacts/$($jesterSuite.ToLower())-tests-engine.log"; $jesterCode=$LASTEXITCODE }
        finally { $ErrorActionPreference='Stop' }
        $jesterResults+=[pscustomobject]@{Suite=$jesterSuite;ExitCode=$jesterCode;Log=$jesterLog}
        ConvertTo-Json -InputObject @($jesterResults) -Depth 4 | Set-Content -LiteralPath artifacts/core-regression.json -Encoding UTF8
        Write-Output "$jesterTarget : $jesterCode"
    }
} finally { $env:QTEST_FUNCTION_TIMEOUT=$jesterPreviousTimeout;Pop-Location }
if(@($jesterResults | Where-Object ExitCode -ne 0).Count) { exit 1 }
