#!/usr/bin/env bash
#
# Runner Linux do pacote de performance do moDb (loadtests/perfpack/README.md).
#
# Faz exatamente o mesmo que run.ps1 no Windows: lê uma suíte, roda cada caso
# num processo novo do modb_load (um work dir limpo por execução, ordem dos
# casos alternada entre repetições) e deixa tudo num diretório de resultado
# autocontido, pronto para `scripts/perfpack.py fetch` trazer e indexar na
# série histórica:
#
#   <results>/<AAAAMMDDTHHMMSSZ>-<ambiente>-<commit12>/
#       raw/*.jsonl      um arquivo do modb_load por execução
#       logs/*.log       saída de cada execução
#       executions.tsv   repetição, caso, código de saída, status, arquivo
#       manifest.json    pacote, commit, suíte, sha256 de cada arquivo
#       DONE             escrito por último: o resultado está completo
#
# Uso:
#   ./run.sh --environment ID [--suite standard|smoke|ARQUIVO] [--repeat N]
#            [--only SUBSTR] [--results-dir DIR] [--environments-file ARQ]
#            [--build] [--dry-run]
#
# Só depende de bash >= 4, coreutils e tar. Se o binário pronto não rodar
# nesta máquina (glibc mais antiga, por exemplo) ou com --build, compila o
# modb_load do fonte do MESMO commit que vem no pacote (exige cmake, ninja e
# g++ >= 13).

set -euo pipefail

PACK="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENVIRONMENT=""
SUITE="standard"
REPEAT_OVERRIDE=""
ONLY=""
RESULTS_DIR="$PACK/../results"
ENVIRONMENTS_FILE="$PACK/environments.json"
FORCE_BUILD=0
DRY_RUN=0

usage() {
    sed -n '/^# Uso:/,/^# Só depende/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' | head -n -1 >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --environment) ENVIRONMENT="$2"; shift 2 ;;
        --suite) SUITE="$2"; shift 2 ;;
        --repeat) REPEAT_OVERRIDE="$2"; shift 2 ;;
        --only) ONLY="$2"; shift 2 ;;
        --results-dir) RESULTS_DIR="$2"; shift 2 ;;
        --environments-file) ENVIRONMENTS_FILE="$2"; shift 2 ;;
        --build) FORCE_BUILD=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "argumento desconhecido: $1" >&2; usage; exit 2 ;;
    esac
done

die() { echo "perfpack: $*" >&2; exit 2; }

[[ -n "$ENVIRONMENT" ]] || die "informe --environment ID (um id de loadtests/environments.json)"
[[ "$ENVIRONMENT" =~ ^[A-Za-z0-9._-]+$ ]] || die "id de ambiente inválido: $ENVIRONMENT"
[[ -f "$ENVIRONMENTS_FILE" ]] || die "catálogo de ambientes não encontrado: $ENVIRONMENTS_FILE"
# O modb_load recusaria caso a caso; melhor recusar antes de começar.
grep -Eq "\"id\"[[:space:]]*:[[:space:]]*\"$ENVIRONMENT\"" "$ENVIRONMENTS_FILE" \
    || die "ambiente '$ENVIRONMENT' não está cadastrado em $ENVIRONMENTS_FILE"
if [[ -n "$REPEAT_OVERRIDE" && ! "$REPEAT_OVERRIDE" =~ ^[1-9][0-9]*$ ]]; then
    die "--repeat precisa ser um inteiro >= 1"
fi

