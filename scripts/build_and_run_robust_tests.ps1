# Build and run robust system tests for all configurations
# Requires: RISC-V toolchain (riscv32/64-unknown-elf-gcc) in PATH

param(
    [switch]$BuildOnly,
    [switch]$RunOnly,
    [int]$MaxInstr = 0  # 0 = run until completion
)

$ErrorActionPreference = "Stop"

Write-Host "`n=== RISC-V TLM Robust System Test Runner ===" -ForegroundColor Cyan

# Check for toolchain
$rv32gcc = Get-Command riscv32-unknown-elf-gcc -ErrorAction SilentlyContinue
$rv64gcc = Get-Command riscv64-unknown-elf-gcc -ErrorAction SilentlyContinue

if (-not $rv32gcc -and -not $RunOnly) {
    Write-Host "`n[ERROR] riscv32-unknown-elf-gcc not found in PATH" -ForegroundColor Red
    Write-Host "Install from: https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/" -ForegroundColor Yellow
    exit 1
}

# Build HEX files
if (-not $RunOnly) {
    Write-Host "`n--- Building RV32 hex ---" -ForegroundColor Yellow
    & riscv32-unknown-elf-gcc -march=rv32imac -mabi=ilp32 -O2 -nostdlib `
        -Wl,--entry=main -Wl,--gc-sections `
        tests/full_system/robust_system_test.c -o robust.elf
    if ($LASTEXITCODE -ne 0) { exit 1 }
    
    & riscv32-unknown-elf-objcopy -O ihex robust.elf tests/hex/robust_system_test.hex
    if ($LASTEXITCODE -ne 0) { exit 1 }
    Write-Host "[OK] tests/hex/robust_system_test.hex created" -ForegroundColor Green
    
    if ($rv64gcc) {
        Write-Host "`n--- Building RV64 hex ---" -ForegroundColor Yellow
        & riscv64-unknown-elf-gcc -march=rv64imac -mabi=lp64 -O2 -nostdlib `
            -Wl,--entry=main -Wl,--gc-sections `
            tests/full_system/robust_system_test.c -o robust64.elf
        if ($LASTEXITCODE -ne 0) { exit 1 }
        
        & riscv64-unknown-elf-objcopy -O ihex robust64.elf tests/hex/robust_system_test64.hex
        if ($LASTEXITCODE -ne 0) { exit 1 }
        Write-Host "[OK] tests/hex/robust_system_test64.hex created" -ForegroundColor Green
    }
    
    if ($BuildOnly) { exit 0 }
}

# Check simulators exist
$vpPlain = "build_plain/RISCV_VP.exe"
$vpPipe = "build_pipe/RISCV_VP.exe"
$tlmPlain = "build_plain/RISCV_TLM.exe"
$tlmPipe = "build_pipe/RISCV_TLM.exe"

if (-not (Test-Path $vpPlain) -or -not (Test-Path $vpPipe)) {
    Write-Host "`n[ERROR] Simulator executables not found. Build first:" -ForegroundColor Red
    Write-Host "  cmake --build build_plain --target RISCV_VP --config Release" -ForegroundColor Yellow
    Write-Host "  cmake --build build_pipe --target RISCV_VP --config Release" -ForegroundColor Yellow
    exit 1
}

# Run tests
$results = @()

function Run-Test {
    param($Name, $Exe, $Hex, $Arch)
    
    Write-Host "`n--- Running: $Name ---" -ForegroundColor Cyan
    $args = @("-f", $Hex, "-R", $Arch)
    if ($MaxInstr -gt 0) {
        if ($Exe -like "*VP.exe") {
            $args += @("--max-instr", $MaxInstr)
        } else {
            $args += @("-M", $MaxInstr)
        }
    }
    
    $output = & $Exe $args 2>&1 | Out-String
    Write-Host $output
    
    # Parse results
    if ($output -match "Cycles:\s+(\d+)") { $cycles = $matches[1] }
    if ($output -match "Instr/Cycle:\s+([\d.]+)") { $ipc = $matches[1] }
    if ($output -match "Total elapsed time:\s+([\d.]+)s") { $time = $matches[1] }
    
    $results += [PSCustomObject]@{
        Config = $Name
        Cycles = $cycles
        IPC = $ipc
        Time = $time
    }
}

# RV32 tests
if (Test-Path "tests/hex/robust_system_test.hex") {
    Run-Test "RV32-VP-NonPipe" $vpPlain "tests/hex/robust_system_test.hex" "32"
    Run-Test "RV32-VP-Pipeline" $vpPipe "tests/hex/robust_system_test.hex" "32"
    
    if (Test-Path $tlmPlain) {
        Run-Test "RV32-TLM-NonPipe" $tlmPlain "tests/hex/robust_system_test.hex" "32"
    }
    if (Test-Path $tlmPipe) {
        Run-Test "RV32-TLM-Pipeline" $tlmPipe "tests/hex/robust_system_test.hex" "32"
    }
}

# RV64 tests
if (Test-Path "tests/hex/robust_system_test64.hex") {
    Run-Test "RV64-VP-NonPipe" $vpPlain "tests/hex/robust_system_test64.hex" "64"
    Run-Test "RV64-VP-Pipeline" $vpPipe "tests/hex/robust_system_test64.hex" "64"
    
    if (Test-Path $tlmPlain) {
        Run-Test "RV64-TLM-NonPipe" $tlmPlain "tests/hex/robust_system_test64.hex" "64"
    }
    if (Test-Path $tlmPipe) {
        Run-Test "RV64-TLM-Pipeline" $tlmPipe "tests/hex/robust_system_test64.hex" "64"
    }
}

# Summary table
Write-Host "`n=== RESULTS SUMMARY ===" -ForegroundColor Cyan
$results | Format-Table -AutoSize

# Compute improvements
$rv32plain = $results | Where-Object { $_.Config -eq "RV32-VP-NonPipe" }
$rv32pipe = $results | Where-Object { $_.Config -eq "RV32-VP-Pipeline" }

if ($rv32plain -and $rv32pipe) {
    $ipcImprove = [math]::Round((([double]$rv32pipe.IPC - [double]$rv32plain.IPC) / [double]$rv32plain.IPC * 100), 2)
    Write-Host "`nRV32 VP Pipeline IPC improvement: $ipcImprove%" -ForegroundColor Green
}

Write-Host "`nDone." -ForegroundColor Green
