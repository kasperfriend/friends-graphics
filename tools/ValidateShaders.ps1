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
    @{ File = 'src\Effects\VolumetricClouds.cpp'; Names = @('kCloudMarchSource', 'kCloudShadowSource', 'kCloudCompositeSource') },
    @{ File = 'src\Effects\AmbientOcclusion.cpp'; Names = @('kAmbientOcclusionSource', 'kAmbientOcclusionApplySource') }
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
    # 512 is the ps_3_0 floor every baseline card must support, so weather
    # still fits them all. It was 496; the rain phase-split (the fix for
    # long-session float32 banding) added its per-layer split+wrap slots.
    'precipitationHLSL'      = 512
    # Water pixel stage, capped on explicit request: 2048 is ~2.3x the slots
    # it actually uses (642 measured 2026-10-14, 713 after the 2026-10-17 CSE
    # pass, and 890 after round 5's quality ladder - the mid octave, the
    # bed-slope shore, the swash/backwash and the foam lace - which the round
    # estimated at "+80" and measured at +177; see docs/water-rendering.md).
    # A quality level does not shrink this number: the gates are [branch], so
    # every level compiles the same shader and a level buys time per pixel
    # rather than slots. The ceiling guards against a pathological edit (an
    # unrolled march, a forgotten constant array) rather than acting as a
    # design input on wave-train quality.
    'source'                 = 2048
    'volumePixelSource'     = 512
    'kLocalFogFieldSource'   = 512
    'kIntegrateSource'       = 512
    'kCloudMarchSource'      = 496
    'kCloudShadowSource'     = 512
    'kCloudCompositeSource'  = 512
    'kAmbientOcclusionSource' = 512
    'kAmbientOcclusionApplySource' = 512
}

# Measured, reported, not capped. This used to hold the water pixel stage: it sat in $budgets at
# 621 (597 slots measured plus 24 of compiler margin), and by the six-train round that ceiling had
# stopped being a guard and become a design input - the cheap way to fit 642 was to let the four
# short wave trains lose their slopes, which is precisely the visible difference between crests
# crossing at six angles and a comb of parallel ones. A ceiling that tight cannot weigh that trade.
# The cap has since been re-added on explicit request at 2048 (~3x the measured count, 642 before
# the 2026-10-17 CSE pass): wide enough
# to never steer wave-train quality, tight enough to catch a pathological edit. Shaders added to
# this list are still annotated with their count on every run, which is where the restraint comes
# from: a change that moves it by tens of slots is visible in the Checks tab of that pull request
# for a human to weigh.
#
# Nothing here claims to protect a device. The real limit is D3DCAPS9.MaxPixelShader30InstructionSlots
# (a ps_3_0 device must report at least 512 and may report up to 32768), fxc enforces neither the floor
# nor the ceiling, and a card that cannot fit the shader fails CreatePixelShader and keeps native
# water - WaterEffect.h logs the reported caps when that happens. Water at 2048 still leaves every
# baseline card (512 floor) its native-water fallback for the authored stage.
$measureOnly = @()

# Workflow annotations: the run log is not always reachable (log retention,
# restricted networks, forks), but annotations come back from the API, so the
# compiler diagnostics have to end up in one.
function Write-Annotation([string]$level, [string]$title, [string]$message) {
    $flat = ($message -replace "`r`n", "`n") -replace "`n", '%0A'
    # A workflow command's fields are comma-separated, so an unescaped comma simply ends the title:
    # "fxc failed (postProcessPixelSource, ps_3_0)" came back from the API as "fxc failed
    # (postProcessPixelSource", which is the one part of the diagnostic that says WHICH profile was
    # being compiled. Escape it the same way the message's newlines are escaped.
    $safeTitle = $title -replace ',', '%2C'
    Write-Output "::$level title=$safeTitle::$flat"
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
            if ($budgets.ContainsKey($name) -or $measureOnly -contains $name) {
                $failures += "$name : instruction count missing"
                Write-Annotation 'error' "Missing instruction count ($name)" 'Cannot weigh a shader cost that fxc did not report.'
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
