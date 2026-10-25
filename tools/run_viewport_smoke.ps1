param([ValidateSet('1','1.5')][string[]]$Scales=@('1','1.5'))
$ErrorActionPreference='Stop'
$jesterRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
New-Item -ItemType Directory -Force -Path (Join-Path $jesterRoot 'artifacts') | Out-Null
$jesterPreviousScale=$env:QT_SCALE_FACTOR
$jesterResults=@()
Push-Location -LiteralPath $jesterRoot
try {
    foreach($jesterScale in $Scales) {
        $env:QT_SCALE_FACTOR=$jesterScale
        $jesterLabel=if($jesterScale -eq '1') { '100' } else { '150' }
        $jesterLog="artifacts/poc-final-$jesterLabel-engine.log"
        $ErrorActionPreference='Continue'
        try {
            & './build/bin/profile/Jester.exe' "--project-path=$jesterRoot" --rhi=dx12 --smoke *> $jesterLog
            $jesterCode=$LASTEXITCODE
        } finally { $ErrorActionPreference='Stop' }
        $jesterReport=Get-Content -Raw -Encoding UTF8 -LiteralPath artifacts/poc-smoke.json | ConvertFrom-Json
        $jesterTraceCount=@(Select-String -LiteralPath $jesterLog -Pattern 'Trace::Error|Trace::Assert').Count
        $jesterPassed=$jesterCode -eq 0 -and $jesterReport.passed -and $jesterTraceCount -eq 0
        Copy-Item -LiteralPath artifacts/poc-smoke.json -Destination "artifacts/poc-final-$jesterLabel.json"
        Copy-Item -LiteralPath artifacts/poc-viewport.png -Destination "artifacts/poc-final-$jesterLabel.png"
        $jesterResults+=[pscustomobject]@{Scale=$jesterScale;Dpi=$jesterReport.after.dpi;ExitCode=$jesterCode;Passed=$jesterPassed;TraceErrors=$jesterTraceCount}
        ConvertTo-Json -InputObject @($jesterResults) -Depth 4 | Set-Content -LiteralPath artifacts/viewport-regression.json -Encoding UTF8
        Write-Output "Atom viewport $jesterLabel% : $jesterPassed"
    }
} finally { $env:QT_SCALE_FACTOR=$jesterPreviousScale;Pop-Location }
if(@($jesterResults | Where-Object Passed -ne $true).Count) { exit 1 }
