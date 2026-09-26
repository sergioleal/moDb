# T4.2 page 8 KiB

## load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 65.859 (6.8%) | 189.755 (0.4%) | 481 | 65.3% | — |
| read | 3 | 125.693 (0.4%) | 460.574 (0.5%) | 481 | 72.7% | — |
| update_inplace | 3 | 79.730 (10.9%) | 131.442 (2.2%) | 954 | 39.4% | — |
| update_grow | 3 | 67.917 (11.5%) | 119.629 (2.4%) | 1.721 | 43.3% | — |
| update_shrink | 3 | 60.080 (1.6%) | 114.128 (0.6%) | 2.656 | 47.4% | — |
| delete | 3 | 180.219 (3.3%) | 267.045 (0.1%) | 2.720 | 32.5% | — |

## load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 67.915 (1.3%) | 186.560 (2.7%) | 483 | 63.6% | — |
| mixed_oltp | 3 | 8.285 (1.3%) | 8.292 (1.3%) | 2.687 | 0.1% | — |

