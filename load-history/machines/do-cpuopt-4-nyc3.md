# Máquina `do-cpuopt-4-nyc3`

Descrição da máquina em que os resultados deste ambiente foram medidos
(`environment = do-cpuopt-4-nyc3` em [`series.jsonl`](../series.jsonl) e em
`load-results/remote/do-cpuopt-4-nyc3/`). Gerado por `scripts/perfpack.py machine`
a partir de `loadtests/perfpack/machine-info.sh`; a saída crua de cada comando
está em [`do-cpuopt-4-nyc3.json`](do-cpuopt-4-nyc3.json). Se a máquina mudar (kernel, plano, droplet
recriado), gere de novo: o histórico do git guarda as versões anteriores.

| | |
|---|---|
| Ambiente | `do-cpuopt-4-nyc3` — Digital Ocean CPU-Optimized Premium Intel 4 vCPU / 8 GB (NYC3) |
| host_class / device_class | `do-cpuopt-premium-4` / `nvme` |
| Coletado em | 2026-09-27T11:52:53Z |
| Plataforma | DigitalOcean Droplet (kvm; hipervisor KVM) |
| Região / droplet | nyc3 / 604082456 |
| SO | Ubuntu 24.04.4 LTS, kernel 6.8.0-124-generic, ldd (Ubuntu GLIBC 2.39-0ubuntu8.7) 2.39 |
| CPU | Intel(R) Xeon(R) Platinum 8358 CPU @ 2.60GHz |
| vCPUs | 4 (1 socket × 4 núcleos × 1 thread/núcleo; 1 nó NUMA) |
| Caches | L1d 128 KiB (4 instances) · L2 16 MiB (4 instances) · L3 não exposto pela VM |
| Extensões | aes avx avx2 avx512bw avx512f bmi2 constant_tsc hypervisor pclmulqdq rdrand sse4_2 |
| Frequência | governor n/a (sem cpufreq: VM); clocksource kvm-clock |
| CPU steal (5 s ocioso) | 0.00% |
| Memória | 7.8 GiB (swap 0.0 GiB; THP always [madvise] never) |
| Disco (fila) | vda: scheduler=none [mq-deadline]  rotational=1 logical_block=512 physical_block=512 write_cache=write back nr_requests=256<br>vdb: scheduler=none [mq-deadline]  rotational=1 logical_block=512 physical_block=512 write_cache=write back nr_requests=256 |
| FS do diretório de trabalho | /dev/vda1      ext4   48G  2.0G   46G   5% /<br>/dev/vda1 ext4   rw,relatime,discard,errors=remount-ro,commit=30 |
| Mitigações ativas | indirect_target_selection, l1tf, meltdown, spec_store_bypass, spectre_v1, spectre_v2 |
| Vulnerável (sem mitigação) | mds, mmio_stale_data |

## Como ler os números deste ambiente

- O disco (fila, cache de escrita, sistema de arquivos) limita os casos de
  commit pequeno (`mixed_oltp`, `snapshot_hold`), um `fsync` por commit; os com
  lote de 1000 objetos (`create_*`, `crud_full`) quase não o sentem.
- A coleta só lê o sistema: nada aqui foi medido com carga.
- `steal` perto de zero confirma CPU dedicada; se subir numa coleta futura, os
  resultados do período ficam suspeitos de vizinhos barulhentos.
- Compare commits **dentro** deste ambiente; entre ambientes, só a forma das
  curvas, nunca o valor absoluto.

## Rodadas medidas nesta máquina

Cada rodada está em `load-results/remote/do-cpuopt-4-nyc3/<rodada>/` (brutos, fora do git)
e seus pontos em `series.jsonl` (`run_id` de cada arquivo em `raw/`). Esta lista é
regerada a cada `perfpack.py machine` e a cada `fetch`.

| rodada | commit | suíte | execuções | falhas | início (UTC) | binário |
|---|---|---|---|---|---|---|
| `20260927T114607Z-do-cpuopt-4-nyc3-c132af550024` | `c132af550024` | smoke × 1 | 3 | 0 | 2026-09-27T11:46:07Z | prebuilt |
| `20260927T114808Z-do-cpuopt-4-nyc3-c132af550024` | `c132af550024` | standard × 5 | 50 | 0 | 2026-09-27T11:48:08Z | prebuilt |
| `20260927T121051Z-do-cpuopt-4-nyc3-fe8106e65cab` | `fe8106e65cab` | standard × 5 | 50 | 0 | 2026-09-27T12:10:51Z | prebuilt |
| `20260927T121601Z-do-cpuopt-4-nyc3-fe8106e65cab` | `fe8106e65cab` | large × 1 | 10 | 1 | 2026-09-27T12:16:01Z | prebuilt |

## Saída crua

<details><summary><code>collected_at</code></summary>

```
2026-09-27T11:52:53Z
```

</details>

<details><summary><code>os_release</code></summary>

