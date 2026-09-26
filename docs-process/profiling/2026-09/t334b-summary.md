# T33.4 A/B sem GC (so update)

## @base eddde62: modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 72.457 (1.6%) | 132.454 (3.0%) | 481 | 45.3% | — |
| read | 5 | 133.902 (1.4%) | 764.627 (2.8%) | 481 | 82.5% | — |
| update_inplace | 5 | 115.417 (4.0%) | 128.798 (4.4%) | 954 | 10.4% | — |
| update_grow | 5 | 96.591 (1.2%) | 107.782 (1.2%) | 1.721 | 10.4% | — |
| update_shrink | 5 | 87.947 (2.6%) | 94.295 (2.7%) | 2.656 | 6.7% | — |
| delete | 5 | 281.156 (4.8%) | 289.349 (4.9%) | 2.720 | 2.8% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 72.726 (1.3%) | 133.727 (2.0%) | 481 | 45.6% | — |
| read | 5 | 136.293 (1.1%) | 818.420 (2.1%) | 481 | 83.3% | — |
| update_inplace | 5 | 112.795 (3.2%) | 125.271 (3.4%) | 954 | 10.0% | — |
| update_grow | 5 | 85.276 (1.1%) | 93.769 (1.2%) | 1.729 | 9.1% | — |
| update_shrink | 5 | 73.275 (3.0%) | 77.656 (2.9%) | 2.914 | 5.6% | — |
| delete | 5 | 271.515 (16.8%) | 279.401 (17.0%) | 2.979 | 2.8% | — |

