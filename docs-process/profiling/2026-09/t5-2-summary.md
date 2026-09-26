# T5.2 durability

## load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.112 (3.2%) | 174.020 (4.0%) | 483 | 63.1% | — |
| mixed_oltp | 5 | 7.699 (2.0%) | 7.705 (2.0%) | 2.687 | 0.1% | — |

## load.mixed_oltp.embedded.10k --durability disabled_diagnostic

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 72.265 (4.6%) | 176.269 (4.7%) | 483 | 59.0% | — |
| mixed_oltp | 5 | 68.567 (7.7%) | 68.948 (7.7%) | 2.687 | 0.6% | — |

## load.snapshot_hold.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 61.164 (3.1%) | 169.593 (3.0%) | 483 | 63.9% | — |
| hold | 5 | 958 (1.7%) | 1.122 (1.4%) | 0 | 14.6% | — |

## load.snapshot_hold.embedded.10k --durability disabled_diagnostic

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 74.787 (1.4%) | 184.073 (1.5%) | 483 | 59.4% | — |
| hold | 5 | 10.453 (1.4%) | 14.038 (1.7%) | 0 | 25.5% | — |

## load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.506 (1.4%) | 176.563 (3.0%) | 481 | 63.5% | — |
| read | 5 | 122.665 (1.4%) | 447.426 (1.0%) | 481 | 72.6% | — |
| update_inplace | 5 | 80.383 (1.4%) | 128.198 (1.9%) | 954 | 37.3% | — |
| update_grow | 5 | 66.500 (2.5%) | 113.177 (1.7%) | 1.721 | 41.2% | — |
| update_shrink | 5 | 56.124 (1.9%) | 105.709 (1.7%) | 2.656 | 46.9% | — |
| delete | 5 | 156.556 (2.7%) | 230.921 (2.5%) | 2.720 | 32.2% | — |

## load.crud_full.embedded.100k --durability disabled_diagnostic

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 74.329 (2.7%) | 181.266 (3.1%) | 481 | 59.0% | — |
| read | 5 | 123.763 (1.8%) | 448.106 (3.0%) | 481 | 72.4% | — |
| update_inplace | 5 | 95.226 (2.3%) | 127.764 (2.4%) | 954 | 25.5% | — |
| update_grow | 5 | 79.575 (2.8%) | 116.190 (2.8%) | 1.721 | 31.5% | — |
| update_shrink | 5 | 73.658 (4.1%) | 109.209 (3.8%) | 2.656 | 32.6% | — |
| delete | 5 | 240.141 (3.0%) | 265.668 (3.1%) | 2.720 | 9.6% | — |

