<#
.SYNOPSIS
Envia o benchmark Linux a um ambiente registrado e executa a carga remotamente.

.EXAMPLE
.\scripts\run-remote-benchmark.ps1 -Thousands 100 -Environment linux-remoto

.NOTES
O ambiente (host, usuário padrão, caminho remoto) vem de loadtests/environments.json
(§4.4 do plano de testes de carga) — cadastre novos ambientes editando esse
arquivo, nunca hardcode host neste script. O OpenSSH solicitará a senha no scp
e novamente no ssh. A senha não é armazenada neste script, no registro de
ambientes, nem exposta na linha de comando.
#>

[CmdletBinding()]
param(
    [Parameter()]
    [ValidateRange(1, 1000000000)]
    [long]$Thousands = 10,

    [Parameter()]
    [ValidatePattern('^[a-zA-Z0-9._-]+$')]
    [string]$Environment = 'linux-remoto',

    [Parameter()]
    [ValidatePattern('^[a-zA-Z0-9._-]+$')]
    [string]$User,

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$BinaryPath = (Join-Path $PSScriptRoot '..\build-linux\modb_object_bench'),

    [Parameter()]
    [ValidatePattern('^/[a-zA-Z0-9._/-]+$')]
    [string]$RemotePath,

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$EnvironmentsFile = (Join-Path $PSScriptRoot '..\loadtests\environments.json')
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
        throw "Ambiente '$Id' é kind='$($entry.kind)' — este script só executa carga em ambientes kind='ssh'."
    }
    if (-not $entry.connection -or -not $entry.connection.host) {
        throw "Ambiente '$Id' não tem 'connection.host' configurado em $Path."
    }
    return $entry
}

$env_entry = Resolve-Environment -Id $Environment -Path $EnvironmentsFile
$HostName = $env_entry.connection.host
if (-not $User) { $User = $env_entry.connection.default_user }
if (-not $User) { $User = 'root' }
if (-not $RemotePath) {
    $remoteWorkDir = $env_entry.connection.remote_work_dir
    $binaryName = $env_entry.connection.binary_name
    if ($remoteWorkDir -and $binaryName) {
        $RemotePath = "$remoteWorkDir/$binaryName"
    }
    else {
        $RemotePath = '/tmp/modb_object_bench'
    }
}

Write-Host "Ambiente: $Environment ($($env_entry.label)) -> ${User}@${HostName}"

$scp = Require-Command 'scp'
$ssh = Require-Command 'ssh'
$resolvedBinary = (Resolve-Path -LiteralPath $BinaryPath).Path

# O servidor é tratado como Linux. Impede o envio acidental do .exe produzido
# pelo build local do Windows, que falharia com "Exec format error".
[byte[]]$header = [byte[]]::new(4)
$stream = [System.IO.File]::OpenRead($resolvedBinary)
try {
    $bytesRead = $stream.Read($header, 0, $header.Length)
}
finally {
    $stream.Dispose()
}
$isElf = $header.Length -eq 4 -and
         $bytesRead -eq 4 -and
         $header[0] -eq 0x7f -and
         $header[1] -eq 0x45 -and
         $header[2] -eq 0x4c -and
         $header[3] -eq 0x46
if (-not $isElf) {
    throw "O arquivo não é um executável Linux ELF: $resolvedBinary. Compile o alvo modb_object_bench no Linux/WSL antes do envio."
}

$destination = "${User}@${HostName}:$RemotePath"
$remote = "${User}@${HostName}"

Write-Host "Enviando '$resolvedBinary' para '$destination'..."
& $scp '--' $resolvedBinary $destination
if ($LASTEXITCODE -ne 0) {
    throw "Falha no scp (código $LASTEXITCODE)."
}

Write-Host "Executando carga de $($Thousands * 1000) objetos em $HostName..."
$remoteCommand = "chmod 700 '$RemotePath' && '$RemotePath' '$Thousands'"
& $ssh $remote $remoteCommand
if ($LASTEXITCODE -ne 0) {
    throw "Falha na execução remota (código $LASTEXITCODE)."
}
