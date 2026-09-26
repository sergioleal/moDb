# T21 retenção MVCC

## load.snapshot_hold.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 71.743 (4.3%) | 132.839 (6.2%) | 483 | 46.0% | — |
| hold | 3 | 2.416 (5.9%) | 2.912 (5.8%) | 0 | 17.0% | — |
| snapshot_read_fresh | 3 | 137.861 (1.8%) | 886.637 (3.7%) | 0 | 84.4% | — |
| snapshot_read_retained | 3 | 129.116 (6.5%) | 802.957 (5.2%) | 0 | 83.9% | — |

## load.snapshot_hold.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 67.041 (6.7%) | 118.276 (9.0%) | 481 | 43.2% | — |
| hold | 3 | 2.560 (2.6%) | 3.033 (3.3%) | 0 | 15.6% | — |
| snapshot_read_fresh | 3 | 137.112 (5.0%) | 804.918 (10.5%) | 0 | 82.9% | — |
| snapshot_read_retained | 3 | 139.794 (2.4%) | 837.045 (6.3%) | 0 | 83.3% | — |

