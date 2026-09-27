#!/usr/bin/env bash
#
# Descreve a máquina Linux em que o pacote de performance roda: hardware,
# virtualização, disco, kernel e tudo que muda os números de uma medição.
# Imprime seções "### nome" seguidas da saída crua de cada comando;
# scripts/perfpack.py machine a transforma em load-history/machines/<ambiente>.{json,md}.
#
# Só lê o sistema (/proc, /sys, comandos de consulta): nenhuma carga de CPU,
# disco ou rede além de ler estes arquivos, e nenhuma escrita. A amostra de
# steal é um sleep de 5 s entre duas leituras de /proc/stat.
#
# Uso: bash machine-info.sh [DIR_DE_TRABALHO]   (padrão: ~/modb-perfpack)

set -u
WORK_DIR="${1:-$HOME/modb-perfpack}"

section() { printf '\n### %s\n' "$1"; }
run() { "$@" 2>&1 || true; }

section collected_at;      date -u +%Y-%m-%dT%H:%M:%SZ
section os_release;        run cat /etc/os-release
section uname;             run uname -a
section kernel_cmdline;    run cat /proc/cmdline
section glibc;             run ldd --version | head -n 1
section virtualization;    run systemd-detect-virt
section dmi
for f in sys_vendor product_name product_version bios_vendor bios_version; do
    printf '%s: %s\n' "$f" "$(cat /sys/class/dmi/id/$f 2>/dev/null)"
done
section cloud_metadata
# Metadados do provedor (Digital Ocean). Endereço link-local, só existe dentro
# da nuvem; fora dela a seção fica vazia.
if command -v curl >/dev/null 2>&1; then
    for k in id region hostname tags; do
        printf '%s: %s\n' "$k" "$(curl -s -m 2 "http://169.254.169.254/metadata/v1/$k" | tr '\n' ' ')"
    done
fi
section lscpu;             run lscpu
section cpu_flags;         grep -m1 '^flags' /proc/cpuinfo | cut -d: -f2 | tr ' ' '\n' | grep -E '^(avx|avx2|avx512f|avx512bw|sse4_2|bmi2|aes|sha_ni|pclmulqdq|rdrand|hypervisor|constant_tsc|nonstop_tsc)$' | sort | tr '\n' ' '
section microcode;         grep -m1 microcode /proc/cpuinfo
section cpu_vulnerabilities
for f in /sys/devices/system/cpu/vulnerabilities/*; do printf '%s: %s\n' "$(basename "$f")" "$(cat "$f")"; done
section cpufreq
printf 'governor: %s\n' "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'n/a (sem cpufreq: VM)')"
printf 'boost: %s\n' "$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo 'n/a')"
section clocksource;       run cat /sys/devices/system/clocksource/clocksource0/current_clocksource
section cpu_steal
# /proc/stat: user nice system idle iowait irq softirq steal. O steal é o tempo
# que o hipervisor deu a outros; numa CPU dedicada deve ficar perto de zero.
read -r _ u n s i w q sq st _ < /proc/stat
sleep 5
read -r _ u2 n2 s2 i2 w2 q2 sq2 st2 _ < /proc/stat
total=$(( (u2+n2+s2+i2+w2+q2+sq2+st2) - (u+n+s+i+w+q+sq+st) ))
printf 'steal_pct_5s: %s\n' "$(awk -v a=$((st2-st)) -v t=$total 'BEGIN{ if (t>0) printf "%.2f", 100*a/t; else print "0" }')"
printf 'loadavg: %s\n' "$(cat /proc/loadavg)"
section memory;            run free -b
section meminfo;           grep -E '^(MemTotal|SwapTotal|HugePages_Total|Hugepagesize)' /proc/meminfo
section transparent_hugepage; run cat /sys/kernel/mm/transparent_hugepage/enabled
section vm_sysctl;         run sysctl vm.dirty_ratio vm.dirty_background_ratio vm.dirty_expire_centisecs vm.swappiness
section numa;              run lscpu -e=CPU,NODE,SOCKET,CORE
section block_devices;     run lsblk -b -o NAME,SIZE,TYPE,ROTA,FSTYPE,MOUNTPOINT,MODEL,TRAN
section block_queue
for d in /sys/block/*; do
    n=$(basename "$d"); case "$n" in loop*|ram*) continue ;; esac
    printf '%s: scheduler=%s rotational=%s logical_block=%s physical_block=%s write_cache=%s nr_requests=%s\n' "$n" \
        "$(cat "$d/queue/scheduler" 2>/dev/null)" "$(cat "$d/queue/rotational" 2>/dev/null)" \
        "$(cat "$d/queue/logical_block_size" 2>/dev/null)" "$(cat "$d/queue/physical_block_size" 2>/dev/null)" \
        "$(cat "$d/queue/write_cache" 2>/dev/null)" "$(cat "$d/queue/nr_requests" 2>/dev/null)"
done
section work_filesystem;   run df -hT "$WORK_DIR"; run findmnt -no SOURCE,FSTYPE,OPTIONS -T "$WORK_DIR"
section ext4_features
dev=$(findmnt -no SOURCE -T "$WORK_DIR" 2>/dev/null)
if command -v tune2fs >/dev/null 2>&1 && [[ -n "$dev" ]]; then
    tune2fs -l "$dev" 2>/dev/null | grep -E '^(Filesystem features|Block size|Journal|Default mount options)' || true
fi
section swap;              run swapon --show
section toolchain
printf 'g++: %s\ncmake: %s\nninja: %s\n' "$(g++ --version 2>/dev/null | head -n1)" \
    "$(cmake --version 2>/dev/null | head -n1)" "$(ninja --version 2>/dev/null)"
section uptime;            run uptime
