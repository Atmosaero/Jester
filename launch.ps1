param(
    [ValidateSet('profile', 'release')][string]$Configuration = 'profile',
    [ValidateSet('dx12', 'vulkan')][string]$Rhi = 'dx12',
    [switch]$Demo,
    [string]$File,
    [switch]$Mcp,
    [ValidateRange(0,65535)][int]$Port=18530,
    [switch]$Poc,
    [switch]$Smoke
)
$ErrorActionPreference='Stop'
$jesterExecutable=Join-Path $PSScriptRoot "build/bin/$Configuration/Jester.exe"
if(-not (Test-Path -LiteralPath $jesterExecutable)) { throw 'Build Jester first: ./build.ps1 -Assets' }
$jesterRhi=$Rhi.ToLowerInvariant()
$jesterArguments=@("--project-path=$PSScriptRoot","--rhi=$jesterRhi")
if($Demo) { $jesterArguments+='--demo' }
if($File) { $jesterArguments+=(Resolve-Path -LiteralPath $File).Path }
if($Mcp) { $jesterArguments+=@('--mcp',"--mcp-port=$Port") }
if($Poc) { $jesterArguments+='--poc' }
if($Smoke) { $jesterArguments+='--smoke' }
Push-Location -LiteralPath $PSScriptRoot
try { & $jesterExecutable @jesterArguments; if($LASTEXITCODE -ne 0) { throw "Jester exited with code $LASTEXITCODE" } }
finally { Pop-Location }
