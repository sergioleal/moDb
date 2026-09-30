#!/usr/bin/env bash
# Teste do SDK binário como quem o usa: extrai o tarball num diretório limpo,
# compila só o módulo de exemplo (examples/sdk_app) contra ele e sobe o engine
# em --local atrás do modb-proxy com token. Confere a recusa sem token, uma
# escrita e uma leitura pelo cliente Python e a sonda `modb ping`.
#
# Uso: scripts/sdk-smoke.sh dist/modb-sdk-<versão>-linux-x86_64.tar.gz
# Precisa de CMake ≥ 3.30, Ninja, g++ ≥ 13 e python3 no PATH.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
tarball="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
work="$(mktemp -d)"
server_pid=""
proxy_pid=""
cleanup() {
    [ -n "$proxy_pid" ] && kill "$proxy_pid" 2>/dev/null || true
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

wait_ready() {
    for _ in $(seq 100); do
        grep -q '^READY' "$1" && return 0
        sleep 0.1
    done
    echo "sem READY em $1:" >&2
    cat "$1" >&2
    return 1
}

cd "$work"
tar -xzf "$tarball"
sdk="$work/$(basename "$tarball" .tar.gz)"

cp -r "$sdk/share/modb/examples" app
cmake -S app/sdk_app -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$sdk"
cmake --build build

head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > link.secret
token="$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')"
"$sdk/bin/modb-proxy" hash-token "$token" web leitor,escritor > proxy.tokens

./build/notas-server --db notas.modb --local "$work/engine.sock" --secret-file link.secret > server.log 2>&1 &
server_pid=$!
wait_ready server.log
port=17474
"$sdk/bin/modb-proxy" --engine "$work/engine.sock" --secret-file link.secret \
    --tokens proxy.tokens --port "$port" > proxy.log 2>&1 &
proxy_pid=$!
wait_ready proxy.log

python3 - "$root/clients/python" "$port" "$token" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
from modb_client import Client, ModbError

port, token = int(sys.argv[2]), sys.argv[3]
try:
    Client("127.0.0.1", port).call("notas.listar", {})
    sys.exit("sem token: o proxy aceitou")
except ModbError as e:
    assert e.code == 73, e
with Client("127.0.0.1", port, token=token) as c:
    created = c.call("notas.criar", {"texto": "via sdk"})
    listed = c.call("notas.listar", {})
    assert [n["texto"] for n in listed] == ["via sdk"], listed
    assert listed[0]["id"] == created["id"], (created, listed)
print("cliente: recusa sem token, escrita e leitura ok")
EOF

"$sdk/bin/modb" ping 127.0.0.1 "$port" "" > /dev/null
kill "$proxy_pid"
wait "$proxy_pid" 2>/dev/null || true
proxy_pid=""
if "$sdk/bin/modb" ping 127.0.0.1 "$port" "" > /dev/null 2>&1; then
    echo "ping respondeu sem o proxy" >&2
    exit 1
fi
echo "SDK SMOKE OK"
