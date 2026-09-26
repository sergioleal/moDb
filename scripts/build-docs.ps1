<#
.SYNOPSIS
Gera o site HTML navegável com toda a documentação Markdown do repositório
(build/docs-site/index.html). Falha se houver link interno quebrado.

.EXAMPLE
.\scripts\build-docs.ps1
.\scripts\build-docs.ps1 -Open
.\scripts\build-docs.ps1 -NoStrict -OutputDir C:\temp\modb-docs
#>

[CmdletBinding()]
param(
    [Parameter()]
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\build\docs-site'),

    [switch]$NoStrict,
    [switch]$Open
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
if (-not $python) { throw "Python 3 não encontrado no PATH (precisa do pacote 'markdown': pip install markdown)." }

$arguments = @((Join-Path $PSScriptRoot 'build_docs_site.py'), '--out', $OutputDir)
if ($NoStrict) { $arguments += '--no-strict' }

& $python.Source @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Open) {
    Start-Process (Join-Path (Resolve-Path $OutputDir).Path 'index.html')
}
