# T15.5 A/B

## @build/ab-pre155/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 59.658 (8.9%) | 101.435 (13.1%) | 481 | 40.9% | — |
| read | 5 | 112.824 (2.9%) | 390.746 (4.1%) | 481 | 71.1% | — |
| update_inplace | 5 | 75.491 (4.5%) | 81.621 (4.7%) | 954 | 7.5% | — |
| update_grow | 5 | 68.614 (6.6%) | 74.521 (6.7%) | 1.721 | 7.9% | — |
| update_shrink | 5 | 62.372 (2.8%) | 65.796 (2.7%) | 2.656 | 5.2% | — |
| delete | 5 | 177.479 (3.0%) | 180.957 (3.0%) | 2.720 | 1.9% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 65.804 (1.7%) | 116.173 (2.9%) | 481 | 43.3% | — |
| read | 5 | 125.620 (2.5%) | 590.104 (5.2%) | 481 | 78.7% | — |
| update_inplace | 5 | 93.162 (0.9%) | 102.146 (0.9%) | 954 | 8.8% | — |
| update_grow | 5 | 79.808 (3.5%) | 87.728 (3.6%) | 1.721 | 9.0% | — |
| update_shrink | 5 | 70.668 (4.5%) | 75.023 (4.6%) | 2.656 | 5.8% | — |
| delete | 5 | 209.394 (3.5%) | 214.334 (3.5%) | 2.720 | 2.3% | — |

## @build/ab-pre155/modb_load.exe load.read_hotspot.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 62.556 (9.4%) | 108.035 (13.1%) | 481 | 41.8% | — |
| read_hotspot | 5 | 42.214 (3.4%) | 97.440 (6.1%) | 160 | 56.6% | — |

## @build/relwithdebinfo/modb_load.exe load.read_hotspot.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 65.237 (3.9%) | 115.138 (4.8%) | 481 | 43.3% | — |
| read_hotspot | 5 | 43.770 (11.3%) | 111.891 (13.9%) | 160 | 60.8% | — |

## @build/ab-pre155/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 63.817 (8.4%) | 115.934 (10.3%) | 483 | 44.9% | — |
| mixed_oltp | 5 | 17.074 (7.4%) | 17.109 (7.4%) | 2.687 | 0.2% | — |

## @build/relwithdebinfo/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 66.946 (4.8%) | 122.166 (5.1%) | 483 | 45.2% | — |
| mixed_oltp | 5 | 18.498 (3.4%) | 18.540 (3.4%) | 2.687 | 0.2% | — |