# --- identidade do pacote -----------------------------------------------------
# PACKAGE.env: KEY=VALUE por linha, escrito por scripts/perfpack.py build.
declare -A PKG
while IFS='=' read -r key value || [[ -n "$key" ]]; do
    key="${key%$'\r'}"; value="${value%$'\r'}"
    [[ -z "$key" || "$key" == \#* ]] && continue
    PKG["$key"]="$value"
done < "$PACK/PACKAGE.env"
for required in PACKAGE_ID MODB_GIT_COMMIT MODB_GIT_BRANCH MODB_GIT_DIRTY MODB_PERFPACK_CMAKE_ARGS; do
    [[ -n "${PKG[$required]:-}" ]] || die "PACKAGE.env sem $required"
done
# O modb_load grava o commit a partir destas variáveis (fora de um repositório
# git não teria como saber com que fonte foi compilado).
export MODB_GIT_COMMIT="${PKG[MODB_GIT_COMMIT]}"
export MODB_GIT_BRANCH="${PKG[MODB_GIT_BRANCH]}"
export MODB_GIT_DIRTY="${PKG[MODB_GIT_DIRTY]}"

# --- suíte --------------------------------------------------------------------
case "$SUITE" in
    */*|*.txt) SUITE_FILE="$SUITE" ;;
    *) SUITE_FILE="$PACK/suites/$SUITE.txt" ;;
esac
[[ -f "$SUITE_FILE" ]] || die "suíte não encontrada: $SUITE_FILE"
SUITE_NAME="$(basename "$SUITE_FILE" .txt)"

REPEAT=1
SEED=123456
CASES=()
while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%$'\r'}"
    line="${line%%#*}"
    read -r -a words <<< "$line" || true
    [[ ${#words[@]} -eq 0 ]] && continue
    case "${words[0]}" in
        repeat) REPEAT="${words[1]:?}" ;;
        seed) SEED="${words[1]:?}" ;;
        case)
            [[ ${#words[@]} -ge 2 ]] || die "$SUITE_FILE: 'case' sem id"
            spec="${words[*]:1}"
            if [[ -z "$ONLY" || "${words[1]}" == *"$ONLY"* ]]; then
                CASES+=("$spec")
            fi
            ;;
        *) die "$SUITE_FILE: linha não reconhecida: $line" ;;
    esac
done < "$SUITE_FILE"
[[ -n "$REPEAT_OVERRIDE" ]] && REPEAT="$REPEAT_OVERRIDE"
[[ ${#CASES[@]} -gt 0 ]] || die "nenhum caso selecionado (suíte $SUITE_FILE, --only '$ONLY')"

# --- binário ------------------------------------------------------------------
BIN="$PACK/bin/linux-x86_64/modb_load"
BINARY_ORIGIN="prebuilt"

build_from_source() {
    for tool in cmake ninja g++ tar; do
        command -v "$tool" >/dev/null 2>&1 || die "para compilar do fonte é preciso '$tool' no PATH"
    done
    local src="$PACK/build/src" out="$PACK/build/out"
    echo "perfpack: compilando modb_load do fonte (${PKG[MODB_GIT_COMMIT]:0:12})..." >&2
    rm -rf "$src" "$out"
    mkdir -p "$src"
    tar -xzf "$PACK/src/modb-src.tar.gz" -C "$src"
    # shellcheck disable=SC2086 # os argumentos do cmake vêm separados por espaço
    cmake -S "$src" -B "$out" -G Ninja ${PKG[MODB_PERFPACK_CMAKE_ARGS]} >&2
    cmake --build "$out" --target modb_load >&2
    BIN="$out/modb_load"
    BINARY_ORIGIN="built-on-target"
}

if [[ "$FORCE_BUILD" -eq 1 ]]; then
    build_from_source
elif [[ ! -x "$BIN" ]] || ! "$BIN" list-profiles >/dev/null 2>&1; then
    echo "perfpack: o binário pronto não roda nesta máquina; compilando do fonte." >&2
    build_from_source
fi

# --- execução -----------------------------------------------------------------
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_NAME="$STAMP-$ENVIRONMENT-${PKG[MODB_GIT_COMMIT]:0:12}"
RUN_DIR="$RESULTS_DIR/$RUN_NAME"
TOTAL=$(( REPEAT * ${#CASES[@]} ))

echo "perfpack: pacote ${PKG[PACKAGE_ID]}  ambiente $ENVIRONMENT  suíte $SUITE_NAME"
echo "perfpack: ${#CASES[@]} caso(s) x $REPEAT repetição(ões) = $TOTAL execução(ões); binário $BINARY_ORIGIN"
if [[ "$DRY_RUN" -eq 1 ]]; then
    for spec in "${CASES[@]}"; do echo "  $spec"; done
    echo "perfpack: --dry-run, nada executado. Resultado iria para $RUN_DIR"
    exit 0
fi

mkdir -p "$RUN_DIR/raw" "$RUN_DIR/logs" "$RUN_DIR/work"
RUN_DIR="$(cd "$RUN_DIR" && pwd)"
STARTED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf 'repetition\tindex\tcase\texit_code\tstatus\tfile\n' > "$RUN_DIR/executions.tsv"

FAILURES=0
N=0
for (( rep = 1; rep <= REPEAT; rep++ )); do
    # Ida nas repetições ímpares, volta nas pares: nenhum caso fica sempre
    # depois do mesmo vizinho.
    order=()
    for (( i = 0; i < ${#CASES[@]}; i++ )); do order+=("$i"); done
    if (( rep % 2 == 0 )); then
        reversed=()
        for (( i = ${#order[@]} - 1; i >= 0; i-- )); do reversed+=("${order[$i]}"); done
        order=("${reversed[@]}")
    fi
    for i in "${order[@]}"; do
        N=$((N + 1))
        read -r -a spec <<< "${CASES[$i]}"
        case_id="${spec[0]}"
        extra=("${spec[@]:1}")
        work="$RUN_DIR/work/r$rep-c$i"
        log="$RUN_DIR/logs/r$rep-c$i-$case_id.log"
        mkdir -p "$work"
        printf '[%d/%d] rep %d  %s ... ' "$N" "$TOTAL" "$rep" "$case_id"
        set +e
        "$BIN" run --profile load-local --case "$case_id" \
            --output-dir "$RUN_DIR/raw" --work-dir "$work" \
            --environment "$ENVIRONMENT" --environments-file "$ENVIRONMENTS_FILE" \
            --seed "$SEED" --no-index --accept-unknown-budget "${extra[@]}" > "$log" 2>&1
        code=$?
        set -e
        rm -rf "$work"
        file="$(sed -n 's/^Resultado:[[:space:]]*\([^[:space:]]*\).*/\1/p' "$log" | tail -n 1)"
        status="completed"
        if [[ $code -ne 0 ]]; then
            status="exit_$code"
        elif [[ -z "$file" || ! -f "$file" ]]; then
            status="no_result"
        elif grep -q '"record":"case_error"' "$file"; then
            status="case_error"
        elif ! grep -q '"record":"case_summary".*"status":"completed"' "$file"; then
            status="incomplete"
        fi
        rel=""
        [[ -n "$file" && -f "$file" ]] && rel="raw/$(basename "$file")"
        printf '%d\t%d\t%s\t%d\t%s\t%s\n' "$rep" "$i" "$case_id" "$code" "$status" "$rel" >> "$RUN_DIR/executions.tsv"
        if [[ "$status" == "completed" ]]; then
            echo "ok"
        else
            FAILURES=$((FAILURES + 1))
            echo "FALHOU ($status; ver logs/$(basename "$log"))"
        fi
    done
done
rmdir "$RUN_DIR/work" 2>/dev/null || true
FINISHED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
RUN_STATUS="completed"
[[ $FAILURES -gt 0 ]] && RUN_STATUS="failed"

# --- manifesto ----------------------------------------------------------------
json_escape() {
    local s="$1"
    s="${s//\\/\\\\}"; s="${s//\"/\\\"}"; s="${s//$'\t'/ }"; s="${s//$'\n'/ }"; s="${s//$'\r'/}"
    printf '%s' "$s"
}
governor="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)"
{
    printf '{\n'
    printf '  "schema": "modb.perfpack.run",\n  "schema_version": 1,\n'
    printf '  "run_name": "%s",\n' "$RUN_NAME"
    printf '  "environment": "%s",\n' "$ENVIRONMENT"
    printf '  "package_id": "%s",\n' "$(json_escape "${PKG[PACKAGE_ID]}")"
    printf '  "git_commit": "%s",\n' "$(json_escape "$MODB_GIT_COMMIT")"
    printf '  "git_branch": "%s",\n' "$(json_escape "$MODB_GIT_BRANCH")"
    printf '  "git_dirty": %s,\n' "$([[ "$MODB_GIT_DIRTY" == 1 ]] && echo true || echo false)"
    printf '  "runner": "run.sh",\n  "os": "linux",\n'
    printf '  "cpu_governor": "%s",\n' "$(json_escape "$governor")"
    printf '  "binary_origin": "%s",\n' "$BINARY_ORIGIN"
    printf '  "suite": "%s",\n' "$(json_escape "$SUITE_NAME")"
    printf '  "only": "%s",\n' "$(json_escape "$ONLY")"
    printf '  "repeat": %d,\n  "seed": "%s",\n' "$REPEAT" "$(json_escape "$SEED")"
    printf '  "started_at": "%s",\n  "finished_at": "%s",\n' "$STARTED_AT" "$FINISHED_AT"
    printf '  "status": "%s",\n  "executions": %d,\n  "failures": %d,\n' "$RUN_STATUS" "$TOTAL" "$FAILURES"
    printf '  "files": ['
    first=1
    while IFS= read -r -d '' f; do
        rel="${f#"$RUN_DIR"/}"
        sum="$(sha256sum "$f" | cut -d' ' -f1)"
        bytes="$(wc -c < "$f" | tr -d ' ')"
        [[ $first -eq 1 ]] && first=0 || printf ','
        printf '\n    {"path": "%s", "sha256": "%s", "bytes": %s}' "$(json_escape "$rel")" "$sum" "$bytes"
    done < <(cd "$RUN_DIR" && find . -type f ! -name manifest.json ! -name DONE -print0 | sort -z | sed -z 's#^\./#'"$RUN_DIR"'/#')
    printf '\n  ]\n}\n'
} > "$RUN_DIR/manifest.json"
# DONE por último: `perfpack.py fetch` só traz resultados completos.
echo "$RUN_STATUS" > "$RUN_DIR/DONE"

echo "perfpack: $RUN_STATUS ($FAILURES falha(s) em $TOTAL execução(ões))"
echo "PERFPACK_RUN $RUN_NAME $RUN_STATUS"
[[ "$RUN_STATUS" == "completed" ]]
