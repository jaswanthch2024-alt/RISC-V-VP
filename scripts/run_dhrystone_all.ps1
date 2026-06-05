# Run Dhrystone tests for RV32 and RV64 with all timing models
# Usage: .\scripts\run_dhrystone_all.ps1 [-MaxInstr 20000000]
# Note: Executables are Linux ELF binaries, requires WSL to run

param(
    [int]$MaxInstr = 20000000
)

$ErrorActionPreference = "Stop"

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  RISC-V TLM Dhrystone Test Runner" -ForegroundColor Cyan
Write-Host "  Max Instructions: $MaxInstr" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

# Check if WSL is available
$wslAvailable = Get-Command wsl -ErrorAction SilentlyContinue
if (-not $wslAvailable) {
    Write-Host "[ERROR] WSL not found. Executables are Linux ELF binaries and require WSL to run." -ForegroundColor Red
    exit 1
}

# Define test configurations
$builds = @(
    @{ Name = "CYCLE6"; Dir = "build_cycle6"; Desc = "6-Stage Pipeline"; Pipeline = "6-stage" },
    @{ Name = "CYCLE2"; Dir = "build_cycle2"; Desc = "2-Stage Pipeline"; Pipeline = "2-stage" },
    @{ Name = "AT"; Dir = "build_at"; Desc = "Approximately-Timed"; Pipeline = "2-stage" },
    @{ Name = "LT"; Dir = "build_lt"; Desc = "Loosely-Timed"; Pipeline = "2-stage" }
)

$tests = @(
    @{ Arch = "RV32"; Hex = "tests/hex/dhrystone32.hex"; Bits = "32-bit" },
    @{ Arch = "RV64"; Hex = "tests/hex/dhrystone64.hex"; Bits = "64-bit" }
)

$results = @()

function Run-DhrystoneTest {
    param($BuildName, $BuildDir, $Pipeline, $Arch, $Bits, $HexFile, $MaxInstr)
    
    $exePath = "$BuildDir/RISCV_VP"
    
    if (-not (Test-Path $exePath)) {
        Write-Host "  [SKIP] RISCV_VP not found in $BuildDir" -ForegroundColor Yellow
        return $null
    }

    if (-not (Test-Path $HexFile)) {
        Write-Host "  [SKIP] $HexFile not found" -ForegroundColor Yellow
        return $null
    }

    Write-Host "  Running $Arch with $BuildName..." -ForegroundColor White
    
    # Convert Windows path to WSL path
    $wslExe = $exePath -replace '\\', '/'
    $wslHex = $HexFile -replace '\\', '/'
    
    $startTime = Get-Date
    
    # Run via WSL
    $archFlag = if ($Arch -eq "RV32") { "32" } else { "64" }
    $wslCmd = "./$wslExe -f $wslHex -R $archFlag --max-instr $MaxInstr 2>&1"
    $output = wsl bash -c $wslCmd | Out-String
    
    $endTime = Get-Date
    $wallTime = ($endTime - $startTime).TotalSeconds
    
    # Parse results
    $cycles = 0
    $instructions = 0
    $cpi = 0.0
    $ipc = 0.0
    $stalls = 0
    $branches = 0
    $simTime = 0
    $mips = 0.0
    
    if ($output -match "Cycles:\s+(\d+)") { $cycles = [long]$matches[1] }
    if ($output -match "Instructions:\s+(\d+)") { $instructions = [long]$matches[1] }
    if ($output -match "CPI:\s+([\d.]+)") { $cpi = [double]$matches[1] }
    if ($output -match "IPC:\s+([\d.]+)") { $ipc = [double]$matches[1] }
    if ($output -match "Stalls:\s+(\d+)") { $stalls = [long]$matches[1] }
    if ($output -match "Branches:\s+(\d+)") { $branches = [long]$matches[1] }
    if ($output -match "Sim time:\s+(\d+)\s*ms") { $simTime = [long]$matches[1] }
    if ($output -match "Wall time:\s+([\d.]+)\s*s") { $wallTime = [double]$matches[1] }
    if ($output -match "IPS:\s+([\d.]+)\s*MIPS") { $mips = [double]$matches[1] }
    
    # If MIPS not found in output, calculate it
    if ($mips -eq 0 -and $wallTime -gt 0) { 
        $mips = [math]::Round($instructions / $wallTime / 1000000, 2) 
    }
    
    # Format cycles for display (e.g., 20.10M)
    $cyclesFormatted = if ($cycles -ge 1000000) { 
        "{0:N2}M" -f ($cycles / 1000000) 
    } else { 
        $cycles.ToString() 
    }
    
    return [PSCustomObject]@{
        Model = "$Arch`n$BuildName"
        Arch = $Bits
        Pipeline = $Pipeline
        Cycles = $cyclesFormatted
        CPI = $cpi
        IPC = $ipc
        MIPS = $mips
        Stalls = $stalls
        RawCycles = $cycles
        RawInstructions = $instructions
        WallTimeSec = [math]::Round($wallTime, 3)
    }
}

