<#
.SYNOPSIS
Envia `modb_load` (binário Linux) a um ambiente registrado, executa uma
campanha remotamente e traz de volta exatamente um arquivo de resultado.

.DESCRIPTION
Subfase I (docs/PLANO_TESTES_DE_CARGA.md §11): reaproveita a resolução de
ambiente já usada em scripts/run-remote-benchmark.ps1 (host/usuário vêm de
loadtests/environments.json, §4.4 -- nunca hardcoded aqui). Diferente do
benchmark antigo, este script TAMBÉM copia de volta o `.jsonl` produzido
remotamente (§11 item 5), imprime seu SHA-256 (item 7) e, por padrão, indexa
o arquivo trazido no `load-history/series.jsonl` local (§13.5) -- a leitura
do JSONL é a mesma independente de qual SO o produziu.

Alvos `embedded`/`loopback`/`remote_colocated` fazem sentido rodando aqui
(o binário remoto enxerga cliente+servidor locais entre si quando o alvo é
`loopback`/`remote_colocated` -- ver loadtests/workloads/create_only.cpp).
`remote_client_local` ainda não tem dispatch (Subfase I, versão mínima:
sem uma segunda máquina disponível para verificar de verdade nesta rodada).

NÃO VERIFICADO CONTRA UM HOST REMOTO DE VERDADE nesta subfase -- o ambiente
`linux-remoto` cadastrado em loadtests/environments.json tinha a chave SSH
diferente da registrada em known_hosts no momento da implementação (aviso de
segurança do OpenSSH, não contornado por este script de propósito). Escrito
seguindo de perto o padrão já em produção de run-remote-benchmark.ps1;
confirme manualmente contra um host acessível antes do primeiro uso real.

.EXAMPLE
.\scripts\run-remote-load.ps1 -Case load.create_only.remote_colocated.10k -Environment linux-remoto -AcceptUnknownBudget

.EXAMPLE
.\scripts\run-remote-load.ps1 -Profile load-smoke -Environment linux-remoto -AcceptUnknownBudget -NoIndex
#>

[CmdletBinding()]
param(
    [Parameter()]
    [ValidatePattern('^[a-zA-Z0-9._-]+$')]
    [string]$Environment = 'linux-remoto',

    [Parameter()]
    [string]$Profile,

    [Parameter()]
    [string]$CaseId,

    [Parameter()]
    [string]$Scale,

    [Parameter()]
    [string]$Workload,

    [Parameter()]
    [string]$Target,

    [Parameter()]
    [long]$Seed,

    [Parameter()]
    [int]$Repeat,

    [switch]$AcceptUnknownBudget,

    [Parameter()]
    [ValidatePattern('^[a-zA-Z0-9._-]+$')]
    [string]$User,

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$BinaryPath = (Join-Path $PSScriptRoot '..\build-linux\modb_load'),

    [Parameter()]
    [ValidatePattern('^/[a-zA-Z0-9._/-]+$')]
    [string]$RemotePath,

    [Parameter()]
    [ValidatePattern('^/[a-zA-Z0-9._/-]+$')]
    [string]$RemoteWorkDir,

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$EnvironmentsFile = (Join-Path $PSScriptRoot '..\loadtests\environments.json'),

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$LocalResultsDir = (Join-Path $PSScriptRoot '..\load-results\remote'),

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$LocalBinaryPath = (Join-Path $PSScriptRoot '..\build\debug\modb_load.exe'),

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$HistoryFile = (Join-Path $PSScriptRoot '..\load-history\series.jsonl'),

    [switch]$NoIndex
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require-Command {
    param([Parameter(Mandatory)][string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "Comando '$Name' não encontrado. Instale o cliente OpenSSH do Windows."
    }
    return $command.Source
}

function Resolve-Environment {
    param(
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Registro de ambientes não encontrado: $Path"
    }
    $catalog = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $entry = $catalog.environments | Where-Object { $_.id -eq $Id }
    if (-not $entry) {
        $known = ($catalog.environments | ForEach-Object { $_.id }) -join ', '
        throw "Ambiente '$Id' não está cadastrado em $Path. Ambientes conhecidos: $known"
    }
    if ($entry.kind -ne 'ssh') {
        throw "Ambiente '$Id' é kind='$($entry.kind)' -- este script só executa em ambientes kind='ssh'."
    }
    if (-not $entry.connection -or -not $entry.connection.host) {
        throw "Ambiente '$Id' não tem 'connection.host' configurado em $Path."
    }
    return $entry
}

function Add-Arg {
    param(
        [Parameter(Mandatory)][System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory)][string]$Flag,
        $Value
    )
    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) { return }
    $List.Add($Flag)
    $List.Add([string]$Value)
}

if (-not $Profile -and -not $CaseId) {
    throw "Informe -Profile ou -CaseId (mesma regra de 'modb_load run', §6.1)."
}

$env_entry = Resolve-Environment -Id $Environment -Path $EnvironmentsFile
$HostName = $env_entry.connection.host
if (-not $User) { $User = $env_entry.connection.default_user }
if (-not $User) { $User = 'root' }
if (-not $RemoteWorkDir) {
    $RemoteWorkDir = $env_entry.connection.remote_work_dir
    if (-not $RemoteWorkDir) { $RemoteWorkDir = '/tmp/modb_load_run' }
}
if (-not $RemotePath) {
    $binaryName = $env_entry.connection.binary_name
    if (-not $binaryName) { $binaryName = 'modb_load' }
    $RemotePath = "$RemoteWorkDir/$binaryName"
}

Write-Host "Ambiente: $Environment ($($env_entry.label)) -> ${User}@${HostName}"

$scp = Require-Command 'scp'
$ssh = Require-Command 'ssh'

