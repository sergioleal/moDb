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
    [string]$BinaryPath = (Join-Path $PSScriptRoot '..\build\debug\modb_bench.exe')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$resolvedBinary = (Resolve-Path -LiteralPath $BinaryPath).Path
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
