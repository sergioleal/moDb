#!/usr/bin/env bash
#
# Executa `modb_load run` localmente, lendo os seletores de um arquivo YAML.
#
# Lê um subconjunto restrito de YAML (chave: valor escalar, ou chave: seguida
# de "  - item" por linha para listas — sem aspas, sem lista em uma linha, sem
# aninhamento além de um nível). O formato completo está documentado no
# cabeçalho de loadtests/config/load-local.yaml.
#
# `modb_load` ainda não existe (docs/PLANO_TESTES_DE_CARGA.md, Subfases A/B).
# Use --dry-run para ver o comando resolvido sem exigir que o binário esteja
# construído — o script continua útil como validador da configuração.
#
# Uso:
#   ./scripts/run-load.sh [--config PATH] [--binary PATH] [--environments-file PATH]
#                          [--environment ID[,ID...]] [--dry-run]
#
# Exemplos:
#   ./scripts/run-load.sh
#   ./scripts/run-load.sh --config loadtests/config/load-standard.yaml --dry-run
#   ./scripts/run-load.sh --environment linux-remoto

set -euo pipefail

if ((BASH_VERSINFO[0] < 4)); then
    echo "Este script exige bash >= 4 (associative arrays). Versão atual: $BASH_VERSION" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_PATH="$ROOT/loadtests/config/load-local.yaml"
BINARY=""
ENVIRONMENTS_FILE="$ROOT/loadtests/environments.json"
ENV_OVERRIDE=""
DRY_RUN=0

usage() {
    echo "Uso: $0 [--config PATH] [--binary PATH] [--environments-file PATH] [--environment ID[,ID...]] [--dry-run]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config) CONFIG_PATH="$2"; shift 2 ;;
        --binary) BINARY="$2"; shift 2 ;;
        --environments-file) ENVIRONMENTS_FILE="$2"; shift 2 ;;
        --environment) ENV_OVERRIDE="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Argumento desconhecido: $1" >&2; usage; exit 2 ;;
    esac
done

if [[ ! -f "$CONFIG_PATH" ]]; then
    echo "Arquivo de configuração não encontrado: $CONFIG_PATH" >&2
    exit 2
fi

trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

declare -A CONFIG
declare -A CONFIG_LIST
CURRENT_LIST_KEY=""
LINE_NO=0