if (-not (Test-Path -LiteralPath $BinaryPath)) {
    throw "Binário Linux de modb_load não encontrado em '$BinaryPath'. Compile o alvo modb_load num build Linux/WSL antes do envio (ver README-BUILD-LINUX.md, se existir, ou replique o build/debug local em build-linux/)."
}
$resolvedBinary = (Resolve-Path -LiteralPath $BinaryPath).Path

# Mesma checagem de run-remote-benchmark.ps1: recusa enviar um binário que
# não é ELF (ex.: o .exe do build local do Windows), que falharia com
# "Exec format error" só depois do scp, gastando banda à toa.
[byte[]]$header = [byte[]]::new(4)
$stream = [System.IO.File]::OpenRead($resolvedBinary)
try {
    $bytesRead = $stream.Read($header, 0, $header.Length)
}
finally {
    $stream.Dispose()
}
$isElf = $header.Length -eq 4 -and $bytesRead -eq 4 -and
         $header[0] -eq 0x7f -and $header[1] -eq 0x45 -and
         $header[2] -eq 0x4c -and $header[3] -eq 0x46
if (-not $isElf) {
    throw "O arquivo não é um executável Linux ELF: $resolvedBinary. Compile modb_load no Linux/WSL antes do envio."
}

$destination = "${User}@${HostName}:$RemotePath"
$remote = "${User}@${HostName}"

Write-Host "Enviando '$resolvedBinary' para '$destination'..."
& $ssh $remote "mkdir -p '$RemoteWorkDir'"
if ($LASTEXITCODE -ne 0) {
    throw "Falha ao criar diretório remoto (código $LASTEXITCODE)."
}
& $scp '--' $resolvedBinary $destination
if ($LASTEXITCODE -ne 0) {
    throw "Falha no scp (código $LASTEXITCODE)."
}

$argsList = [System.Collections.Generic.List[string]]::new()
$argsList.Add('run')
Add-Arg $argsList '--profile' $Profile
Add-Arg $argsList '--case' $CaseId
Add-Arg $argsList '--scale' $Scale
Add-Arg $argsList '--workload' $Workload
Add-Arg $argsList '--target' $Target
Add-Arg $argsList '--seed' $Seed
Add-Arg $argsList '--repeat' $Repeat
Add-Arg $argsList '--output-dir' $RemoteWorkDir
Add-Arg $argsList '--work-dir' $RemoteWorkDir
# `--no-index` -- este script indexa a cópia LOCAL do resultado (abaixo), não
# faz sentido o binário remoto tentar escrever load-history/ lá também.
$argsList.Add('--no-index')
if ($AcceptUnknownBudget) { $argsList.Add('--accept-unknown-budget') }

$remoteCommand = "chmod 700 '$RemotePath' && '$RemotePath' " + (($argsList | ForEach-Object { "'$_'" }) -join ' ')
Write-Host "Executando em ${HostName}: $remoteCommand"
$remoteOutput = & $ssh $remote $remoteCommand
if ($LASTEXITCODE -ne 0) {
    $remoteOutput | ForEach-Object { Write-Host $_ }
    throw "Falha na execução remota (código $LASTEXITCODE)."
}
$remoteOutput | ForEach-Object { Write-Host $_ }

# `modb_load run` imprime "Resultado: <caminho>  run_id=... status=..." em
# stdout (loadtests/modb_load.cpp, command_run) -- é daí que vem o caminho
# remoto exato, nunca um glob sobre o diretório de trabalho (§11 item 5: UM
# arquivo por campanha, não "o mais recente que achar").
$resultLine = $remoteOutput | Where-Object { $_ -match '^Resultado:\s*(\S+)\s+run_id=(\S+)\s+status=(\S+)' } | Select-Object -Last 1
if (-not $resultLine) {
    throw "Não foi possível encontrar a linha 'Resultado: ...' na saída remota -- não sei qual arquivo trazer de volta."
}
$null = $resultLine -match '^Resultado:\s*(\S+)\s+run_id=(\S+)\s+status=(\S+)'
$remoteResultPath = $Matches[1]
$runId = $Matches[2]
$status = $Matches[3]

New-Item -ItemType Directory -Force -Path $LocalResultsDir | Out-Null
$localFileName = Split-Path -Leaf $remoteResultPath
$localDestination = Join-Path $LocalResultsDir $localFileName
if (Test-Path -LiteralPath $localDestination) {
    throw "Já existe um arquivo local em '$localDestination' -- não sobrescrevo (§11 item 8). Mova-o ou apague-o antes de rodar de novo."
}

Write-Host "Copiando '${remote}:${remoteResultPath}' para '$localDestination'..."
& $scp '--' "${remote}:${remoteResultPath}" $localDestination
if ($LASTEXITCODE -ne 0) {
    throw "Falha ao copiar o resultado de volta (código $LASTEXITCODE)."
}

$hash = (Get-FileHash -LiteralPath $localDestination -Algorithm SHA256).Hash.ToLowerInvariant()
$size = (Get-Item -LiteralPath $localDestination).Length
Write-Host "Trazido: $localDestination"
Write-Host "  run_id=$runId  status=$status  bytes=$size  sha256=$hash"

if (-not $NoIndex) {
    if (-not (Test-Path -LiteralPath $LocalBinaryPath)) {
        Write-Warning "modb_load local não encontrado em '$LocalBinaryPath' -- pulando indexação. Rode 'modb_load index $localDestination' manualmente."
    }
    else {
        Write-Host "Indexando em $HistoryFile..."
        & $LocalBinaryPath index $localDestination '--history-file' $HistoryFile '--environments-file' $EnvironmentsFile
        if ($LASTEXITCODE -gt 1) {
            throw "Indexação falhou com código $LASTEXITCODE."
        }
    }
}
