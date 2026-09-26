# T29 cache 1024 vs 8192

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 63.637 (4.0%) | 111.035 (4.5%) | 481 | 42.7% | — |
| read | 5 | 127.997 (0.6%) | 690.501 (1.5%) | 481 | 81.5% | — |
| update_inplace | 5 | 97.290 (3.7%) | 106.968 (4.0%) | 954 | 9.0% | — |
| update_grow | 5 | 82.481 (3.1%) | 90.786 (3.3%) | 1.721 | 9.1% | — |
| update_shrink | 5 | 72.196 (2.4%) | 76.644 (2.5%) | 2.656 | 5.8% | — |
| delete | 5 | 226.823 (4.8%) | 232.914 (4.9%) | 2.720 | 2.6% | — |

## @build/ab-cache8k/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 62.423 (1.3%) | 106.076 (2.2%) | 481 | 41.1% | — |
| read | 5 | 127.477 (1.4%) | 718.350 (1.8%) | 481 | 82.2% | — |
| update_inplace | 5 | 88.877 (3.3%) | 96.680 (3.7%) | 954 | 8.1% | — |
| update_grow | 5 | 72.278 (3.3%) | 78.512 (3.5%) | 1.721 | 7.9% | — |
| update_shrink | 5 | 70.839 (2.9%) | 75.005 (3.1%) | 2.656 | 5.5% | — |
| delete | 5 | 234.641 (4.7%) | 240.277 (4.8%) | 2.720 | 2.3% | — |

