<#
.SYNOPSIS
Compila o moDb com instrumentação --coverage, roda a suíte de testes via CTest
e gera um relatório de cobertura com gcovr (se instalado) ou gcov bruto.

.EXAMPLE
.\scripts\run-coverage.ps1
.\scripts\run-coverage.ps1 -SkipBuild -SkipTests
#>

[CmdletBinding()]
param(
    [Parameter()]
    [string]$ConfigurePreset = 'coverage',

    [Parameter()]
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\coverage-results'),

    [switch]$SkipBuild,
    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDir = Join-Path $root "build\$ConfigurePreset"

if (-not $SkipBuild) {
    Write-Host "Configurando preset '$ConfigurePreset' ..."
    cmake --preset $ConfigurePreset
    if ($LASTEXITCODE -ne 0) { throw "cmake --preset $ConfigurePreset falhou" }

    Write-Host "Compilando ..."
    cmake --build --preset $ConfigurePreset
    if ($LASTEXITCODE -ne 0) { throw "cmake --build --preset $ConfigurePreset falhou" }
}

if (-not $SkipTests) {
    Write-Host "Executando testes (gera os .gcda) ..."
    ctest --preset $ConfigurePreset
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "ctest reportou falhas - o relatorio de cobertura ainda sera gerado com os dados coletados."
    }
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

# gcov precisa corresponder a versao do compilador usado para gerar os .gcno,
# entao deriva do CMakeCache em vez de confiar num `gcov` generico do PATH.
$cacheMatch = Select-String -Path (Join-Path $buildDir 'CMakeCache.txt') -Pattern '^CMAKE_CXX_COMPILER:FILEPATH=(.+)$'
$cxxCompiler = $cacheMatch.Matches[0].Groups[1].Value
$compilerDir = Split-Path $cxxCompiler -Parent
$gcovExe = Join-Path $compilerDir 'gcov.exe'
if (-not (Test-Path $gcovExe)) { $gcovExe = 'gcov' }

$gcovrCmd = Get-Command gcovr -ErrorAction SilentlyContinue
if ($gcovrCmd) {
    Write-Host "Gerando relatorio com gcovr ..."
    $htmlPath = Join-Path $OutputDir 'coverage.html'
    $txtPath = Join-Path $OutputDir 'coverage.txt'
    & gcovr --root $root --filter "include/modb/.*" --filter "src/.*" `
        --gcov-executable $gcovExe --object-directory $buildDir `
        --print-summary --html-details $htmlPath -o $txtPath
    if ($LASTEXITCODE -ne 0) { throw "gcovr falhou" }
    Write-Host "Relatorio: $txtPath / $htmlPath"
} else {
    Write-Warning "gcovr nao encontrado no PATH (instale com 'pip install gcovr' para um relatorio agregado em HTML). Gerando saida bruta do gcov ..."
    $gcdaFiles = Get-ChildItem -Path $buildDir -Recurse -Filter '*.gcda' -ErrorAction SilentlyContinue
    if (-not $gcdaFiles) {
        throw "Nenhum arquivo .gcda encontrado em $buildDir - confirme que MODB_ENABLE_COVERAGE=ON e que os testes rodaram."
    }
    Push-Location $OutputDir
    try {
        foreach ($gcda in $gcdaFiles) {
            & $gcovExe --object-directory $gcda.DirectoryName $gcda.FullName *> $null
        }
    } finally {
        Pop-Location
    }
    Write-Host "Arquivos .gcov brutos gerados em $OutputDir (um por unidade de traducao)."
}
