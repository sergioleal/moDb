# T29 A/B

## @build/ab-pre29/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 60.810 (5.6%) | 105.728 (5.9%) | 481 | 42.5% | — |
| read | 5 | 115.772 (12.1%) | 654.817 (7.4%) | 481 | 82.3% | — |
| update_inplace | 5 | 97.186 (2.1%) | 106.952 (2.3%) | 954 | 9.1% | — |
| update_grow | 5 | 80.275 (1.9%) | 88.259 (2.0%) | 1.721 | 9.0% | — |
| update_shrink | 5 | 71.786 (2.5%) | 76.350 (2.4%) | 2.656 | 6.0% | — |
| delete | 5 | 225.425 (4.1%) | 231.295 (4.1%) | 2.720 | 2.5% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.389 (2.8%) | 112.413 (3.3%) | 481 | 42.7% | — |
| read | 5 | 127.031 (1.3%) | 684.152 (2.1%) | 481 | 81.4% | — |
| update_inplace | 5 | 99.806 (4.8%) | 110.026 (4.8%) | 954 | 9.3% | — |
| update_grow | 5 | 76.835 (10.8%) | 84.672 (10.6%) | 1.721 | 9.3% | — |
| update_shrink | 5 | 71.648 (3.2%) | 76.159 (2.9%) | 2.656 | 5.9% | — |
| delete | 5 | 233.981 (1.7%) | 240.086 (1.7%) | 2.720 | 2.5% | — |

## @build/ab-pre29/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 63.030 (4.6%) | 111.368 (5.5%) | 483 | 43.4% | — |
| mixed_oltp | 5 | 18.257 (4.6%) | 18.305 (4.7%) | 2.687 | 0.3% | — |

## @build/relwithdebinfo/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 65.276 (2.1%) | 116.809 (3.7%) | 483 | 44.1% | — |
| mixed_oltp | 5 | 18.251 (4.4%) | 18.299 (4.4%) | 2.687 | 0.3% | — |

