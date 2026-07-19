# Operação — backup, restauração, supervisor e diagnóstico

Fase **10F**. Complementa [OPERACAO_MODULOS.md](OPERACAO_MODULOS.md) (falhas do
runtime de módulos) com o ciclo operacional do arquivo.

## Papéis dos arquivos

| Arquivo | Conteúdo |
|---|---|
| `<db>` (ex.: `shop.modb`) | páginas do banco (superbloco + dados) |
| `<db>.wal` | log de escrita antecipada; só existe com trabalho pendente ou após crash |

Nunca copie só um dos dois se o WAL existir: o par deve ser consistente.

## Supervisor

O processo do servidor/CLI deve rodar sob um supervisor externo:

- Linux: systemd / Kubernetes
- Windows: Serviço Windows / agendador equivalente

Após crash, a **próxima abertura** do banco executa recovery do WAL (Fase 5):
commits duráveis reaparecem; transações sem commit não. Não há sandbox para
módulos nativos no MVP — um módulo defeituoso pode derrubar o processo; o
supervisor reinicia e o recovery restaura o estado durável.

## Backup quiescente

1. Pare escritores (pare o `modb serve` / feche o processo dono do arquivo).
2. Confirme que não há outro processo com o arquivo aberto.
3. Copie **atomicamente o par**:
   - `shop.modb`
   - `shop.modb.wal` (se existir)
4. Guarde os dois no mesmo snapshot (mesmo diretório/timestamp).

Exemplo (PowerShell, banco parado):

```powershell
New-Item -ItemType Directory -Force backup\2026-07-19 | Out-Null
Copy-Item shop.modb backup\2026-07-19\
if (Test-Path shop.modb.wal) { Copy-Item shop.modb.wal backup\2026-07-19\ }
```

Não faça backup “a quente” sem coordenação: páginas e WAL podem divergir.

## Restauração

1. Pare qualquer processo usando o destino.
2. Restaure `<db>` e, se existir no backup, `<db>.wal` para o mesmo prefixo.
3. Abra o banco (CLI ou API) — a recovery corre na abertura se o WAL estiver
   presente.
4. Rode diagnóstico:

```powershell
.\build\debug\modb.exe db check shop.modb
```

## Diagnóstico — `modb db check`

```powershell
modb db check <file>
```

Camadas:

1. Superbloco / abertura (`PageFile::open`)
2. Classificação por magic (DBRT, IDMD, IDMP, BLBP, IXDR, BTLF, BTIN, SLPG, THRP)
3. Validação de cabeçalhos (versão, length de blob, …)
4. Cadeias de TableHeap e registros legíveis em SLPG

Saída não zero indica páginas/registros inválidos. Bancos só com páginas
reconhecidas e sem erros de heap/registro são considerados verdes para o
critério operacional desta fase.

## Fluxo validado (demo)

```powershell
cmake --build --preset debug
.\build\debug\modb.exe oo employee demo shop.modb --force
.\build\debug\modb.exe db check shop.modb

# backup
New-Item -ItemType Directory -Force .\op-backup | Out-Null
Copy-Item shop.modb, shop.modb.wal -Destination .\op-backup -ErrorAction SilentlyContinue

# “desastre”: remove o original e restaura
Remove-Item shop.modb, shop.modb.wal -ErrorAction SilentlyContinue
Copy-Item .\op-backup\* -Destination .
.\build\debug\modb.exe db check shop.modb
.\build\debug\modb.exe oo employee get shop.modb 1 --schema 2
```

(Ajuste o ObjectId conforme a saída do demo.)

## Relacionados

- Transações / crash: `modb tx demo`, `modb tx crash`, `modb tx wal-info`
- API: [API_PUBLICA.md](API_PUBLICA.md)
- Formato: [FORMATO_DE_ARQUIVO.md](FORMATO_DE_ARQUIVO.md)
