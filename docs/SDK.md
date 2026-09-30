# SDK binário do Ring0 (Linux x86_64)

Com o SDK você usa o Ring0 **sem compilar o motor**. Ele traz:

- o `modb-proxy` e o CLI `modb`, prontos para rodar;
- as bibliotecas estáticas, os headers e o pacote CMake (`find_package(moDb)`)
  para compilar **só o módulo de procs** da sua aplicação.

As regras de negócio de uma aplicação são stored procedures em C++ compiladas
dentro do servidor dela (ADR-025). Por isso o servidor da aplicação é sempre
compilado, mas só com o código da aplicação: o motor vem pronto no SDK.

## Versões

Cada release é uma tag do repositório, com o SDK anexado:

| Tag | Protocolo | Conteúdo |
|---|---|---|
| [`v0.1.1`](https://github.com/sergioleal/moDb/releases/tag/v0.1.1) | major 1, minor 2 (proxy e autenticação por token) | SDK Linux x86_64 |
| `v0.1.0` | major 1, minor 2 | só o código-fonte, sem SDK anexado (use a `v0.1.1`) |

Os links para o código nesta documentação apontam para a tag, e não para
`master`: o que está na tag é o que foi compilado no SDK.

## Requisitos

| Para | Precisa de |
|---|---|
| Rodar `modb`, `modb-proxy` e o servidor gerado | Linux x86_64 com glibc ≥ 2.34 e libstdc++ do GCC ≥ 13 (Ubuntu 24.04, Debian 13 ou mais novos) |
| Compilar o módulo de procs | g++ ≥ 13 (C++23), CMake ≥ 3.30, Ninja |

O Ubuntu 24.04 traz o CMake 3.28. Para instalar um mais novo sem mexer no
sistema: `python3 -m venv ~/.venvs/cmake && ~/.venvs/cmake/bin/pip install "cmake>=3.30"`.

## 1. Baixar

```bash
curl -LO https://github.com/sergioleal/moDb/releases/download/v0.1.1/modb-sdk-0.1.1-linux-x86_64.tar.gz
curl -LO https://github.com/sergioleal/moDb/releases/download/v0.1.1/modb-sdk-0.1.1-linux-x86_64.tar.gz.sha256
sha256sum -c modb-sdk-0.1.1-linux-x86_64.tar.gz.sha256
sudo tar -C /opt -xzf modb-sdk-0.1.1-linux-x86_64.tar.gz
```

Conteúdo, em `/opt/modb-sdk-0.1.1-linux-x86_64`:

```text
bin/modb, bin/modb-proxy
include/modb/...                 headers (contrato em docs/API_PUBLICA.md)
lib/libmodb*.a                   motor, app_client, server_host, proxy
lib/cmake/moDb/                  find_package(moDb) e modb_add_server
share/modb/examples/sdk_app/     projeto de exemplo que usa só o SDK
share/modb/examples/server_procs/ módulo de procs de exemplo (notas)
share/modb/SDK.md                este guia
```

## 2. Compilar o servidor da aplicação

O exemplo [`examples/sdk_app/CMakeLists.txt`](https://github.com/sergioleal/moDb/blob/v0.1.1/examples/sdk_app/CMakeLists.txt)
é o modelo:

```cmake
cmake_minimum_required(VERSION 3.30)
project(minha_app LANGUAGES CXX)

find_package(moDb 0.1 CONFIG REQUIRED)

add_library(minha_app_procs STATIC procs.cpp)
target_link_libraries(minha_app_procs PUBLIC modb::server_host)

modb_add_server(minha-app-server MODULES minha_app_procs)
```

O módulo define `modb::server::Module modb_module_minha_app_procs()`: o nome do
alvo da biblioteca, com `-` e `.` trocados por `_`. Como escrever as procs:
[`include/modb/server/module.hpp`](https://github.com/sergioleal/moDb/blob/v0.1.1/include/modb/server/module.hpp)
e o exemplo [`examples/server_procs/notas_procs.cpp`](https://github.com/sergioleal/moDb/blob/v0.1.1/examples/server_procs/notas_procs.cpp).

Configuração da aplicação (prefixos, códigos, limites) não vai em variável de
ambiente: o módulo declara com `.setting(nome, padrão, descrição, validador)`, o
servidor lê `<módulo>.<nome>` do `.conf` ou de `--<módulo>.<nome>`, valida na
subida, e a proc lê com `c.setting(nome)`
([`OPERACAO.md`](https://github.com/sergioleal/moDb/blob/v0.1.1/docs/OPERACAO.md), "Configurações dos módulos").

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=/opt/modb-sdk-0.1.1-linux-x86_64
cmake --build build
```

Para testar o SDK antes de escrever o seu módulo, compile o exemplo que vem
dentro dele:

```bash
cp -r /opt/modb-sdk-0.1.1-linux-x86_64/share/modb/examples ~/modb-exemplo
cmake -S ~/modb-exemplo/sdk_app -B ~/modb-exemplo/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/modb-sdk-0.1.1-linux-x86_64
cmake --build ~/modb-exemplo/build      # gera ~/modb-exemplo/build/notas-server
```

## 3. Rodar: engine local + proxy

O engine escuta só num socket local; os clientes falam com o `modb-proxy`
(ADR-028). O segredo do link é o mesmo arquivo nos dois lados.

```bash
SDK=/opt/modb-sdk-0.1.1-linux-x86_64
openssl rand -hex 32 > link.secret
chmod 600 link.secret

# um token por cliente; o arquivo guarda só o SHA-256
TOKEN=$(openssl rand -hex 24)
$SDK/bin/modb-proxy hash-token "$TOKEN" web leitor,escritor >> proxy.tokens

./build/minha-app-server --db app.modb --local "$PWD/engine.sock" --secret-file link.secret &
$SDK/bin/modb-proxy --engine "$PWD/engine.sock" --secret-file link.secret \
                    --tokens proxy.tokens --port 7474 &

$SDK/bin/modb ping 127.0.0.1 7474 ""     # sonda: responde enquanto o proxy tem link
```

Cada processo imprime `READY <porta>` quando está pronto. O engine imprime
`READY 0` porque não tem TCP. Configuração por arquivo, systemd, políticas
(`read_only`, allowlist, limites) e auditoria:
[`docs/OPERACAO.md`](https://github.com/sergioleal/moDb/blob/v0.1.1/docs/OPERACAO.md)
e [`examples/server_procs/deploy/`](https://github.com/sergioleal/moDb/tree/v0.1.1/examples/server_procs/deploy).

## 4. Clientes

O cliente conecta no proxy (`127.0.0.1:7474` acima) e se autentica com o
token. O protocolo está em
[`docs/PROTOCOLO_CLIENTES.md`](https://github.com/sergioleal/moDb/blob/v0.1.1/docs/PROTOCOLO_CLIENTES.md);
a implementação de referência em Python é
[`clients/python/modb_client.py`](https://github.com/sergioleal/moDb/blob/v0.1.1/clients/python/modb_client.py):

```python
from modb_client import Client
with Client("127.0.0.1", 7474, token=TOKEN) as c:
    c.call("notas.criar", {"texto": "primeira"})
    print(c.call("notas.listar", {}))
```

Um cliente C++ usa `modb::app_client` do próprio SDK
(`ConnectionOptions::token`).

## O que não vem no SDK

- **Windows e macOS:** só Linux x86_64 nesta versão.
- **TLS no proxy:** ainda não existe, e o token viaja em claro. Deixe cliente
  e proxy na mesma máquina ou numa rede confiável, ou ponha um terminador TLS
  na frente.
- **Testes, benchmarks e exemplos por fase:** ficam no repositório.

## Gerar o SDK (mantenedores)

A release sai sozinha: um push de tag `v<versão>` dispara o workflow
[`release-sdk`](https://github.com/sergioleal/moDb/blob/v0.1.1/.github/workflows/release-sdk.yml).
Ele confere se a tag bate com o `project(VERSION)` do `CMakeLists.txt`, gera o
SDK no Ubuntu 24.04, testa o tarball como quem o usa
([`scripts/sdk-smoke.sh`](https://github.com/sergioleal/moDb/blob/v0.1.1/scripts/sdk-smoke.sh))
e publica a release com o `.tar.gz` e o `.sha256`.

```bash
# nova versão: suba o VERSION no CMakeLists.txt, commit, e então
git tag -a v0.2.0 -m "Ring0 v0.2.0" && git push origin v0.2.0
```

Localmente, num Linux ou no WSL com CMake ≥ 3.30 no PATH:

```bash
scripts/package-sdk.sh                                        # gera dist/modb-sdk-<versão>-linux-x86_64.tar.gz e o .sha256
scripts/sdk-smoke.sh dist/modb-sdk-0.1.1-linux-x86_64.tar.gz  # o mesmo teste da release
```
