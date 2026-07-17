# ADR-010 — Protocolo binário próximo do armazenamento lógico

- Estado: aceito para a Fase 8
- Data: 2026-07-17

## Contexto

O servidor precisa transferir objetos e resultados de métodos com baixa
latência, poucas alocações e o mínimo possível de recodificação. O armazenamento
já possui um codec binário genérico, registros de tamanho variável e páginas
com diretório de slots. Reaproveitar esses princípios no protocolo reduz a
distância entre leitura persistente, serialização e envio pela rede.

Copiar literalmente páginas e slots, porém, transformaria detalhes físicos em
contrato público. Um objeto pode mudar de página ou slot por atualização,
compactação ou MVCC; `RecordId{PageId, SlotId, generation}` é localização
física, enquanto `ObjectId` é sua identidade permanente. Formato de página,
tamanho de página, checksums e headers do Buffer Pool também precisam poder
evoluir sem exigir atualização simultânea de todos os clientes.

## Decisão

O protocolo será próximo do **armazenamento lógico**, não uma exposição do
armazenamento físico.

O payload de valores e objetos reutiliza o codec canônico da
[ADR-003](ADR-003-tipos-e-encoding.md), incluindo little-endian, tags de tipo,
`FieldId`, comprimentos e validação de limites. O envelope de rede é próprio e
versionado; não reutiliza `ObjectHeader`, headers de página ou estruturas C++ em
memória.

Cada objeto transferido usa um `ObjectEnvelope`:

```text
| object_id u64 | type_definition_id u64 | payload_length u32 | payload |
```

O `payload` é o mesmo payload lógico interpretável pelo codec genérico. O
`BaselineId` corrente é negociado em `HelloOk`; quando uma mensagem precisar
de outra baseline, ela deve declará-la no envelope da própria mensagem.

Para streaming, o tipo físico `ObjectFrame` usa um diretório inspirado em uma
slotted page. Seu layout completo, incluindo compressão opcional, é definido
abaixo. Slots permanecem em ordem de produção; intervalos sobrepostos, fora da
área de dados, truncados ou com envelopes inválidos encerram a mensagem com
erro de protocolo.

`ObjectFrame` é coalescência física, não lote lógico. O primeiro resultado é
elegível para envio imediatamente; o servidor não espera atingir quantidade ou
tamanho mínimo. Ele pode incluir outros resultados que já estejam disponíveis,
até o limite negociado do frame. Um frame com um único slot é sempre válido.
Backpressure limita a produção antes da construção do frame seguinte.

### Compressão por frame

Compressão é uma propriedade opcional e independente de cada `ObjectFrame`,
negociada no `Hello`/`HelloOk`. Ela não altera o codec lógico do objeto nem o
formato persistente. O codec `none` é obrigatório; outros codecs recebem ids
versionados. A escolha inicial entre LZ4, Zstandard ou outra implementação será
feita por benchmark, sem fixar uma dependência nesta ADR.

```text
| query_id u32 | record_count u32 | compression u8 | reservado[3] |
| uncompressed_size u32 | encoded_size u32                         |
| slot[record_count]                                             |
| encoded_data[encoded_size]                                     |
```

Os offsets e comprimentos dos slots referem-se à área de dados
**descomprimida**. O diretório permanece fora da compressão para que seus
limites possam ser validados antes da alocação e da descompressão. Quando
`compression=none`, `encoded_size == uncompressed_size` e `encoded_data` é a
área de dados original.

O servidor não espera formar um frame maior para obter melhor taxa de
compressão. Ele só tenta comprimir frames que já seriam enviados e que excedam
um limiar configurável. Se a saída não for materialmente menor, envia `none`.
Blobs ou payloads reconhecidamente já comprimidos podem pular a tentativa.

Cada frame é comprimido independentemente, sem dicionário ou estado herdado de
frames anteriores na primeira implementação. Isso preserva streaming,
cancelamento, recuperação após erro e consumo de memória limitado.

Antes de alocar ou descomprimir, o receptor valida `record_count`,
`encoded_size`, `uncompressed_size`, a razão máxima de expansão e os limites
negociados da conexão. Saída menor ou maior que `uncompressed_size`, dados
comprimidos truncados e codec não negociado produzem `protocol_error`. A
compressão ocorre antes de eventual criptografia do transporte.

O protocolo nunca expõe nem aceita como identidade externa:

- `PageId`;
- `SlotId`;
- `RecordId` ou `generation` física;
- offsets no arquivo;
- headers de página, ponteiros do Buffer Pool ou registros do WAL.

Chamadas de método (`OpCall`/`OpResult`) reutilizam o mesmo codec de valores e
envelopes, mas não precisam usar `ObjectFrame` quando carregam um único payload.
Argumentos e resultados continuam independentes de ABI, padding, ponteiros ou
endianness nativa.

O transporte inicial usa sockets nativos encapsulados por `NativeSocket`, no
mesmo padrão portável de `NativeFile`. O frame externo permanece:

```text
| length u32 | type u8 | payload |
```

`length` cobre `type + payload` e é validado antes de qualquer alocação. O
limite inicial é 16 MiB e pode ser reduzido durante a negociação.

## Consequências

- Storage e rede compartilham o codec lógico e parte do caminho de validação.
- O servidor pode montar frames com buffers contíguos e usar scatter/gather I/O
  sem materializar uma coleção lógica de resultados.
- O cliente obtém acesso O(1) ao registro N de um frame pelo diretório de slots.
- O primeiro resultado não espera formação de lote, preservando TTFR e
  backpressure.
- Compressão pode reduzir bytes transferidos sem mudar a representação lógica;
  frames pequenos ou incompressíveis continuam no modo `none`.
- A negociação e os limites de expansão impedem que compressão se torne uma
  fonte de alocações sem limite.
- Compactação, MVCC, realocação de registros e mudanças no formato de página não
  alteram identidades nem o contrato de rede.
- Não é permitido enviar uma página do banco diretamente, mesmo quando seu
  layout atual pareça compatível com `ObjectFrame`.
- Formato de arquivo e protocolo continuam versionados separadamente.
