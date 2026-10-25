param(
    [string[]]$Suites=@('Ui','Mvp','Automation','Notes','PhysicsUi','Motion','Media','UxScope'),
    [ValidateSet('1','1.5')][string]$Scale='1',
    [string]$Character
)
$ErrorActionPreference='Stop'
$jesterRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
New-Item -ItemType Directory -Force -Path (Join-Path $jesterRoot 'artifacts') | Out-Null
$jesterReport=Join-Path $jesterRoot 'artifacts/gui-regression.json'
$jesterResults=@()
if(Test-Path -LiteralPath $jesterReport) {
    $jesterPreviousResults=Get-Content -Raw -LiteralPath $jesterReport | ConvertFrom-Json
    foreach($jesterPreviousResult in $jesterPreviousResults) {
        if($jesterPreviousResult.Suite) { $jesterResults+=$jesterPreviousResult }
    }
}
Push-Location -LiteralPath $jesterRoot
$jesterPreviousScale=$env:QT_SCALE_FACTOR
$jesterPreviousCharacter=$env:TRICKSTER_TEST_CHARACTER
try {
    $env:QT_SCALE_FACTOR=$Scale
    if($Character) { $env:TRICKSTER_TEST_CHARACTER=(Resolve-Path -LiteralPath $Character).Path }
    foreach($jesterSuite in $Suites) {
        $jesterReportSuite=if($Scale -eq '1') { $jesterSuite } else { "$jesterSuite@150" }
        $jesterLogStem=$jesterSuite.ToLower()+$(if($Scale -eq '1') { '' } else { '-150' })
        $jesterTarget="Jester${jesterSuite}Tests"
        $jesterExecutable=Join-Path $jesterRoot "build/bin/profile/$jesterTarget.exe"
        if(-not (Test-Path -LiteralPath $jesterExecutable)) { throw "Build $jesterTarget first." }
        $jesterTestLog=Join-Path $jesterRoot "artifacts/$jesterLogStem-tests.txt"
        $jesterEngineLog=Join-Path $jesterRoot "artifacts/$jesterLogStem-tests-engine.log"
        $ErrorActionPreference='Continue'
        try {
            $jesterTestArguments=@('-o',"$jesterTestLog,txt")
            if($jesterSuite -ne 'Media') { $jesterTestArguments=@("--project-path=$jesterRoot",'--rhi=dx12','--unattended')+$jesterTestArguments }
            & $jesterExecutable @jesterTestArguments *> $jesterEngineLog
            $jesterCode=$LASTEXITCODE
        } finally { $ErrorActionPreference='Stop' }
        $jesterTraceErrors=@(Select-String -LiteralPath $jesterEngineLog -Pattern 'Trace::Error|Trace::Assert').Count
        if($jesterCode -eq 0 -and $jesterTraceErrors) { $jesterCode=100 }
        $jesterResults=@($jesterResults | Where-Object Suite -ne $jesterReportSuite)
        $jesterResults+=[pscustomobject]@{Suite=$jesterReportSuite;ExitCode=$jesterCode;Log=$jesterTestLog;EngineLog=$jesterEngineLog}
        ConvertTo-Json -InputObject @($jesterResults) -Depth 4 | Set-Content -LiteralPath $jesterReport -Encoding UTF8
        Write-Output "$jesterTarget : $jesterCode"
    }
} finally { $env:QT_SCALE_FACTOR=$jesterPreviousScale;$env:TRICKSTER_TEST_CHARACTER=$jesterPreviousCharacter;Pop-Location }
if(@($jesterResults | Where-Object ExitCode -ne 0).Count) { exit 1 }