```
PRETTY_NAME="Ubuntu 24.04.4 LTS"
NAME="Ubuntu"
VERSION_ID="24.04"
VERSION="24.04.4 LTS (Noble Numbat)"
VERSION_CODENAME=noble
ID=ubuntu
ID_LIKE=debian
HOME_URL="https://www.ubuntu.com/"
SUPPORT_URL="https://help.ubuntu.com/"
BUG_REPORT_URL="https://bugs.launchpad.net/ubuntu/"
PRIVACY_POLICY_URL="https://www.ubuntu.com/legal/terms-and-policies/privacy-policy"
UBUNTU_CODENAME=noble
LOGO=ubuntu-logo
```

</details>

<details><summary><code>uname</code></summary>

```
Linux ubuntu-c-4-intel-nyc3 6.8.0-124-generic #124-Ubuntu SMP PREEMPT_DYNAMIC Tue May 26 13:00:45 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
```

</details>

<details><summary><code>kernel_cmdline</code></summary>

```
BOOT_IMAGE=/vmlinuz-6.8.0-124-generic root=UUID=cc6f3ac6-f24d-4276-abe9-c242adec4e04 ro console=tty1 console=ttyS0 net.ifnames=0 biosdevname=0
```

</details>

<details><summary><code>glibc</code></summary>

```
ldd (Ubuntu GLIBC 2.39-0ubuntu8.7) 2.39
```

</details>

<details><summary><code>virtualization</code></summary>

```
kvm
```

</details>

<details><summary><code>dmi</code></summary>

```
sys_vendor: DigitalOcean
product_name: Droplet
product_version: 20171212
bios_vendor: DigitalOcean
bios_version: 20171212
```

</details>

<details><summary><code>cloud_metadata</code></summary>

```
id: 604082456
region: nyc3
hostname: agentik.al
tags:
```

</details>

<details><summary><code>lscpu</code></summary>

```
Architecture:                            x86_64
CPU op-mode(s):                          32-bit, 64-bit
Address sizes:                           40 bits physical, 48 bits virtual
Byte Order:                              Little Endian
CPU(s):                                  4
On-line CPU(s) list:                     0-3
Vendor ID:                               GenuineIntel
BIOS Vendor ID:                          QEMU
Model name:                              Intel(R) Xeon(R) Platinum 8358 CPU @ 2.60GHz
BIOS Model name:                         pc-i440fx-6.1  CPU @ 2.0GHz
BIOS CPU family:                         1
CPU family:                              6
Model:                                   106
Thread(s) per core:                      1
Core(s) per socket:                      4
Socket(s):                               1
Stepping:                                6
BogoMIPS:                                5187.82
Flags:                                   fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ht syscall nx pdpe1gb rdtscp lm constant_tsc arch_perfmon rep_good nopl xtopology cpuid tsc_known_freq pni pclmulqdq vmx ssse3 fma cx16 pcid sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes xsave avx f16c rdrand hypervisor lahf_lm abm 3dnowprefetch cpuid_fault pti ssbd ibrs ibpb tpr_shadow flexpriority ept vpid ept_ad fsgsbase bmi1 avx2 smep bmi2 erms invpcid avx512f avx512dq rdseed adx smap clflushopt clwb avx512cd avx512bw avx512vl xsaveopt xsavec xgetbv1 wbnoinvd arat vnmi avx512vbmi umip pku ospke avx512_vbmi2 gfni vaes vpclmulqdq avx512_vnni avx512_bitalg avx512_vpopcntdq
Virtualization:                          VT-x
Hypervisor vendor:                       KVM
Virtualization type:                     full
L1d cache:                               128 KiB (4 instances)
L1i cache:                               128 KiB (4 instances)
L2 cache:                                16 MiB (4 instances)
NUMA node(s):                            1
NUMA node0 CPU(s):                       0-3
Vulnerability Gather data sampling:      Unknown: Dependent on hypervisor status
Vulnerability Indirect target selection: Mitigation; Aligned branch/return thunks
Vulnerability Itlb multihit:             KVM: Mitigation: VMX disabled
Vulnerability L1tf:                      Mitigation; PTE Inversion; VMX conditional cache flushes, SMT disabled
Vulnerability Mds:                       Vulnerable: Clear CPU buffers attempted, no microcode; SMT Host state unknown
Vulnerability Meltdown:                  Mitigation; PTI
Vulnerability Mmio stale data:           Vulnerable: Clear CPU buffers attempted, no microcode; SMT Host state unknown
Vulnerability Reg file data sampling:    Not affected
Vulnerability Retbleed:                  Not affected
Vulnerability Spec rstack overflow:      Not affected
Vulnerability Spec store bypass:         Mitigation; Speculative Store Bypass disabled via prctl
Vulnerability Spectre v1:                Mitigation; usercopy/swapgs barriers and __user pointer sanitization
Vulnerability Spectre v2:                Mitigation; Retpolines; IBPB conditional; IBRS_FW; STIBP disabled; RSB filling; PBRSB-eIBRS Not affected; BHI Retpoline
Vulnerability Srbds:                     Not affected
Vulnerability Tsa:                       Not affected
Vulnerability Tsx async abort:           Not affected
Vulnerability Vmscape:                   Not affected
```

