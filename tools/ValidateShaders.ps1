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

try { $Fxc = Resolve-Fxc $Fxc }
catch {
    Write-Output "::error title=fxc not found::$($_.Exception.Message)"
    throw
}
Write-Output "Using fxc: $Fxc"

$jobs = @(
    @{ File = 'VolumeEffects.h'; Names = @('volumePixelSource', 'volumeBlurPixelSource', 'volumeCopyPixelSource', 'volumeTemporalPixelSource', 'contactShadowPixelSource', 'rayCompositePixelSource', 'solarRadialPixelSource', 'localLightPixelSource', 'celestialMarkerPixelSource', 'postProcessPixelSource') },
    @{ File = 'WaterEffect.h'; Names = @('source', 'vertexSource') },
    @{ File = 'src\Effects\LocalLightingRenderer.cpp'; Names = @('kSurfaceSource', 'kCompositeSource') },
    @{ File = 'WeatherVisuals.h'; Names = @('precipitationHLSL', 'lensDropletsHLSL', 'nativeParticleHLSL') },
    @{ File = 'src\Effects\DirectionalVolumetricLighting.cpp'; Names = @('kBoundarySource', 'kGroundHeightSource', 'kDepthSource', 'kLocalFogFieldSource', 'kIntegrateSource', 'kTemporalSource', 'kUpsampleSource', 'kCompositeSource', 'kBoundaryDebugSource') },
    @{ File = 'src\Effects\VolumetricClouds.cpp'; Names = @('kCloudMarchSource', 'kCloudShadowSource', 'kCloudCompositeSource') }
)

# A ps_3_0 device has to support at least 512 instruction slots and may report more, up to
# 32768, through D3DCAPS9.MaxPixelShader30InstructionSlots; fxc enforces neither number, so
# compiling here says nothing about creating on a player's card. The budgeted shaders are the ones
# with a raymarch loop, where an innocent-looking edit can unroll past the
# limit on a real driver long after it still compiles here.
# The cloud march is held below the ceiling rather than at it: these
# shaders are compiled at runtime by whatever d3dcompiler the player's
# Windows ships, which does not have to agree with this runner's fxc about
# instruction selection. A change that needs the last sixteen slots should
# have to say so here, in a diff, rather than discover it on a player's
# machine.
$budgets = @{
    'precipitationHLSL'      = 496 # Weather must fit baseline PS3 hardware too
    # Water: the volume rewrite measures 542 slots (fxc /O3 on this runner, 2026-10-14).
    # That is above the 512 instruction slots a ps_3_0 *device* is required to support,
    # which is what the old 496 budget existed to stay under (496 kept 16 slots of margin
    # for another SDK's instruction selection). It is not a bytecode-format limit: a ps_3_0
    # shader may run as long as D3DCAPS9.MaxPixelShader30InstructionSlots allows, up to
    # 32768, and fxc assembles 542 without a word of complaint. Nothing in the client caps
    # it either - this shader is created on the game's own device by us - so the ceiling is
    # per-device and stated by the driver.
    # The cuts were priced one shader per candidate before this number moved (table in
    # docs/water-rendering.md): SSR march 69, sun-road march 51, analytic swell 37, scatter
    # lobe 17, sky gradient 15, wave-lit body 13, shoal derivative 11 - and the SSR step
    # count 0, because those loops are [loop], so steps buy frame time and no slots. Every
    # route to 496 cost a visible term of the new shading, so the ceiling moved instead of
    # the look. The consequence is a fallback, not a break: on a device reporting 512 slots,
    # CreatePixelShader fails, WaterEffect logs "water shader creation failed hr=... PS30
    # slots=512; keeping native water" having written no device state at all, and that
    # machine keeps the client's own water until the device is reset.
    # The margin absorbs a different SDK's instruction selection; growth beyond it should
    # have to say so here, in a diff, exactly as before.
    'source'                = 560 # Water: 542 used, 18 spare
    'volumePixelSource'     = 512
    'kLocalFogFieldSource'   = 512
    'kIntegrateSource'       = 512
    'kCloudMarchSource'      = 496
    'kCloudShadowSource'     = 512
    'kCloudCompositeSource'  = 512
}

# Workflow annotations: the run log is not always reachable (log retention,
# restricted networks, forks), but annotations come back from the API, so the
# compiler diagnostics have to end up in one.
function Write-Annotation([string]$level, [string]$title, [string]$message) {
    $flat = ($message -replace "`r`n", "`n") -replace "`n", '%0A'
    Write-Output "::$level title=$title::$flat"
}

$temp = Join-Path $env:TEMP 'modern-wow-renderer-shaders'
New-Item -ItemType Directory -Force -Path $temp | Out-Null

# Every shader is attempted even after one fails, so a single run reports the
# full picture instead of only the first problem.
$failures = @()
$report = @()
foreach ($job in $jobs) {
    $source = Get-Content -Raw (Join-Path $root $job.File)
    foreach ($name in $job.Names) {
        $pattern = 'const char\* ' + [regex]::Escape($name) + '\s*=\s*R"HLSL\((?<body>[\s\S]*?)\)HLSL";'
        $match = [regex]::Match($source, $pattern)
        if (-not $match.Success) {
            $failures += "$name : source not found in $($job.File)"
            Write-Annotation 'error' "Shader source not found ($name)" "No raw HLSL string literal named $name in $($job.File)."
            continue
        }
        $inputFile = Join-Path $temp ($name + '.hlsl')
        $assembly = Join-Path $temp ($name + '.asm')
        Set-Content -Path $inputFile -Value $match.Groups['body'].Value -Encoding ascii
        $profile = if ($name -eq 'vertexSource') { 'vs_3_0' } elseif ($name -eq 'nativeParticleHLSL') { 'ps_2_0' } else { 'ps_3_0' }

        $output = & $Fxc /nologo /T $profile /E main /O3 /Fc $assembly $inputFile 2>&1
        $exit = $LASTEXITCODE
        $text = ($output | Out-String).Trim()
        if ($text) { Write-Output $text }
        if ($exit -ne 0) {
            $failures += "$name : fxc exit $exit"
            Write-Annotation 'error' "fxc failed ($name, $profile)" $text
            continue
        }

        $slots = Select-String -Path $assembly -Pattern 'approximately ([0-9]+) instruction slots used' | Select-Object -First 1
        if ($slots) {
            $count = [int]$slots.Matches[0].Groups[1].Value
            $report += "$name = $count slots"
            Write-Output "$name $count slots"
            if ($budgets.ContainsKey($name) -and $count -gt $budgets[$name]) {
                $failures += "$name : $count slots > budget $($budgets[$name])"
                Write-Annotation 'error' "Shader over budget ($name)" "$count instruction slots used, budget is $($budgets[$name])."
            }
        }
        else {
            if ($budgets.ContainsKey($name)) {
                $failures += "$name : instruction count missing"
                Write-Annotation 'error' "Missing instruction count ($name)" 'Cannot enforce the shader budget without an assembly instruction count.'
            }
            $report += "$name = compiled"
            Write-Output "$name compiled"
        }
    }
}

Write-Annotation 'notice' 'Shader instruction counts' ($report -join "`n")
if ($failures.Count -gt 0) {
    Write-Annotation 'error' 'Shader validation failed' ($failures -join "`n")
    throw "Shader validation failed:`n" + ($failures -join "`n")
}

Write-Output 'All embedded shaders compiled.'
