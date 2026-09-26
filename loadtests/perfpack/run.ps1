<#
.SYNOPSIS
Runner Windows do pacote de performance do moDb (loadtests/perfpack/README.md).

.DESCRIPTION
Faz exatamente o mesmo que run.sh no Linux: lê uma suíte, roda cada caso num
processo novo do modb_load (um work dir limpo por execução, ordem dos casos
alternada entre repetições) e deixa tudo num diretório de resultado
autocontido, pronto para `scripts/perfpack.py fetch` trazer e indexar na série
histórica:

  <results>\<AAAAMMDDTHHMMSSZ>-<ambiente>-<commit12>\
      raw\*.jsonl      um arquivo do modb_load por execução
      logs\*.log       saída de cada execução
      executions.tsv   repetição, caso, código de saída, status, arquivo
      manifest.json    pacote, commit, suíte, sha256 de cada arquivo
      DONE             escrito por último: o resultado está completo

Roda no Windows PowerShell 5.1 (o que vem no Windows). Se o binário pronto não
rodar ou com -Build, compila o modb_load do fonte do MESMO commit que vem no
pacote (exige cmake, ninja e g++ >= 13 no PATH).

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File run.ps1 -Environment desktop-windows

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File run.ps1 -Environment win-bench -Suite smoke
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$Environment,

    [string]$Suite = 'standard',

    [ValidateRange(1, 1000)]
    [int]$Repeat = 0,

    [string]$Only = '',

    [string]$ResultsDir = '',

    [string]$EnvironmentsFile = '',

    [switch]$Build,

    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Direto no stdout do processo: Write-Host vai para o stream de informação, que
# o PowerShell serializa como CLIXML quando a saída não é um console (SSH).
function Say([string]$Text, [switch]$NoNewline) {
    if ($NoNewline) { [Console]::Out.Write($Text) } else { [Console]::Out.WriteLine($Text) }
    [Console]::Out.Flush()
}

$Pack = $PSScriptRoot
if (-not $ResultsDir) { $ResultsDir = Join-Path $Pack '..\results' }
if (-not $EnvironmentsFile) { $EnvironmentsFile = Join-Path $Pack 'environments.json' }

function Fail([string]$Message) {
    [Console]::Error.WriteLine("perfpack: $Message")
    exit 2
}

if (-not (Test-Path -LiteralPath $EnvironmentsFile)) { Fail "catálogo de ambientes não encontrado: $EnvironmentsFile" }
$EnvironmentsFile = (Resolve-Path -LiteralPath $EnvironmentsFile).Path
# O modb_load recusaria caso a caso; melhor recusar antes de começar.
$catalog = [IO.File]::ReadAllText($EnvironmentsFile) | ConvertFrom-Json
if (-not ($catalog.environments | Where-Object { $_.id -eq $Environment })) {
    Fail "ambiente '$Environment' não está cadastrado em $EnvironmentsFile"
}

# --- identidade do pacote ------------------------------------------------------
# PACKAGE.env: KEY=VALUE por linha, escrito por scripts/perfpack.py build.
$Pkg = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $Pack 'PACKAGE.env') -Encoding UTF8) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $at = $line.IndexOf('=')
    if ($at -lt 1) { continue }
    $Pkg[$line.Substring(0, $at)] = $line.Substring($at + 1)
}
foreach ($required in 'PACKAGE_ID', 'MODB_GIT_COMMIT', 'MODB_GIT_BRANCH', 'MODB_GIT_DIRTY', 'MODB_PERFPACK_CMAKE_ARGS') {
    if (-not $Pkg.ContainsKey($required) -or -not $Pkg[$required]) { Fail "PACKAGE.env sem $required" }
}
# O modb_load grava o commit a partir destas variáveis (fora de um repositório
# git não teria como saber com que fonte foi compilado).
$env:MODB_GIT_COMMIT = $Pkg['MODB_GIT_COMMIT']
$env:MODB_GIT_BRANCH = $Pkg['MODB_GIT_BRANCH']
$env:MODB_GIT_DIRTY = $Pkg['MODB_GIT_DIRTY']
$Short = $Pkg['MODB_GIT_COMMIT'].Substring(0, [Math]::Min(12, $Pkg['MODB_GIT_COMMIT'].Length))

# --- suíte ---------------------------------------------------------------------
if ($Suite -match '[\\/]' -or $Suite.EndsWith('.txt')) { $SuiteFile = $Suite }
else { $SuiteFile = Join-Path $Pack "suites\$Suite.txt" }
if (-not (Test-Path -LiteralPath $SuiteFile)) { Fail "suíte não encontrada: $SuiteFile" }
$SuiteName = [IO.Path]::GetFileNameWithoutExtension($SuiteFile)

