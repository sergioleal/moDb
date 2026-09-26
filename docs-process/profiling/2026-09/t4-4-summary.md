# T4.4 payload

## load.create_only.embedded.10k --payload slim

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 98.644 (5.7%) | 193.397 (3.4%) | 283 | 49.0% | — |

## load.create_only.embedded.10k --payload normal

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 61.836 (6.4%) | 170.103 (5.0%) | 483 | 63.7% | — |

## load.create_only.embedded.10k --payload fat

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 6.397 (3.3%) | 37.000 (9.2%) | 8.316 | 82.6% | — |

## load.crud_full.embedded.10k --payload slim

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 96.360 (3.3%) | 189.642 (2.3%) | 283 | 49.2% | — |
| read | 3 | 253.220 (0.2%) | 508.248 (0.5%) | 283 | 50.2% | — |
| update_inplace | 3 | 90.822 (1.8%) | 137.018 (1.4%) | 564 | 33.7% | — |
| update_grow | 3 | 88.967 (2.9%) | 134.727 (1.4%) | 902 | 34.0% | — |
| update_shrink | 3 | 89.160 (4.0%) | 139.724 (0.7%) | 1.210 | 36.2% | — |
| delete | 3 | 209.532 (2.1%) | 365.287 (3.3%) | 1.274 | 42.6% | — |

## load.crud_full.embedded.10k --payload normal

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 62.446 (6.8%) | 169.760 (7.3%) | 483 | 63.2% | — |
| read | 3 | 123.291 (2.7%) | 478.411 (3.3%) | 483 | 74.2% | — |
| update_inplace | 3 | 77.336 (8.3%) | 126.553 (6.4%) | 955 | 38.9% | — |
| update_grow | 3 | 60.860 (3.2%) | 104.413 (4.1%) | 1.721 | 41.7% | — |
| update_shrink | 3 | 52.611 (8.6%) | 104.777 (10.2%) | 2.706 | 49.7% | — |
| delete | 3 | 170.548 (4.1%) | 271.596 (5.7%) | 2.770 | 37.2% | — |

