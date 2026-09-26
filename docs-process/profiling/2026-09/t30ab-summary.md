# T30 A/B

## @build/ab-pre30/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.262 (2.2%) | 111.112 (3.7%) | 481 | 42.1% | — |
| read | 5 | 125.352 (1.6%) | 579.809 (3.1%) | 481 | 78.4% | — |
| update_inplace | 5 | 92.388 (1.8%) | 100.816 (2.0%) | 954 | 8.4% | — |
| update_grow | 5 | 76.175 (4.0%) | 83.139 (4.3%) | 1.721 | 8.4% | — |
| update_shrink | 5 | 68.568 (3.0%) | 72.552 (3.1%) | 2.656 | 5.5% | — |
| delete | 5 | 204.559 (1.2%) | 209.036 (1.2%) | 2.720 | 2.1% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.251 (1.8%) | 111.870 (2.3%) | 481 | 42.6% | — |
| read | 5 | 129.329 (2.0%) | 711.535 (2.2%) | 481 | 81.8% | — |
| update_inplace | 5 | 101.658 (2.3%) | 111.912 (2.5%) | 954 | 9.2% | — |
| update_grow | 5 | 82.892 (1.5%) | 91.202 (1.6%) | 1.721 | 9.1% | — |
| update_shrink | 5 | 73.432 (2.3%) | 78.061 (2.5%) | 2.656 | 5.9% | — |
| delete | 5 | 235.005 (4.6%) | 241.224 (4.7%) | 2.720 | 2.6% | — |