# Run all tests
foreach ($build in $builds) {
    Write-Host "`n--- $($build.Desc) ($($build.Name)) ---" -ForegroundColor Cyan
    
    foreach ($test in $tests) {
        $result = Run-DhrystoneTest -BuildName $build.Name -BuildDir $build.Dir `
            -Pipeline $build.Pipeline -Arch $test.Arch -Bits $test.Bits `
            -HexFile $test.Hex -MaxInstr $MaxInstr
        
        if ($result) {
            $results += $result
        }
    }
}

# Display results table
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  A. Dhrystone Benchmark (20M Instructions)" -ForegroundColor Cyan
Write-Host "  Linear integer workload. Ideal for measuring peak MIPS." -ForegroundColor Gray
Write-Host "========================================`n" -ForegroundColor Cyan

if ($results.Count -gt 0) {
    # Display formatted table
    Write-Host ("{0,-12} {1,-8} {2,-10} {3,-10} {4,-8} {5,-8} {6,-8} {7,-10}" -f "Model", "Arch", "Pipeline", "Cycles", "CPI", "IPC", "MIPS", "Stalls") -ForegroundColor White
    Write-Host ("{0,-12} {1,-8} {2,-10} {3,-10} {4,-8} {5,-8} {6,-8} {7,-10}" -f "-----", "----", "--------", "------", "---", "---", "----", "------") -ForegroundColor White
    
    foreach ($r in $results) {
        $modelParts = $r.Model -split "`n"
        $modelDisplay = "$($modelParts[0]) $($modelParts[1])"
        Write-Host ("{0,-12} {1,-8} {2,-10} {3,-10} {4,-8} {5,-8} {6,-8} {7,-10}" -f $modelDisplay, $r.Arch, $r.Pipeline, $r.Cycles, $r.CPI, $r.IPC, $r.MIPS, $r.Stalls)
    }
    
    # Save to file
    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $outputFile = "dhrystone_eval_$timestamp.txt"
    
    "RISC-V TLM Dhrystone Evaluation Results" | Out-File $outputFile
    "========================================" | Out-File $outputFile -Append
    "A. Dhrystone Benchmark (20M Instructions)" | Out-File $outputFile -Append
    "Linear integer workload. Ideal for measuring peak MIPS." | Out-File $outputFile -Append
    "Generated: $(Get-Date)" | Out-File $outputFile -Append
    "`n" | Out-File $outputFile -Append
    
    # Markdown table
    "| Model | Arch | Pipeline | Cycles | CPI | IPC | MIPS | Stalls |" | Out-File $outputFile -Append
    "|-------|------|----------|--------|-----|-----|------|--------|" | Out-File $outputFile -Append
    foreach ($r in $results) {
        $modelParts = $r.Model -split "`n"
        $modelDisplay = "$($modelParts[0]) $($modelParts[1])"
        "| $modelDisplay | $($r.Arch) | $($r.Pipeline) | $($r.Cycles) | $($r.CPI) | $($r.IPC) | $($r.MIPS) | $($r.Stalls) |" | Out-File $outputFile -Append
    }
    
    Write-Host "`nResults saved to: $outputFile" -ForegroundColor Green
} else {
    Write-Host "[ERROR] No test results collected. Check that builds exist and WSL is configured." -ForegroundColor Red
}

Write-Host "`nDone." -ForegroundColor Green
