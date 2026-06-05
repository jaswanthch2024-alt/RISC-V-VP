# Build and run robust_fast_unified.c for both RV32 and RV64
# Requires: RISC-V toolchain in PATH or WSL

param(
    [switch]$BuildOnly,
    [switch]$RunOnly,
    [string]$TimingModel = "CYCLE6"
)

$ErrorActionPreference = "Stop"

$SrcFile = "tests\full_system\robust_fast_unified.c"
$HexDir = "tests\hex"

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  Robust Fast Test Builder & Runner" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

# Ensure hex directory exists
if (-not (Test-Path $HexDir)) {
    New-Item -ItemType Directory -Path $HexDir -Force | Out-Null
}

# Build phase
if (-not $RunOnly) {
    Write-Host "--- Building RV32 version ---" -ForegroundColor Yellow
    
    $rv32gcc = Get-Command riscv32-unknown-elf-gcc -ErrorAction SilentlyContinue
    if ($rv32gcc) {
        & riscv32-unknown-elf-gcc -march=rv32imac -mabi=ilp32 -O2 -nostdlib `
            -D__RV32__ "-Wl,--entry=main" "-Wl,--gc-sections" `
            $SrcFile -o robust_fast32.elf
        
        & riscv32-unknown-elf-objcopy -O ihex robust_fast32.elf "$HexDir\robust_fast32.hex"
        Write-Host "[OK] Created: $HexDir\robust_fast32.hex" -ForegroundColor Green
    } else {
        # Try WSL
        Write-Host "  Using WSL for RV32 build..." -ForegroundColor Gray
        $wslSrc = $SrcFile -replace '\\', '/'
        $wslHex = "$HexDir/robust_fast32.hex" -replace '\\', '/'
        
        wsl bash -c "riscv32-unknown-elf-gcc -march=rv32imac -mabi=ilp32 -O2 -nostdlib -D__RV32__ -Wl,--entry=main -Wl,--gc-sections $wslSrc -o robust_fast32.elf && riscv32-unknown-elf-objcopy -O ihex robust_fast32.elf $wslHex"
        
        if ($LASTEXITCODE -eq 0) {
            Write-Host "[OK] Created: $HexDir\robust_fast32.hex" -ForegroundColor Green
        } else {
            Write-Host "[SKIP] RV32 build failed or toolchain not found" -ForegroundColor Yellow
        }
    }

    Write-Host "`n--- Building RV64 version ---" -ForegroundColor Yellow
    
    $rv64gcc = Get-Command riscv64-unknown-elf-gcc -ErrorAction SilentlyContinue
    if ($rv64gcc) {
        & riscv64-unknown-elf-gcc -march=rv64imac -mabi=lp64 -O2 -nostdlib `
            -D__RV64__ "-Wl,--entry=main" "-Wl,--gc-sections" `
            $SrcFile -o robust_fast64.elf
        
        & riscv64-unknown-elf-objcopy -O ihex robust_fast64.elf "$HexDir\robust_fast64.hex"
        Write-Host "[OK] Created: $HexDir\robust_fast64.hex" -ForegroundColor Green
    } else {
        # Try WSL
        Write-Host "  Using WSL for RV64 build..." -ForegroundColor Gray
        $wslSrc = $SrcFile -replace '\\', '/'
        $wslHex = "$HexDir/robust_fast64.hex" -replace '\\', '/'
        
        wsl bash -c "riscv64-unknown-elf-gcc -march=rv64imac -mabi=lp64 -O2 -nostdlib -D__RV64__ -Wl,--entry=main -Wl,--gc-sections $wslSrc -o robust_fast64.elf && riscv64-unknown-elf-objcopy -O ihex robust_fast64.elf $wslHex"
        
        if ($LASTEXITCODE -eq 0) {
            Write-Host "[OK] Created: $HexDir\robust_fast64.hex" -ForegroundColor Green
        } else {
            Write-Host "[SKIP] RV64 build failed or toolchain not found" -ForegroundColor Yellow
        }
    }

    if ($BuildOnly) {
        Write-Host "`nBuild complete." -ForegroundColor Green
        exit 0
    }
}

# Run phase
Write-Host "`n--- Running Tests ---" -ForegroundColor Yellow

# Determine build directory based on timing model
$buildDir = switch ($TimingModel.ToUpper()) {
    "CYCLE6" { "build_cycle6" }
    "CYCLE2" { "build_cycle2" }
    "AT"     { "build_at" }
    "LT"     { "build_lt" }
    default  { "build_cycle6" }
}

$exe = "$buildDir/RISCV_VP"

if (-not (Test-Path $exe)) {
    Write-Host "[ERROR] Simulator not found: $exe" -ForegroundColor Red
    Write-Host "Build with: cmake --build $buildDir --target RISCV_VP" -ForegroundColor Yellow
    exit 1
}

$results = @()

function Run-RobustTest {
    param($Arch, $HexFile)
    
    if (-not (Test-Path $HexFile)) {
        Write-Host "  [SKIP] $HexFile not found" -ForegroundColor Yellow
        return $null
    }
    
    Write-Host "`n  Running $Arch test..." -ForegroundColor Cyan
    
    $archFlag = if ($Arch -eq "RV32") { "32" } else { "64" }
    $wslExe = $exe -replace '\\', '/'
    $wslHex = $HexFile -replace '\\', '/'
    
    $startTime = Get-Date
    $output = wsl bash -c "./$wslExe -f $wslHex -R $archFlag 2>&1" | Out-String
    $endTime = Get-Date
    $wallTime = ($endTime - $startTime).TotalSeconds
    
    Write-Host $output
    
    # Parse results
    $pass = $output -match "\[ROBUST\] PASS"
    $mcycle = 0
    $minstret = 0
    $irqCount = 0
    $dmaMatch = 0
    
    if ($output -match "mcycle=(\d+)") { $mcycle = [long]$matches[1] }
    if ($output -match "minstret=(\d+)") { $minstret = [long]$matches[1] }
    if ($output -match "irq_count=(\d+)") { $irqCount = [long]$matches[1] }
    if ($output -match "dma_mismatch=(\d+)") { $dmaMatch = [long]$matches[1] }
    
    return [PSCustomObject]@{
        Arch = $Arch
        Status = if ($pass) { "PASS" } else { "FAIL" }
        Cycles = $mcycle
        Instructions = $minstret
        IRQs = $irqCount
        DMAErrors = $dmaMatch
        WallTime = [math]::Round($wallTime, 2)
    }
}

# Run RV32 test
$result32 = Run-RobustTest -Arch "RV32" -HexFile "$HexDir\robust_fast32.hex"
if ($result32) { $results += $result32 }

# Run RV64 test
$result64 = Run-RobustTest -Arch "RV64" -HexFile "$HexDir\robust_fast64.hex"
if ($result64) { $results += $result64 }

# Display summary
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  ROBUST TEST RESULTS ($TimingModel)" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

if ($results.Count -gt 0) {
    $results | Format-Table -Property Arch, Status, Cycles, Instructions, IRQs, DMAErrors, WallTime -AutoSize
    
    $allPass = ($results | Where-Object { $_.Status -eq "PASS" }).Count -eq $results.Count
    if ($allPass) {
        Write-Host "[SUCCESS] All tests passed!" -ForegroundColor Green
    } else {
        Write-Host "[WARNING] Some tests failed!" -ForegroundColor Red
    }
} else {
    Write-Host "[ERROR] No tests were run." -ForegroundColor Red
}

Write-Host "`nDone." -ForegroundColor Green
