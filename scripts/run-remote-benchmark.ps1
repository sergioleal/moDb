<#
.SYNOPSIS
Envia o benchmark Linux ao servidor e executa a carga remotamente.

.EXAMPLE
.\scripts\run-remote-benchmark.ps1 -Thousands 100 -User root

.NOTES
O OpenSSH solicitará a senha no scp e novamente no ssh. A senha não é
armazenada neste script nem exposta na linha de comando.
#>

[CmdletBinding()]
param(
    [Parameter()]
    [ValidateRange(1, 1000000000)]
    [long]$Thousands = 10,

    [Parameter()]
    [ValidatePattern('^[a-zA-Z0-9._-]+$')]
    [string]$User = 'root',

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$BinaryPath = (Join-Path $PSScriptRoot '..\build-linux\modb_object_bench'),

    [Parameter()]
    [ValidatePattern('^/[a-zA-Z0-9._/-]+$')]
    [string]$RemotePath = '/tmp/modb_object_bench'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$HostName = '161.35.9.43'

function Require-Command {
    param([Parameter(Mandatory)][string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "Comando '$Name' não encontrado. Instale o cliente OpenSSH do Windows."
    }
    return $command.Source
}

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
