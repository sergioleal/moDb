<#
.SYNOPSIS
Executa `modb_load run` localmente, lendo os seletores de um arquivo YAML.

.DESCRIPTION
Lê um subconjunto restrito de YAML (chave: valor escalar, ou chave: seguida de
"  - item" por linha para listas — sem aspas, sem lista em uma linha, sem
aninhamento além de um nível). O formato completo está documentado no
cabeçalho de loadtests/config/load-local.yaml.

`modb_load` ainda não existe (docs/PLANO_TESTES_DE_CARGA.md, Subfases A/B).
Use -DryRun para ver o comando resolvido sem exigir que o binário esteja
construído — o script continua útil como validador da configuração.

.EXAMPLE
.\scripts\run-load.ps1
Usa loadtests\config\load-local.yaml.

.EXAMPLE
.\scripts\run-load.ps1 -ConfigPath loadtests\config\load-standard.yaml -DryRun

.EXAMPLE
.\scripts\run-load.ps1 -Environment linux-remoto
Sobrescreve a lista `environment:` do YAML sem editar o arquivo.
#>

[CmdletBinding()]
param(
    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$ConfigPath = (Join-Path $PSScriptRoot '..\loadtests\config\load-local.yaml'),

    [Parameter()]
    [string]$BinaryPath,

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$EnvironmentsFile = (Join-Path $PSScriptRoot '..\loadtests\environments.json'),

    [Parameter()]
    [string]$Environment,

    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-RestrictedYaml {
    param([Parameter(Mandatory)][string]$Path)

    $config = [ordered]@{}
    $currentListKey = $null
    $lineNumber = 0

    foreach ($rawLine in Get-Content -LiteralPath $Path -Encoding UTF8) {
        $lineNumber++
        $line = $rawLine.TrimEnd()
        $trimmedStart = $line.TrimStart()
        if ($trimmedStart -eq '' -or $trimmedStart.StartsWith('#')) {
            continue
        }

        if ($line -match '^\s+-\s*(.*)$') {
            $item = $Matches[1].Trim()
            if (-not $currentListKey) {
                throw "${Path}:${lineNumber}: item de lista sem chave anterior: '$rawLine'"
            }
            if ($null -eq $config[$currentListKey]) {
                $config[$currentListKey] = @()
            }
            elseif ($config[$currentListKey] -isnot [array]) {
                throw "${Path}:${lineNumber}: '$currentListKey' já tem valor escalar; não pode virar lista."
            }
            $config[$currentListKey] = @($config[$currentListKey]) + $item
            continue
        }

        if ($trimmedStart -match '^([A-Za-z0-9_]+):\s*(.*)$') {
            $key = $Matches[1]
            $value = $Matches[2].Trim()
            if ($value -eq '') {
                $config[$key] = $null
                $currentListKey = $key
            }
            else {
                $config[$key] = $value
                $currentListKey = $null
            }
            continue
        }

        throw "${Path}:${lineNumber}: linha fora do subconjunto restrito de YAML: '$rawLine'"
    }

    return $config
}

function Add-Arg {
    param(
        [Parameter(Mandatory)][System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory)][string]$Flag,
        $Value
    )
    if ($null -eq $Value) { return }
    if ($Value -is [array]) {
        if ($Value.Count -eq 0) { return }
        $List.Add($Flag)
        $List.Add(($Value -join ','))
        return
    }
    if ([string]::IsNullOrWhiteSpace([string]$Value)) { return }
    $List.Add($Flag)
    $List.Add([string]$Value)
}

$resolvedConfigPath = (Resolve-Path -LiteralPath $ConfigPath).Path
$config = Read-RestrictedYaml -Path $resolvedConfigPath

if ($Environment) {
    $config['environment'] = @($Environment -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
}

# Valida ambientes citados contra o catálogo (§4.4) antes de montar o comando —
# erro de digitação no ambiente é mais barato de pegar aqui do que depois de
# `modb_load` já ter subido.
if (Test-Path -LiteralPath $EnvironmentsFile) {
    $catalog = Get-Content -LiteralPath $EnvironmentsFile -Raw -Encoding UTF8 | ConvertFrom-Json
    $envList = $config['environment']
    foreach ($envId in @($envList)) {
        if (-not $envId) { continue }
        $entry = $catalog.environments | Where-Object { $_.id -eq $envId }
        if (-not $entry) {
            $known = ($catalog.environments | ForEach-Object { $_.id }) -join ', '
            throw "Ambiente '$envId' (config $resolvedConfigPath) não está cadastrado em $EnvironmentsFile. Conhecidos: $known"
        }
        if ($entry.kind -eq 'ssh') {
            Write-Warning "Ambiente '$envId' é kind='ssh' -- run-load.ps1 executa localmente. Use scripts/run-remote-benchmark.ps1 (ou o futuro run-remote-load) para executar nele."
        }
    }
}
else {
    Write-Warning "Registro de ambientes não encontrado em $EnvironmentsFile -- pulando validação."
}

$argsList = [System.Collections.Generic.List[string]]::new()
$argsList.Add('run')
Add-Arg $argsList '--profile' $config['profile']
Add-Arg $argsList '--seed' $config['seed']
Add-Arg $argsList '--output-dir' $config['output_dir']
Add-Arg $argsList '--work-dir' $config['work_dir']
Add-Arg $argsList '--filter' $config['filter']
Add-Arg $argsList '--exclude' $config['exclude']
Add-Arg $argsList '--repeat' $config['repeat']
Add-Arg $argsList '--max-duration' $config['max_duration']
Add-Arg $argsList '--max-disk-gb' $config['max_disk_gb']
Add-Arg $argsList '--max-rss-mb' $config['max_rss_mb']
Add-Arg $argsList '--scale' $config['scale']
Add-Arg $argsList '--workload' $config['workload']
Add-Arg $argsList '--target' $config['target']
Add-Arg $argsList '--environment' $config['environment']
Add-Arg $argsList '--concurrency' $config['concurrency']
Add-Arg $argsList '--payload' $config['payload']
Add-Arg $argsList '--case' $config['case']

if ("$($config['accept_unknown_budget'])" -eq 'true') { $argsList.Add('--accept-unknown-budget') }
if ("$($config['dry_run'])" -eq 'true') { $argsList.Add('--dry-run') }

Write-Host "Configuração: $resolvedConfigPath"
Write-Host ("Comando: modb_load " + ($argsList -join ' '))

if ($DryRun) {
    Write-Host '-DryRun: nada foi executado.'
    return
}

if (-not $BinaryPath) {
    $candidates = @(
        (Join-Path $PSScriptRoot '..\build\debug\modb_load.exe'),
        (Join-Path $PSScriptRoot '..\build\release\modb_load.exe')
    )
    $BinaryPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}

if (-not $BinaryPath -or -not (Test-Path -LiteralPath $BinaryPath)) {
    throw "modb_load não encontrado. Ele ainda não está implementado -- ver docs/PLANO_TESTES_DE_CARGA.md, Subfases A/B. Use -DryRun para só ver o comando que seria executado."
}

$resolvedBinary = (Resolve-Path -LiteralPath $BinaryPath).Path
& $resolvedBinary @argsList
if ($LASTEXITCODE -ne 0) {
    throw "modb_load falhou com código $LASTEXITCODE"
}
