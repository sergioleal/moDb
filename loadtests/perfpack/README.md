# Pacote de performance (`perfpack`)

Um pacote único para medir o moDb em **Linux e Windows do mesmo jeito**, rodar
por SSH e trazer os resultados para a série histórica
(`load-history/series.jsonl`), onde `modb_load trend/report/gate` e o
dashboard comparam commits e máquinas.

```
máquina de desenvolvimento                      máquina medida (Linux ou Windows)
─────────────────────────                       ─────────────────────────────────
perfpack.py build   ─── pacote .tar.gz ───────▶  ~/modb-perfpack/<pacote>/
perfpack.py deploy                                run.sh  |  run.ps1   (mesma suíte,
perfpack.py run     ─── ssh ──────────────────▶        mesmos argumentos)
perfpack.py fetch   ◀── scp + sha256 ─────────  ~/modb-perfpack/results/<rodada>/
      │
      └─▶ load-results/remote/<ambiente>/<rodada>/   (brutos, fora do git)
          load-history/series.jsonl                   (série, versionada)
```

## O que vai no pacote

| caminho | o que é |
|---|---|
| `bin/linux-x86_64/modb_load` | binário Linux (compilado no WSL, libstdc++ estática; exige glibc ≥ 2.39) |
| `bin/windows-x86_64/` | `modb_load.exe` e as três DLLs do runtime MinGW |
| `src/modb-src.tar.gz` | o fonte do **mesmo commit** (sem `docs-process` e sem mídia) |
| `run.sh`, `run.ps1` | runners equivalentes: mesma suíte, mesma ordem, mesmos argumentos |
| `suites/standard.txt` | suíte padrão: 10 casos `embedded`, 5 repetições, ~5 min |
| `suites/smoke.txt` | confere que roda (< 30 s); não serve para comparar |
| `environments.json` | o catálogo de ambientes do commit (o `deploy`/`run` reenviam o atual) |
| `PACKAGE.env`, `PACKAGE.json` | commit, branch, árvore, opções do CMake, sha256 de cada arquivo |

As duas plataformas usam o build de referência do profiling: `RelWithDebInfo` com
frame pointers (`MODB_ENABLE_PROFILING=ON`). Os binários prontos vão sem a
informação de depuração (`strip --strip-debug`): o código de máquina é o mesmo e
a tabela de símbolos fica. Se o binário pronto não rodar no destino (glibc mais
antiga, por exemplo), o runner **compila ali** o fonte do mesmo commit — precisa
de `cmake`, `ninja` e `g++ ≥ 13` — e registra `binary_origin: built-on-target`
no manifesto.

O commit é gravado em cada resultado. Fora de um repositório git o `modb_load`
não saberia qual é, então o runner o informa por `MODB_GIT_COMMIT`,
`MODB_GIT_BRANCH` e `MODB_GIT_DIRTY` (lidos de `PACKAGE.env`).

## Cadastrar uma máquina

Cada máquina é um ambiente de `loadtests/environments.json`. O id vai em cada
resultado e separa as séries: números de máquinas diferentes nunca se misturam.

```json
{
  "id": "bench-win-01",
  "label": "Servidor Windows de benchmark",
  "kind": "ssh",
  "host_class": "bench-windows-01",
  "device_class": "nvme",
  "os_hint": "windows",
  "connection": { "host": "10.0.0.5", "default_user": "bench" },
  "perfpack": { "transport": "ssh", "os": "windows", "remote_dir": "modb-perfpack" }
}
```

- `perfpack.transport`: `ssh` (máquina remota), `wsl` (o Linux do WSL desta
  máquina, com `distro`) ou `local` (este Windows). Os dois últimos passam pelo
  mesmo caminho do SSH, sem SSH, e servem para testar o pacote.
- `remote_dir` relativo é relativo ao home do usuário remoto, nos dois sistemas.
  Não use `/tmp`: os resultados precisam sobreviver até o `fetch`.
- `perfpack.port` (padrão 22), `perfpack.host` e `perfpack.user` sobrescrevem
  `connection`.
- Senha nunca vai para o catálogo nem para os scripts: use chave SSH, ou o
  OpenSSH pergunta. Um aviso de chave de host diferente em `known_hosts` é para
  resolver **antes**, não para contornar.

No destino, o pacote só precisa de: Linux, bash ≥ 4, coreutils e tar; Windows,
o OpenSSH Server e o Windows PowerShell 5.1 que vêm com o sistema (o `tar.exe`
do Windows é usado para extrair).

## Uso

