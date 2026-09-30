#!/usr/bin/env bash
# Gera o SDK binário do Ring0 para Linux x86_64: bibliotecas, headers, pacote
# CMake (find_package(moDb)), `modb` e `modb-proxy`. Saída:
#   dist/modb-sdk-<versão>-linux-x86_64.tar.gz e o .sha256 ao lado.
#
# Uso (da raiz do repositório, num Linux ou no WSL):
#   scripts/package-sdk.sh [diretório-de-build]
# CMake ≥ 3.30 no PATH (docs/CONCORRENCIA_TSAN.md mostra como instalar no WSL).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$HOME/modb-build/sdk}"
version="$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -1)"
name="modb-sdk-${version}-linux-x86_64"
stage="$build/stage/$name"

cmake -S "$root" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMODB_BUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF \
    -DMODB_BUILD_TRAINING=OFF
cmake --build "$build" --target modb modb_app_client modb_server_host modb_proxy modb_cli modb_proxy_cli
rm -rf "$stage"
cmake --install "$build" --prefix "$stage" --strip

# O exemplo de aplicação e o guia vão junto, para compilar sem o repositório.
mkdir -p "$stage/share/modb/examples"
cp -r "$root/examples/sdk_app" "$root/examples/server_procs" "$stage/share/modb/examples/"
cp "$root/docs/SDK.md" "$stage/share/modb/"

mkdir -p "$root/dist"
tar -C "$build/stage" -czf "$root/dist/$name.tar.gz" "$name"
(cd "$root/dist" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
echo "$root/dist/$name.tar.gz"
