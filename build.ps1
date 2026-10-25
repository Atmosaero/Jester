param(
    [string]$EngineRoot = 'D:/O3DE',
    [ValidateSet('profile', 'release')][string]$Configuration = 'profile',
    [switch]$Assets,
    [switch]$Tests,
    [string]$PackagesRoot,
    [ValidateRange(1,8)][int]$Jobs=1
)
$ErrorActionPreference = 'Stop'
$jesterRoot = $PSScriptRoot
$jesterCmake = Join-Path $EngineRoot 'cmake/runtime/bin/cmake.exe'
$jesterPackages = $PackagesRoot
if (-not $jesterPackages) {
    $jesterCache = Join-Path $jesterRoot 'build/CMakeCache.txt'
    if (Test-Path -LiteralPath $jesterCache) {
        $jesterPackageLine = Select-String -LiteralPath $jesterCache -Pattern '^LY_3RDPARTY_PATH:[^=]+=' | Select-Object -First 1
        if ($jesterPackageLine) { $jesterPackages = ($jesterPackageLine.Line -split '=', 2)[1] }
    }
    if (-not $jesterPackages) { $jesterPackages = Join-Path $jesterRoot 'build/packages' }
}
& $jesterCmake -S $jesterRoot -B (Join-Path $jesterRoot 'build') -G 'Visual Studio 18 2026' -A x64 "-DJESTER_O3DE_ROOT=$EngineRoot" "-DLY_3RDPARTY_PATH=$jesterPackages" -DLY_UNITY_BUILD=OFF -DBUILD_TESTING=OFF
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)." }
if ($Assets) {
    & (Join-Path $EngineRoot 'bin/Windows/profile/Default/AssetProcessorBatch.exe') "--project-path=$jesterRoot" --platforms=pc --unattended
    if ($LASTEXITCODE -ne 0) { throw "Asset processing failed ($LASTEXITCODE)." }
}
$jesterTargets=@('JesterEditor')
if($Tests) { $jesterTargets+='JesterRegression' }
& $jesterCmake --build (Join-Path $jesterRoot 'build') --target @jesterTargets --config $Configuration --parallel $Jobs -- /p:CL_MPCount=1
if ($LASTEXITCODE -ne 0) { throw "Native build failed ($LASTEXITCODE)." }
