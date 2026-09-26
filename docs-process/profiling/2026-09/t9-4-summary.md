# T9.4 restart_recovery ckpt1 vs 64

## load.restart_recovery.embedded.10k --checkpoint-interval 1

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 60.841 (3.2%) | 103.607 (4.5%) | 483 | 41.2% | — |
| restart_recovery | 5 | 0 (0.0%) | — | 0 | 0.0% | — |

## load.restart_recovery.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 66.745 (3.4%) | 119.792 (5.2%) | 483 | 44.2% | — |
| restart_recovery | 5 | 0 (0.0%) | — | 0 | 0.0% | — |