$SuiteRepeat = 1
$Seed = '123456'
$Cases = New-Object System.Collections.Generic.List[string]
foreach ($raw in Get-Content -LiteralPath $SuiteFile -Encoding UTF8) {
    $text = $raw
    $hash = $text.IndexOf('#')
    if ($hash -ge 0) { $text = $text.Substring(0, $hash) }
    $words = @($text.Trim() -split '\s+' | Where-Object { $_ })
    if ($words.Count -eq 0) { continue }
    switch ($words[0]) {
        'repeat' { $SuiteRepeat = [int]$words[1] }
        'seed' { $Seed = $words[1] }
        'case' {
            if ($words.Count -lt 2) { Fail "${SuiteFile}: 'case' sem id" }
            if (-not $Only -or $words[1].Contains($Only)) {
                $Cases.Add(($words[1..($words.Count - 1)] -join ' '))
            }
        }
        default { Fail "${SuiteFile}: linha não reconhecida: $raw" }
    }
}
if ($Repeat -gt 0) { $SuiteRepeat = $Repeat }
if ($Cases.Count -eq 0) { Fail "nenhum caso selecionado (suíte $SuiteFile, -Only '$Only')" }

# --- binário -------------------------------------------------------------------
$Bin = Join-Path $Pack 'bin\windows-x86_64\modb_load.exe'
$BinaryOrigin = 'prebuilt'

function Test-Binary([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Path list-profiles *> $null; return ($LASTEXITCODE -eq 0) }
    catch { return $false }
    finally { $ErrorActionPreference = $previous }
}

function Build-FromSource {
    foreach ($tool in 'cmake', 'ninja', 'g++') {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "para compilar do fonte é preciso '$tool' no PATH" }
    }
    $src = Join-Path $Pack 'build\src'
    $out = Join-Path $Pack 'build\out'
    [Console]::Error.WriteLine("perfpack: compilando modb_load do fonte ($Short)...")
    # Os objetos do CMake ficam ~110 caracteres abaixo de $out; acima de 260 no
    # total o compilador falha com "No such file or directory" (MAX_PATH).
    if ($out.Length -gt 140) {
        [Console]::Error.WriteLine("perfpack: aviso: caminho longo ($($out.Length) caracteres) -- a compilação pode passar do limite de 260 do Windows; instale o pacote num diretório mais curto.")
    }
    foreach ($dir in $src, $out) { if (Test-Path -LiteralPath $dir) { Remove-Item -Recurse -Force -LiteralPath $dir } }
    New-Item -ItemType Directory -Force -Path $src | Out-Null
    # O tar do Windows (bsdtar): um GNU tar do PATH (Git, MSYS) lê 'C:' como host.
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -xzf (Join-Path $Pack 'src\modb-src.tar.gz') -C $src
    if ($LASTEXITCODE -ne 0) { Fail "falha ao extrair o fonte" }
    $cmakeArgs = @('-S', $src, '-B', $out, '-G', 'Ninja') + ($Pkg['MODB_PERFPACK_CMAKE_ARGS'] -split ' ')
    & cmake @cmakeArgs | ForEach-Object { Say "$_" }
    if ($LASTEXITCODE -ne 0) { Fail "cmake (configuração) falhou" }
    & cmake --build $out --target modb_load | ForEach-Object { Say "$_" }
    if ($LASTEXITCODE -ne 0) { Fail "cmake (build) falhou" }
    $script:Bin = Join-Path $out 'modb_load.exe'
    $script:BinaryOrigin = 'built-on-target'
}

if ($Build) { Build-FromSource }
elseif (-not (Test-Binary $Bin)) {
    [Console]::Error.WriteLine('perfpack: o binário pronto não roda nesta máquina; compilando do fonte.')
    Build-FromSource
}

# --- execução ------------------------------------------------------------------
$Stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$RunName = "$Stamp-$Environment-$Short"
$RunDir = Join-Path $ResultsDir $RunName
$Total = $SuiteRepeat * $Cases.Count

Say "perfpack: pacote $($Pkg['PACKAGE_ID'])  ambiente $Environment  suíte $SuiteName"
Say "perfpack: $($Cases.Count) caso(s) x $SuiteRepeat repetição(ões) = $Total execução(ões); binário $BinaryOrigin"
if ($DryRun) {
    foreach ($spec in $Cases) { Say "  $spec" }
    Say "perfpack: -DryRun, nada executado. Resultado iria para $RunDir"
    exit 0
}

foreach ($sub in 'raw', 'logs', 'work') { New-Item -ItemType Directory -Force -Path (Join-Path $RunDir $sub) | Out-Null }
$RunDir = (Resolve-Path -LiteralPath $RunDir).Path
$StartedAt = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$Tsv = New-Object System.Text.StringBuilder
[void]$Tsv.Append("repetition`tindex`tcase`texit_code`tstatus`tfile`n")

