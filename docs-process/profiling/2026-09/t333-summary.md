# T33.3 A/B

## @build/ab-pre333/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.752 (4.8%) | 114.452 (5.7%) | 481 | 43.4% | — |
| read | 5 | 123.356 (10.9%) | 695.297 (16.8%) | 481 | 82.1% | — |
| update_inplace | 5 | 95.664 (15.4%) | 106.136 (15.7%) | 954 | 9.8% | — |
| update_grow | 5 | 83.345 (12.3%) | 92.180 (12.8%) | 1.721 | 9.5% | — |
| update_shrink | 5 | 72.810 (13.9%) | 77.657 (14.1%) | 2.656 | 6.2% | — |
| delete | 5 | 241.785 (6.1%) | 248.277 (6.2%) | 2.720 | 2.6% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 63.759 (7.3%) | 112.258 (8.2%) | 481 | 43.2% | — |
| read | 5 | 122.538 (7.5%) | 663.291 (10.6%) | 481 | 81.5% | — |
| update_inplace | 5 | 100.565 (4.7%) | 111.317 (4.9%) | 954 | 9.7% | — |
| update_grow | 5 | 84.353 (4.5%) | 93.349 (4.8%) | 1.721 | 9.6% | — |
| update_shrink | 5 | 74.175 (6.2%) | 79.091 (6.3%) | 2.656 | 6.2% | — |
| delete | 5 | 243.705 (6.2%) | 250.327 (6.2%) | 2.720 | 2.6% | — |

## @build/ab-pre333/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 64.930 (1.9%) | 116.879 (3.1%) | 483 | 44.4% | — |
| mixed_oltp | 5 | 19.024 (5.3%) | 19.075 (5.3%) | 2.687 | 0.3% | — |

## @build/relwithdebinfo/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 67.282 (3.7%) | 122.354 (4.5%) | 483 | 45.0% | — |
| mixed_oltp | 5 | 20.299 (4.6%) | 20.347 (4.6%) | 2.687 | 0.2% | — |

