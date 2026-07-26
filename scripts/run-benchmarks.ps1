<#
.SYNOPSIS
Executa uma campanha modb_bench e imprime o JSONL gerado.

.EXAMPLE
.\scripts\run-benchmarks.ps1 -Profile smoke -Seed 1
#>

[CmdletBinding()]
param(
    [Parameter()]
    [ValidateSet('smoke', 'standard', 'diagnostic')]
    [string]$Profile = 'smoke',

    [Parameter()]
    [long]$Seed = 1,

    [Parameter()]
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\benchmark-results'),

    [Parameter()]
    [string]$BinaryPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $BinaryPath) {
    # Ordem deliberada: builds otimizados primeiro. Gate oficial nunca sai de
    # Debug (-O0) -- ver docs-process/PLANO_PROFILING.md §3, defeito M1.
    $candidates = @(
        (Join-Path $PSScriptRoot '..\build\relwithdebinfo\modb_bench.exe'),
        (Join-Path $PSScriptRoot '..\build\release\modb_bench.exe'),
        (Join-Path $PSScriptRoot '..\build\debug\modb_bench.exe')
    )
    $BinaryPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $BinaryPath -or -not (Test-Path -LiteralPath $BinaryPath)) {
    throw "modb_bench.exe não encontrado em build\relwithdebinfo\, build\release\ nem build\debug\ -- compile com 'cmake --build --preset relwithdebinfo' ou passe -BinaryPath."
}

$resolvedBinary = (Resolve-Path -LiteralPath $BinaryPath).Path
if ($resolvedBinary -match '\\build\\debug\\') {
    Write-Warning "usando o binário Debug (-O0): valida o runner, não mede desempenho. Gates oficiais usam Release/RelWithDebInfo."
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

Write-Host "Executando modb_bench run --profile $Profile --seed $Seed ..."
& $resolvedBinary run --profile $Profile --seed $Seed --output-dir $OutputDir
if ($LASTEXITCODE -ne 0) {
    throw "modb_bench falhou com codigo $LASTEXITCODE"
}

$latest = Get-ChildItem -LiteralPath $OutputDir -Filter 'modb-benchmark-*.jsonl' |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $latest) {
    throw "Nenhum JSONL encontrado em $OutputDir"
}
Write-Host "Resultado: $($latest.FullName)"
