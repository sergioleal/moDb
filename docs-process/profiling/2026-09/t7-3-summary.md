# T7.3 um wal_sync

## load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 59.488 (5.4%) | 99.071 (7.5%) | 483 | 39.9% | — |
| mixed_oltp | 5 | 8.091 (2.2%) | 8.100 (2.2%) | 2.687 | 0.1% | — |

## load.snapshot_hold.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 57.197 (3.4%) | 95.263 (4.8%) | 483 | 39.9% | — |
| hold | 5 | 1.013 (3.0%) | 1.189 (3.0%) | 0 | 14.8% | — |

## load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 60.227 (2.8%) | 100.005 (3.6%) | 481 | 39.8% | — |
| read | 5 | 113.395 (4.0%) | 397.574 (6.6%) | 481 | 71.4% | — |
| update_inplace | 5 | 72.159 (1.4%) | 77.425 (1.4%) | 954 | 6.8% | — |
| update_grow | 5 | 59.887 (3.6%) | 64.370 (3.7%) | 1.721 | 7.0% | — |
| update_shrink | 5 | 47.950 (1.5%) | 49.914 (1.5%) | 2.656 | 3.9% | — |
| delete | 5 | 146.122 (4.6%) | 148.477 (4.6%) | 2.720 | 1.6% | — |

