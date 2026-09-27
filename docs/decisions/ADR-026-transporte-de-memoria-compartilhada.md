# ADR-026 — Clientes em outras linguagens: protocolo nativo publicado e transporte por memória compartilhada

- Estado: aceito
- Data: 2026-09-27
- Relacionados: ADR-010 (protocolo binário), ADR-025 (servidor com procs compiladas), PLANO_SERVIDOR_PROCS S10

## Contexto

O plano previa um gateway HTTP/JSON para clientes em outras linguagens (S10). O
pedido mudou: um protocolo mais performático, na linha do RDMA — a mensagem vai
direto para uma memória que o outro lado lê, sem syscall nem cópia por mensagem,
e os dois lados fazem polling em vez de esperar o kernel acordá-los.

RDMA de verdade (InfiniBand, RoCE, iWARP) exige placa com suporte e não existe
nas máquinas que temos (desktop Windows, droplets da DigitalOcean). Soft-RoCE
(`rdma_rxe`) roda sem placa, mas só serve para testar a funcionalidade, não
para ganhar desempenho.

Onde está o custo de uma chamada de proc hoje, pelo código (sem medição):

1. **Travessias do kernel:** um `send` e um `recv` em cada ponta por chamada.
2. **Trocas de thread:** no cliente, `Client::call` envia e espera numa
   `condition_variable` até a thread leitora (`ClientConn::reader_loop`)
   entregar a resposta; no servidor, a thread leitora da sessão põe a mensagem
   numa fila e acorda a thread da sessão (`wait_inbound`). São quatro
   acordares de thread por chamada, cada um na casa de microssegundos.
3. **Codificação:** o frame (`length u32 | type u8 | payload`) e o `Value` v1
   já são binários, compactos e sem esquema; decodificar é ler tags e
   comprimentos. Não é onde o tempo vai.

Alternativas avaliadas:

| Opção | O que dá | Por que não (ou não agora) |
|---|---|---|
| Gateway HTTP/JSON | qualquer linguagem, `curl` | é o mais lento: HTTP/1.1, JSON, um salto a mais. Já existe como padrão na `biblioteca-web` |
| gRPC (HTTP/2 + protobuf) | geração de cliente em muitas linguagens | as procs não têm esquema de argumentos (`Value` é autodescrito); usar `google.protobuf.Struct` perde a vantagem do protobuf. Dependência grande (gRPC, abseil, protoc) e HTTP/2 por baixo: não é mais rápido que o protocolo nativo |
| Cap'n Proto RPC | leitura sem cópia, *promise pipelining* | exige esquema; suporte fraco fora de C++/Rust/Go; o ganho sem cópia já é possível no nosso formato |
| MessagePack-RPC | bibliotecas otimizadas em todas as linguagens | mesma ordem de custo do `Value` v1; não resolve (1) nem (2) |
| Arrow Flight | colunar, ótimo para consultas grandes | é sobre gRPC; serve para *streams* de consulta, não para chamadas de proc curtas |
| RDMA (verbs) | sem kernel e sem CPU remota no caminho | falta hardware; fica como transporte futuro sobre o mesmo anel (abaixo) |

## Decisão

1. **O protocolo nativo (ADR-010) é o protocolo dos clientes em outras
   linguagens.** Fica especificado byte a byte em
   [`docs/PROTOCOLO_CLIENTES.md`](../PROTOCOLO_CLIENTES.md) (frame, `Hello`,
   `OpCall`/`OpResult`, `Value` v1), com um cliente de referência em Python
   (`clients/python/modb_client.py`, só biblioteca padrão). Um cliente novo é
   ~200 linhas em qualquer linguagem. Nada de gateway intermediário.

2. **Transporte por memória compartilhada ("anel") para clientes na mesma
   máquina** — o equivalente local do RDMA:
   - O cliente conecta por TCP como sempre e pede `ShmAttach`. O servidor cria
     uma região compartilhada (no Windows, um mapeamento nomeado; no Linux, um
     arquivo em `/dev/shm`) e responde `ShmAttachOk` com o nome e o tamanho.
   - A região tem dois anéis SPSC (pedidos cliente→servidor, respostas
     servidor→cliente). Cada mensagem no anel é **exatamente o frame do TCP**:
     os mesmos codificadores, os mesmos limites. Só `OpCall`/`OpResult` passam
     pelo anel; consultas continuam no TCP.
   - O cliente escreve o pedido e faz polling da resposta **na própria thread**;
     o servidor tem uma thread por anel fazendo polling dos pedidos. Nenhuma
     syscall e nenhuma troca de thread no caminho quente — (1) e (2) somem.
   - Espera adaptativa nos dois lados: gira (~100 µs), depois cede a CPU
     (`yield`) até 5 ms, depois dorme em passos crescentes até 1 ms. Com
     chamadas em sequência o anel não dorme; ocioso, dorme depois de 5 ms e
     custa quase nada de CPU; a primeira chamada depois de parado paga até
     ~1 ms. (Uma janela de `yield` de 200 µs foi medida no desktop e derrubava
     payloads de 64 KiB de ~29 mil para ~3 mil chamadas/s: o intervalo entre
     chamadas passava da janela e cada uma pagava o acordar do sono.)
   - A conexão TCP continua aberta como **linha de vida**: fechou (cliente
     morreu, servidor parou), a thread do anel termina e a região é liberada.
   - Mensagens são contíguas no anel (sem quebra na volta): quem lê decodifica
     no lugar. Posições são contadores u64 que só crescem; publicar é gravar a
     posição nova (release) depois dos bytes; ler é o contrário (acquire).

3. **RDMA de verdade fica como transporte futuro sobre o mesmo anel.** O layout
   já é o de um *ring buffer* registrado (FaRM/HERD): o cliente faria RDMA WRITE
   do pedido no anel do servidor e da posição nova; o servidor, o mesmo no anel
   de respostas do cliente. Condição de reabertura: uma máquina com placa RDMA
   (bare metal com ConnectX, AWS EFA, Azure HB/HC) para medir. Teste funcional
   possível antes com Soft-RoCE num Linux.

## Consequências

- Chamada de proc local sem kernel e sem troca de thread; a latência passa a
  ser dominada pela proc e pelo `engine_mutex_` (que é a S9).
- O custo é CPU em polling enquanto houver chamadas; a espera adaptativa
  limita isso quando o anel fica ocioso.
- Qualquer linguagem que mapeie memória (Python `mmap`, Java
  `MappedByteBuffer`, Go, Rust, C#) usa o anel. A leitura e a escrita das
  posições precisam de semântica acquire/release; em x86-64 um load/store de
  8 bytes alinhado já tem, e é o que o cliente Python faz (em ARM, use as
  atômicas da linguagem).
- Sem segurança própria, como o resto do servidor (removida do plano a pedido):
  a região tem as permissões padrão do usuário do servidor.
- Os números (TCP × anel) só valem de máquina dedicada; o `modb_rpc_bench`
  mede os dois com a mesma proc.