</details>

<details><summary><code>cpu_flags</code></summary>

```
aes avx avx2 avx512bw avx512f bmi2 constant_tsc hypervisor pclmulqdq rdrand sse4_2
```

</details>

<details><summary><code>microcode</code></summary>

```
microcode	: 0x1
```

</details>

<details><summary><code>cpu_vulnerabilities</code></summary>

```
gather_data_sampling: Unknown: Dependent on hypervisor status
indirect_target_selection: Mitigation: Aligned branch/return thunks
itlb_multihit: KVM: Mitigation: VMX disabled
l1tf: Mitigation: PTE Inversion; VMX: conditional cache flushes, SMT disabled
mds: Vulnerable: Clear CPU buffers attempted, no microcode; SMT Host state unknown
meltdown: Mitigation: PTI
mmio_stale_data: Vulnerable: Clear CPU buffers attempted, no microcode; SMT Host state unknown
reg_file_data_sampling: Not affected
retbleed: Not affected
spec_rstack_overflow: Not affected
spec_store_bypass: Mitigation: Speculative Store Bypass disabled via prctl
spectre_v1: Mitigation: usercopy/swapgs barriers and __user pointer sanitization
spectre_v2: Mitigation: Retpolines; IBPB: conditional; IBRS_FW; STIBP: disabled; RSB filling; PBRSB-eIBRS: Not affected; BHI: Retpoline
srbds: Not affected
tsa: Not affected
tsx_async_abort: Not affected
vmscape: Not affected
```

</details>

<details><summary><code>cpufreq</code></summary>

```
governor: n/a (sem cpufreq: VM)
boost: n/a
```

</details>

<details><summary><code>clocksource</code></summary>

```
kvm-clock
```

</details>

<details><summary><code>cpu_steal</code></summary>

```
steal_pct_5s: 0.00
loadavg: 0.32 0.52 0.26 1/181 10800
```

</details>

<details><summary><code>memory</code></summary>

```
total        used        free      shared  buff/cache   available
Mem:      8326959104   480002048  7268397056     4218880   842174464  7846957056
Swap:              0           0           0
```

</details>

<details><summary><code>meminfo</code></summary>

```
MemTotal:        8131796 kB
SwapTotal:             0 kB
HugePages_Total:       0
Hugepagesize:       2048 kB
```

</details>

<details><summary><code>transparent_hugepage</code></summary>

```
always [madvise] never
```

</details>

<details><summary><code>vm_sysctl</code></summary>

```
vm.dirty_ratio = 20
vm.dirty_background_ratio = 10
vm.dirty_expire_centisecs = 3000
vm.swappiness = 60
```

</details>

<details><summary><code>numa</code></summary>

```
CPU NODE SOCKET CORE
  0    0      0    0
  1    0      0    1
  2    0      0    2
  3    0      0    3
```

</details>

<details><summary><code>block_devices</code></summary>

```
NAME           SIZE TYPE ROTA FSTYPE  MOUNTPOINT MODEL TRAN
vda     53687091200 disk    1                          virtio
├─vda1  52612283904 part    1 ext4    /                virtio
├─vda14     4194304 part    1                          virtio
├─vda15   111149056 part    1 vfat    /boot/efi        virtio
└─vda16   957350400 part    1 ext4    /boot            virtio
vdb          501760 disk    1 iso9660                  virtio
```

</details>

<details><summary><code>block_queue</code></summary>

```
vda: scheduler=none [mq-deadline]  rotational=1 logical_block=512 physical_block=512 write_cache=write back nr_requests=256
vdb: scheduler=none [mq-deadline]  rotational=1 logical_block=512 physical_block=512 write_cache=write back nr_requests=256
```

</details>

<details><summary><code>work_filesystem</code></summary>

```
Filesystem     Type  Size  Used Avail Use% Mounted on
/dev/vda1      ext4   48G  2.0G   46G   5% /
/dev/vda1 ext4   rw,relatime,discard,errors=remount-ro,commit=30
```

</details>

<details><summary><code>ext4_features</code></summary>

```
Filesystem features:      has_journal ext_attr resize_inode dir_index filetype needs_recovery extent 64bit flex_bg sparse_super large_file huge_file dir_nlink extra_isize metadata_csum
Default mount options:    user_xattr acl
Block size:               4096
Journal inode:            8
Journal backup:           inode blocks
```

</details>

<details><summary><code>swap</code></summary>

```

```

</details>

<details><summary><code>toolchain</code></summary>

```
g++: 
cmake: 
ninja:
```

</details>

<details><summary><code>uptime</code></summary>

```
11:52:58 up 8 min,  1 user,  load average: 0.32, 0.52, 0.26
```

</details>