$Failures = 0
$N = 0
for ($rep = 1; $rep -le $SuiteRepeat; $rep++) {
    # Ida nas repetições ímpares, volta nas pares: nenhum caso fica sempre
    # depois do mesmo vizinho.
    $order = @(0..($Cases.Count - 1))
    if ($rep % 2 -eq 0) { [Array]::Reverse($order) }
    foreach ($i in $order) {
        $N++
        $spec = @($Cases[$i] -split ' ')
        $caseId = $spec[0]
        $extra = @()
        if ($spec.Count -gt 1) { $extra = $spec[1..($spec.Count - 1)] }
        $work = Join-Path $RunDir "work\r$rep-c$i"
        $log = Join-Path $RunDir "logs\r$rep-c$i-$caseId.log"
        New-Item -ItemType Directory -Force -Path $work | Out-Null
        Say -NoNewline ("[{0}/{1}] rep {2}  {3} ... " -f $N, $Total, $rep, $caseId)
        $runArgs = @('run', '--profile', 'load-local', '--case', $caseId,
                     '--output-dir', (Join-Path $RunDir 'raw'), '--work-dir', $work,
                     '--environment', $Environment, '--environments-file', $EnvironmentsFile,
                     '--seed', $Seed, '--no-index', '--accept-unknown-budget') + $extra
        # 2>&1 de um executável nativo com ErrorActionPreference=Stop vira
        # exceção no PowerShell 5.1; a saída de erro também vai para o log.
        $ErrorActionPreference = 'Continue'
        $output = & $Bin @runArgs 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        [IO.File]::WriteAllLines($log, [string[]]@($output), $Utf8)
        Remove-Item -Recurse -Force -LiteralPath $work -ErrorAction SilentlyContinue

        $file = ''
        foreach ($l in $output) { if ($l -match '^Resultado:\s*(\S+)') { $file = $Matches[1] } }
        $status = 'completed'
        if ($code -ne 0) { $status = "exit_$code" }
        elseif (-not $file -or -not (Test-Path -LiteralPath $file)) { $status = 'no_result' }
        else {
            $content = [IO.File]::ReadAllText($file)
            if ($content.Contains('"record":"case_error"')) { $status = 'case_error' }
            elseif ($content -notmatch '"record":"case_summary".*"status":"completed"') { $status = 'incomplete' }
        }
        $rel = ''
        if ($file -and (Test-Path -LiteralPath $file)) { $rel = 'raw/' + (Split-Path -Leaf $file) }
        [void]$Tsv.Append("$rep`t$i`t$caseId`t$code`t$status`t$rel`n")
        if ($status -eq 'completed') { Say 'ok' }
        else {
            $Failures++
            Say "FALHOU ($status; ver logs/$(Split-Path -Leaf $log))"
        }
    }
}
Remove-Item -Force -LiteralPath (Join-Path $RunDir 'work') -ErrorAction SilentlyContinue
[IO.File]::WriteAllText((Join-Path $RunDir 'executions.tsv'), $Tsv.ToString(), $Utf8)
$FinishedAt = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$RunStatus = 'completed'
if ($Failures -gt 0) { $RunStatus = 'failed' }

# --- manifesto -----------------------------------------------------------------
$files = @()
$prefix = $RunDir.TrimEnd('\') + '\'
foreach ($f in Get-ChildItem -LiteralPath $RunDir -Recurse -File | Sort-Object FullName) {
    if ($f.Name -eq 'manifest.json' -or $f.Name -eq 'DONE') { continue }
    $files += [ordered]@{
        path   = $f.FullName.Substring($prefix.Length).Replace('\', '/')
        sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        bytes  = $f.Length
    }
}
$manifest = [ordered]@{
    schema         = 'modb.perfpack.run'
    schema_version = 1
    run_name       = $RunName
    environment    = $Environment
    package_id     = $Pkg['PACKAGE_ID']
    git_commit     = $Pkg['MODB_GIT_COMMIT']
    git_branch     = $Pkg['MODB_GIT_BRANCH']
    git_dirty      = ($Pkg['MODB_GIT_DIRTY'] -eq '1')
    runner         = 'run.ps1'
    os             = 'windows'
    cpu_governor   = ''
    binary_origin  = $BinaryOrigin
    suite          = $SuiteName
    only           = $Only
    repeat         = $SuiteRepeat
    seed           = $Seed
    started_at     = $StartedAt
    finished_at    = $FinishedAt
    status         = $RunStatus
    executions     = $Total
    failures       = $Failures
    files          = $files
}
[IO.File]::WriteAllText((Join-Path $RunDir 'manifest.json'), ($manifest | ConvertTo-Json -Depth 5), $Utf8)
# DONE por último: `perfpack.py fetch` só traz resultados completos.
[IO.File]::WriteAllText((Join-Path $RunDir 'DONE'), "$RunStatus`n", $Utf8)

Say "perfpack: $RunStatus ($Failures falha(s) em $Total execução(ões))"
Say "PERFPACK_RUN $RunName $RunStatus"
if ($RunStatus -ne 'completed') { exit 1 }
exit 0