while IFS= read -r raw_line || [[ -n "$raw_line" ]]; do
    LINE_NO=$((LINE_NO + 1))
    line="${raw_line%$'\r'}"
    trimmed="$(trim "$line")"

    if [[ -z "$trimmed" ]]; then continue; fi
    if [[ "$trimmed" == \#* ]]; then continue; fi

    if [[ "$line" =~ ^[[:space:]]+-[[:space:]]*(.*)$ ]]; then
        item="$(trim "${BASH_REMATCH[1]}")"
        if [[ -z "$CURRENT_LIST_KEY" ]]; then
            echo "$CONFIG_PATH:$LINE_NO: item de lista sem chave anterior: $raw_line" >&2
            exit 2
        fi
        if [[ -n "${CONFIG[$CURRENT_LIST_KEY]:-}" ]]; then
            echo "$CONFIG_PATH:$LINE_NO: '$CURRENT_LIST_KEY' já tem valor escalar; não pode virar lista." >&2
            exit 2
        fi
        if [[ -z "${CONFIG_LIST[$CURRENT_LIST_KEY]:-}" ]]; then
            CONFIG_LIST["$CURRENT_LIST_KEY"]="$item"
        else
            CONFIG_LIST["$CURRENT_LIST_KEY"]+=",$item"
        fi
        continue
    fi

    if [[ "$trimmed" =~ ^([A-Za-z0-9_]+):[[:space:]]*(.*)$ ]]; then
        key="${BASH_REMATCH[1]}"
        value="$(trim "${BASH_REMATCH[2]}")"
        if [[ -z "$value" ]]; then
            CURRENT_LIST_KEY="$key"
        else
            CONFIG["$key"]="$value"
            CURRENT_LIST_KEY=""
        fi
        continue
    fi

    echo "$CONFIG_PATH:$LINE_NO: linha fora do subconjunto restrito de YAML: $raw_line" >&2
    exit 2
done < "$CONFIG_PATH"

if [[ -n "$ENV_OVERRIDE" ]]; then
    CONFIG_LIST[environment]="$ENV_OVERRIDE"
    unset 'CONFIG[environment]' 2>/dev/null || true
fi

# Valida ambientes citados contra o catálogo (§4.4) antes de montar o comando.
# Best-effort: sem `jq`, avisa e segue — não é dependência obrigatória.
if [[ -f "$ENVIRONMENTS_FILE" && -n "${CONFIG_LIST[environment]:-}" ]]; then
    if command -v jq >/dev/null 2>&1; then
        IFS=',' read -ra ENV_IDS <<< "${CONFIG_LIST[environment]}"
        for env_id in "${ENV_IDS[@]}"; do
            kind="$(jq -r --arg id "$env_id" '.environments[] | select(.id == $id) | .kind' "$ENVIRONMENTS_FILE")"
            if [[ -z "$kind" ]]; then
                known="$(jq -r '[.environments[].id] | join(", ")' "$ENVIRONMENTS_FILE")"
                echo "Ambiente '$env_id' (config $CONFIG_PATH) não está cadastrado em $ENVIRONMENTS_FILE. Conhecidos: $known" >&2
                exit 2
            fi
            if [[ "$kind" == "ssh" ]]; then
                echo "Aviso: ambiente '$env_id' é kind=ssh -- run-load.sh executa localmente. Use scripts/run-remote-benchmark.ps1 (ou o futuro run-remote-load) para executar nele." >&2
            fi
        done
    else
        echo "Aviso: 'jq' não encontrado -- pulando validação de $ENVIRONMENTS_FILE." >&2
    fi
elif [[ ! -f "$ENVIRONMENTS_FILE" ]]; then
    echo "Aviso: registro de ambientes não encontrado em $ENVIRONMENTS_FILE -- pulando validação." >&2
fi

ARGS=(run)

add_scalar() {
    local flag="$1" key="$2" value="${CONFIG[$2]:-}"
    # `[[ ]] && cmd` como último comando de uma função devolve o status do
    # teste quando ele é falso, e essa chamada de função é abortada por
    # `set -e` -- por isso if/fi explícito, nunca `&&` aqui.
    if [[ -n "$value" ]]; then
        ARGS+=("$flag" "$value")
    fi
}

add_list() {
    local flag="$1" key="$2" value="${CONFIG_LIST[$2]:-}"
    if [[ -n "$value" ]]; then
        ARGS+=("$flag" "$value")
    fi
}

add_scalar --profile profile
add_scalar --seed seed
add_scalar --output-dir output_dir
add_scalar --work-dir work_dir
add_scalar --filter filter
add_scalar --exclude exclude
add_scalar --repeat repeat
add_scalar --max-duration max_duration
add_scalar --max-disk-gb max_disk_gb
add_scalar --max-rss-mb max_rss_mb
add_list --scale scale
add_list --workload workload
add_list --target target
add_list --environment environment
add_list --concurrency concurrency
add_list --payload payload
add_list --case case

if [[ "${CONFIG[accept_unknown_budget]:-false}" == "true" ]]; then
    ARGS+=(--accept-unknown-budget)
fi
if [[ "${CONFIG[dry_run]:-false}" == "true" ]]; then
    ARGS+=(--dry-run)
fi

echo "Configuração: $CONFIG_PATH"
echo "Comando: modb_load ${ARGS[*]}"

if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "--dry-run (script): nada foi executado."
    exit 0
fi

if [[ -z "$BINARY" ]]; then
    for candidate in "$ROOT/build/debug/modb_load" "$ROOT/build/release/modb_load" "$ROOT/build-linux/modb_load"; do
        if [[ -x "$candidate" ]]; then
            BINARY="$candidate"
            break
        fi
    done
fi

if [[ -z "$BINARY" || ! -x "$BINARY" ]]; then
    echo "modb_load não encontrado. Ele ainda não está implementado -- ver docs/PLANO_TESTES_DE_CARGA.md, Subfases A/B." >&2
    echo "Use --dry-run para só ver o comando que seria executado." >&2
    exit 1
fi

"$BINARY" "${ARGS[@]}"
