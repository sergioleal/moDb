# T4.2 page 16 KiB

## load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 55.317 (3.2%) | 117.396 (4.1%) | 521 | 52.9% | — |
| read | 3 | 104.110 (0.9%) | 263.512 (0.9%) | 521 | 60.5% | — |
| update_inplace | 3 | 56.345 (5.3%) | 74.760 (3.6%) | 1.026 | 24.7% | — |
| update_grow | 3 | 51.530 (5.3%) | 71.357 (4.5%) | 1.797 | 27.8% | — |
| update_shrink | 3 | 37.371 (0.7%) | 61.306 (0.3%) | 3.523 | 39.0% | — |
| delete | 3 | 109.822 (3.0%) | 136.456 (1.5%) | 3.604 | 19.5% | — |

## load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 56.185 (4.0%) | 120.770 (1.1%) | 524 | 53.5% | — |
| mixed_oltp | 3 | 7.368 (6.8%) | 7.374 (6.8%) | 5.241 | 0.1% | — |

