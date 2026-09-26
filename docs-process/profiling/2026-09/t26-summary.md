# T26 A/B

## @build/ab-pre26/modb_load.exe load.restart_recovery.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 66.945 (4.4%) | 119.992 (5.6%) | 483 | 44.2% | — |
| restart_recovery | 5 | 0 (0.0%) | — | 0 | 0.0% | — |

## @build/relwithdebinfo/modb_load.exe load.restart_recovery.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 65.149 (5.2%) | 116.214 (6.8%) | 483 | 43.9% | — |
| restart_recovery | 5 | 0 (0.0%) | — | 0 | 0.0% | — |

## @build/ab-pre26/modb_load.exe load.restart_recovery.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.876 (5.4%) | 113.088 (7.0%) | 481 | 42.6% | — |
| restart_recovery | 5 | 0 (0.0%) | — | 0 | 0.0% | — |

## @build/relwithdebinfo/modb_load.exe load.restart_recovery.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 62.600 (5.6%) | 109.539 (6.3%) | 481 | 42.8% | — |
| restart_recovery | 5 | 0 (0.0%) | — | 0 | 0.0% | — |

## @build/ab-pre26/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 63.443 (3.0%) | 113.505 (3.1%) | 483 | 44.1% | — |
| mixed_oltp | 5 | 18.490 (6.6%) | 18.533 (6.6%) | 2.687 | 0.2% | — |

## @build/relwithdebinfo/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 62.331 (8.2%) | 109.415 (13.5%) | 483 | 42.7% | — |
| mixed_oltp | 5 | 13.194 (58.8%) | 13.222 (58.8%) | 2.687 | 0.2% | — |

