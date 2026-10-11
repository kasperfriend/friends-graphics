param(
    [string]$Fxc = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# CI runners and local Windows SDK installs disagree about the SDK version
# directory, so locate fxc.exe instead of pinning one path. An explicit
# -Fxc argument always wins.
function Resolve-Fxc([string]$explicit) {
    if ($explicit -and (Test-Path -LiteralPath $explicit)) { return $explicit }
    if ($explicit) { Write-Warning "fxc.exe not found at $explicit, searching the Windows SDK" }

    $candidates = @()
    foreach ($base in @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "$env:ProgramFiles\Windows Kits\10\bin")) {
        if (Test-Path -LiteralPath $base) {
            $candidates += Get-ChildItem -Path $base -Recurse -Filter 'fxc.exe' -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '\\(x86|x64)\\fxc\.exe$' }
        }
    }
    $found = $candidates | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $found) { throw 'fxc.exe not found. Install the Windows 10/11 SDK or pass -Fxc <path>.' }
    return $found.FullName
}

$Fxc = Resolve-Fxc $Fxc
Write-Output "Using fxc: $Fxc"

$jobs = @(
    @{ File = 'WaterEffect.h'; Names = @('source', 'vertexSource') },
    @{ File = 'src\Effects\LocalLightingRenderer.cpp'; Names = @('kSurfaceSource', 'kCompositeSource') },
    @{ File = 'WeatherVisuals.h'; Names = @('precipitationHLSL', 'lensDropletsHLSL', 'nativeParticleHLSL') },
    @{ File = 'src\Effects\DirectionalVolumetricLighting.cpp'; Names = @('kBoundarySource', 'kGroundHeightSource', 'kDepthSource', 'kLocalFogFieldSource', 'kIntegrateSource', 'kTemporalSource', 'kUpsampleSource', 'kCompositeSource', 'kBoundaryDebugSource') },
    @{ File = 'src\Effects\VolumetricClouds.cpp'; Names = @('kCloudMarchSource', 'kCloudCompositeSource') }
)

# ps_3_0 allows 512 instruction slots. The budgeted shaders are the ones
# with a raymarch loop, where an innocent-looking edit can unroll past the
# limit on a real driver long after it still compiles here.
$budgets = @{
    'kLocalFogFieldSource' = 512
    'kIntegrateSource'     = 512
    'kCloudMarchSource'    = 512
}

$temp = Join-Path $env:TEMP 'modern-wow-renderer-shaders'
New-Item -ItemType Directory -Force -Path $temp | Out-Null
foreach ($job in $jobs) {
    $source = Get-Content -Raw (Join-Path $root $job.File)
    foreach ($name in $job.Names) {
        $pattern = 'const char\* ' + [regex]::Escape($name) + '\s*=\s*R"HLSL\((?<body>[\s\S]*?)\)HLSL";'
        $match = [regex]::Match($source, $pattern)
        if (-not $match.Success) { throw "Shader source $name not found" }
        $inputFile = Join-Path $temp ($name + '.hlsl')
        $assembly = Join-Path $temp ($name + '.asm')
        Set-Content -Path $inputFile -Value $match.Groups['body'].Value -Encoding ascii
        $profile = if ($name -eq 'vertexSource') { 'vs_3_0' } elseif ($name -eq 'nativeParticleHLSL') { 'ps_2_0' } else { 'ps_3_0' }
        & $Fxc /nologo /T $profile /E main /O3 /Fc $assembly $inputFile
        if ($LASTEXITCODE -ne 0) { throw "Shader compilation failed: $name" }
        $slots = Select-String -Path $assembly -Pattern 'approximately ([0-9]+) instruction slots used' | Select-Object -First 1
        if ($slots) {
            $count = [int]$slots.Matches[0].Groups[1].Value
            Write-Output "$name $count slots"
            if ($budgets.ContainsKey($name) -and $count -gt $budgets[$name]) {
                throw "Shader exceeds its instruction budget: $name ($count > $($budgets[$name]))"
            }
        }
        else { Write-Output "$name compiled" }
    }
}

Write-Output 'All embedded shaders compiled.'
