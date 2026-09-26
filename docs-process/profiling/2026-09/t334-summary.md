# T33.4 A/B sem GC

## @base eddde62: modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 71.276 (5.5%) | 131.320 (6.9%) | 481 | 45.7% | — |
| read | 5 | 134.214 (3.5%) | 755.329 (7.0%) | 481 | 82.2% | — |
| update_inplace | 5 | 114.518 (7.5%) | 127.926 (7.7%) | 954 | 10.5% | — |
| update_grow | 5 | 97.954 (4.2%) | 109.611 (4.4%) | 1.721 | 10.6% | — |
| update_shrink | 5 | 88.918 (3.3%) | 95.666 (3.3%) | 2.656 | 7.1% | — |
| delete | 5 | 282.114 (3.8%) | 290.079 (3.8%) | 2.720 | 2.7% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 72.527 (3.7%) | 133.821 (5.2%) | 481 | 45.8% | — |
| read | 5 | 136.245 (1.7%) | 809.743 (2.4%) | 481 | 83.2% | — |
| update_inplace | 5 | 115.320 (3.1%) | 128.282 (3.3%) | 954 | 10.1% | — |
| update_grow | 5 | 87.623 (1.9%) | 96.626 (2.1%) | 1.729 | 9.3% | — |
| update_shrink | 5 | 74.393 (3.8%) | 78.934 (3.8%) | 2.914 | 5.8% | — |
| delete | 5 | 134.353 (3.3%) | 136.105 (3.3%) | 3.687 | 1.3% | — |

## @base eddde62: modb_load.exe load.create_delete_interleaved.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 69.476 (2.5%) | 125.124 (3.7%) | 481 | 44.5% | — |
| delete | 5 | 196.455 (16.5%) | 200.445 (16.6%) | 837 | 2.0% | — |

## @build/relwithdebinfo/modb_load.exe load.create_delete_interleaved.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 69.116 (3.4%) | 124.495 (4.6%) | 481 | 44.5% | — |
| delete | 5 | 188.139 (14.0%) | 191.897 (14.1%) | 837 | 1.9% | — |

