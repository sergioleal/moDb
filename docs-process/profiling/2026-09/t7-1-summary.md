# T7.1 A/B

## @build/ab-t9/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 66.098 (5.6%) | 116.738 (7.0%) | 483 | 43.3% | — |
| mixed_oltp | 5 | 17.997 (7.2%) | 18.038 (7.2%) | 2.687 | 0.2% | — |

## @build/relwithdebinfo/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 69.028 (4.0%) | 126.198 (6.2%) | 483 | 45.2% | — |
| mixed_oltp | 5 | 19.670 (4.3%) | 19.719 (4.3%) | 2.687 | 0.2% | — |

## @build/ab-t9/modb_load.exe load.create_only.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 63.234 (2.8%) | 109.135 (1.9%) | 481 | 42.1% | — |

## @build/relwithdebinfo/modb_load.exe load.create_only.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 65.694 (9.6%) | 116.056 (11.9%) | 481 | 43.3% | — |

## @build/ab-t9/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.673 (4.9%) | 110.833 (6.3%) | 481 | 41.6% | — |
| read | 5 | 115.287 (3.0%) | 409.224 (4.4%) | 481 | 71.8% | — |
| update_inplace | 5 | 78.465 (8.5%) | 84.578 (8.7%) | 954 | 7.2% | — |
| update_grow | 5 | 65.299 (5.8%) | 70.432 (5.9%) | 1.721 | 7.3% | — |
| update_shrink | 5 | 60.402 (2.8%) | 63.424 (2.9%) | 2.656 | 4.8% | — |
| delete | 5 | 174.633 (1.0%) | 178.022 (1.0%) | 2.720 | 1.9% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 69.034 (3.9%) | 123.819 (5.1%) | 481 | 44.2% | — |
| read | 5 | 112.450 (6.3%) | 395.313 (9.4%) | 481 | 71.5% | — |
| update_inplace | 5 | 83.027 (5.1%) | 89.844 (5.2%) | 954 | 7.6% | — |
| update_grow | 5 | 71.390 (5.8%) | 77.491 (5.9%) | 1.721 | 7.9% | — |
| update_shrink | 5 | 62.891 (1.5%) | 66.180 (1.4%) | 2.656 | 5.0% | — |
| delete | 5 | 192.705 (8.6%) | 196.418 (8.6%) | 2.720 | 1.9% | — |