```bash
python scripts/perfpack.py build                       # pacote do commit atual (exige árvore limpa)
python scripts/perfpack.py all -e linux-remoto         # deploy + run (suíte standard) + fetch
python scripts/perfpack.py all -e bench-win-01 --suite smoke
```

Passo a passo, quando convém separar:

```bash
python scripts/perfpack.py deploy -e linux-remoto
python scripts/perfpack.py run    -e linux-remoto --detach   # Linux: sobrevive à queda do SSH
python scripts/perfpack.py status -e linux-remoto            # o que já terminou, o que falta trazer
python scripts/perfpack.py fetch  -e linux-remoto            # traz, confere sha256, indexa
```

- `run` aceita `--suite`, `--repeat N`, `--only SUBSTR` (só casos cujo id
  contém o texto) e `--build` (compila no destino mesmo com binário pronto).
- `--detach` é só para Linux (`nohup setsid`). No Windows o OpenSSH encerra os
  processos da sessão ao desconectar: rode sem `--detach`, ou direto na máquina.
- `fetch` é idempotente: só traz rodadas com `DONE` que ainda não estão em
  `load-results/remote/<ambiente>/`, confere cada arquivo contra o
  `manifest.json` (sha256, tamanho, nada faltando ou sobrando) e indexa os
  `.jsonl` na série. Rodadas de outro ambiente no mesmo diretório remoto são
  ignoradas.
- `build --allow-dirty` monta um pacote da árvore de trabalho (via `git stash
  create`, sem mexer em nada); os resultados saem com `git_dirty: true`, e o id
  do pacote leva `-dirty-<árvore>`. Serve para testar o pacote, não para a série.

### Rodar à mão na máquina

O pacote também roda sozinho, sem o `perfpack.py`:

```bash
tar -xzf modb-perfpack-<commit>.tar.gz
./modb-perfpack-<commit>/run.sh --environment linux-remoto
```

```powershell
tar.exe -xzf modb-perfpack-<commit>.tar.gz
powershell -NoProfile -ExecutionPolicy Bypass -File .\modb-perfpack-<commit>\run.ps1 -Environment bench-win-01
```

O resultado fica em `results/` ao lado do pacote. Copiado para cá (pendrive,
scp à mão), entra na série com:

```bash
python scripts/perfpack.py import caminho/para/<rodada>
```

## O que uma rodada produz

```
results/<AAAAMMDDTHHMMSSZ>-<ambiente>-<commit12>/
    raw/*.jsonl       um arquivo do modb_load por execução (caso x repetição)
    logs/*.log        saída de cada execução
    executions.tsv    repetição, caso, código de saída, status, arquivo
    manifest.json     pacote, commit, suíte, SO, origem do binário, sha256 de tudo
    DONE              última coisa escrita: "completed" ou "failed"
```

Cada execução é um processo novo com work dir limpo, e a ordem dos casos
alterna entre repetições (ida, volta, ...), como em `scripts/measure_load.py`.
Uma execução que falha não interrompe a suíte: fica em `executions.tsv` com o
status (`exit_N`, `no_result`, `case_error`, `incomplete`), a rodada termina
`failed`, e as execuções que completaram entram na série normalmente.

## Análise histórica

A série é a de sempre: um ponto por caso e execução, com commit, ambiente,
`host_class`, SO, CPU, sistema de arquivos e compilador (registro `environment`
do `modb_load`).

```bash
modb_load trend  --case load.crud_full.embedded.100k --metric ops_per_second --phase update_shrink
modb_load report --case load.crud_full.embedded.100k --format csv
modb_load gate   --case load.mixed_oltp.embedded.10k --metric ops_per_second
```

O `trend` separa uma série por ambiente e compara cada ponto com a mediana dos
anteriores. O dashboard (`loadtests/dashboard/index.html`, abra o
`load-history/series.jsonl`) filtra por ambiente e mostra os commits lado a
lado. Faça commit do `series.jsonl` depois de um `fetch` para a série ficar no
repositório; os brutos em `load-results/` ficam locais.

## Verificado e não verificado

Testado nesta máquina: pacote montado (Windows + Linux, 11,9 MB); `deploy`,
`run`, `status`, `fetch` e `import` pelos transportes `local` (Windows) e `wsl`
(Ubuntu 24.04); compilação no destino (`--build`) nos dois sistemas; rodada com
caso que falha; ambiente não cadastrado; arquivo adulterado recusado; `fetch`
repetido; `--detach`.

**Não testado:** o transporte `ssh` em si (não havia servidor SSH acessível). Ele
só troca a execução local por `ssh`/`scp`; os comandos que monta foram
conferidos, mas a primeira rodada real contra um host é a verificação que falta.
