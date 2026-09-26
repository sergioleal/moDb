# A/B base vs T7.3 vs T7.3+T9

## @build/ab-base/modb_load.exe load.mixed_oltp.embedded.10k --checkpoint-interval 1

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 61.653 (5.6%) | 103.128 (7.3%) | 483 | 40.2% | — |
| mixed_oltp | 5 | 7.024 (10.2%) | 7.030 (10.2%) | 2.687 | 0.1% | — |

## @build/relwithdebinfo/modb_load.exe load.mixed_oltp.embedded.10k --checkpoint-interval 1

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.614 (4.2%) | 109.971 (5.5%) | 483 | 41.2% | — |
| mixed_oltp | 5 | 9.165 (6.0%) | 9.173 (6.0%) | 2.687 | 0.1% | — |

## @build/relwithdebinfo/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 68.652 (4.8%) | 122.978 (6.1%) | 483 | 44.1% | — |
| mixed_oltp | 5 | 19.126 (11.3%) | 19.163 (11.3%) | 2.687 | 0.2% | — |

## @build/ab-base/modb_load.exe load.snapshot_hold.embedded.10k --checkpoint-interval 1

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 63.541 (4.6%) | 107.585 (6.1%) | 483 | 40.9% | — |
| hold | 5 | 900 (7.6%) | 1.052 (7.4%) | 0 | 14.5% | — |

## @build/relwithdebinfo/modb_load.exe load.snapshot_hold.embedded.10k --checkpoint-interval 1

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.249 (5.9%) | 109.586 (8.2%) | 483 | 41.3% | — |
| hold | 5 | 1.125 (7.1%) | 1.318 (7.2%) | 0 | 14.6% | — |

## @build/relwithdebinfo/modb_load.exe load.snapshot_hold.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 68.378 (3.8%) | 122.568 (4.5%) | 483 | 44.2% | — |
| hold | 5 | 2.518 (6.6%) | 3.027 (8.5%) | 0 | 16.7% | — |

## @build/ab-base/modb_load.exe load.crud_full.embedded.100k --checkpoint-interval 1

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 62.229 (7.0%) | 103.102 (8.3%) | 481 | 39.6% | — |
| read | 5 | 114.155 (8.8%) | 415.791 (10.5%) | 481 | 72.5% | — |
| update_inplace | 5 | 78.623 (5.0%) | 84.372 (5.2%) | 954 | 6.8% | — |
| update_grow | 5 | 65.460 (5.6%) | 70.375 (5.7%) | 1.721 | 7.0% | — |
| update_shrink | 5 | 54.440 (6.1%) | 56.782 (6.2%) | 2.656 | 4.1% | — |
| delete | 5 | 164.286 (6.3%) | 166.978 (6.3%) | 2.720 | 1.6% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k --checkpoint-interval 1

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.489 (5.8%) | 108.339 (7.6%) | 481 | 40.4% | — |
| read | 5 | 118.407 (2.8%) | 425.923 (5.5%) | 481 | 72.2% | — |
| update_inplace | 5 | 75.997 (8.9%) | 81.572 (9.1%) | 954 | 6.8% | — |
| update_grow | 5 | 64.235 (11.5%) | 69.134 (11.7%) | 1.721 | 7.1% | — |
| update_shrink | 5 | 55.349 (7.2%) | 57.796 (7.4%) | 2.656 | 4.2% | — |
| delete | 5 | 166.207 (8.8%) | 168.955 (8.8%) | 2.720 | 1.6% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 68.647 (6.2%) | 120.376 (7.7%) | 481 | 42.9% | — |
| read | 5 | 119.159 (3.0%) | 431.075 (3.9%) | 481 | 72.3% | — |
| update_inplace | 5 | 85.612 (3.9%) | 92.499 (4.0%) | 954 | 7.4% | — |
| update_grow | 5 | 70.887 (4.8%) | 76.613 (5.0%) | 1.721 | 7.5% | — |
| update_shrink | 5 | 62.896 (5.0%) | 66.087 (5.1%) | 2.656 | 4.8% | — |
| delete | 5 | 196.216 (6.8%) | 199.951 (6.9%) | 2.720 | 1.9% | — |

