# P1 revisado (sem power throttling)

## @C:/Users/SERGIO~1.FON/AppData/Local/Temp/claude/C--tmp-apps-cpp-moDb2/461740ed-dd74-4d2f-bb07-a0b6eb13fca1/scratchpad/wt-pre/build/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 58.227 (9.6%) | 157.050 (12.2%) | 483 | 62.8% | — |
| mixed_oltp | 5 | 6.636 (6.0%) | 6.641 (6.0%) | 2.687 | 0.1% | — |

## @build/relwithdebinfo/modb_load.exe load.mixed_oltp.embedded.10k --checkpoint-interval 1

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 51.153 (28.5%) | 86.770 (30.2%) | 483 | 40.8% | — |
| mixed_oltp | 5 | 7.706 (6.7%) | 7.715 (6.7%) | 2.687 | 0.1% | — |

## @build/relwithdebinfo/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 59.368 (29.4%) | 107.284 (31.0%) | 483 | 44.3% | — |
| mixed_oltp | 5 | 16.205 (27.6%) | 16.241 (27.6%) | 2.687 | 0.2% | — |

## @C:/Users/SERGIO~1.FON/AppData/Local/Temp/claude/C--tmp-apps-cpp-moDb2/461740ed-dd74-4d2f-bb07-a0b6eb13fca1/scratchpad/wt-pre/build/modb_load.exe load.snapshot_hold.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 45.081 (24.3%) | 112.178 (29.0%) | 483 | 59.2% | — |
| hold | 5 | 735 (16.5%) | 851 (16.6%) | 0 | 13.7% | — |

## @build/relwithdebinfo/modb_load.exe load.snapshot_hold.embedded.10k --checkpoint-interval 1

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 61.272 (13.8%) | 105.405 (19.5%) | 483 | 41.3% | — |
| hold | 5 | 902 (41.1%) | 1.084 (40.5%) | 0 | 16.7% | — |

## @build/relwithdebinfo/modb_load.exe load.snapshot_hold.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 68.917 (10.2%) | 127.213 (12.6%) | 483 | 45.7% | — |
| hold | 5 | 2.572 (4.5%) | 3.052 (3.9%) | 0 | 15.7% | — |

## @C:/Users/SERGIO~1.FON/AppData/Local/Temp/claude/C--tmp-apps-cpp-moDb2/461740ed-dd74-4d2f-bb07-a0b6eb13fca1/scratchpad/wt-pre/build/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 60.719 (4.1%) | 166.147 (5.6%) | 481 | 63.4% | — |
| read | 5 | 107.158 (19.5%) | 408.100 (15.4%) | 481 | 73.5% | — |
| update_inplace | 5 | 78.013 (5.3%) | 123.667 (5.0%) | 954 | 36.9% | — |
| update_grow | 5 | 63.089 (6.5%) | 107.250 (6.1%) | 1.721 | 41.2% | — |
| update_shrink | 5 | 50.379 (8.6%) | 96.652 (7.6%) | 2.656 | 47.9% | — |
| delete | 5 | 156.905 (12.1%) | 232.200 (10.9%) | 2.720 | 32.5% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k --checkpoint-interval 1

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 65.487 (8.5%) | 112.663 (11.1%) | 481 | 41.7% | — |
| read | 5 | 116.066 (4.7%) | 410.223 (7.2%) | 481 | 71.7% | — |
| update_inplace | 5 | 78.070 (5.1%) | 84.039 (5.3%) | 954 | 7.1% | — |
| update_grow | 5 | 69.319 (7.7%) | 74.993 (8.0%) | 1.721 | 7.6% | — |
| update_shrink | 5 | 58.371 (5.6%) | 61.164 (5.8%) | 2.656 | 4.6% | — |
| delete | 5 | 169.117 (4.6%) | 172.077 (4.6%) | 2.720 | 1.7% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 70.524 (2.4%) | 127.361 (3.4%) | 481 | 44.6% | — |
| read | 5 | 119.072 (2.3%) | 424.473 (4.4%) | 481 | 71.9% | — |
| update_inplace | 5 | 83.039 (5.6%) | 89.755 (5.8%) | 954 | 7.5% | — |
| update_grow | 5 | 69.627 (8.9%) | 75.554 (9.2%) | 1.721 | 7.8% | — |
| update_shrink | 5 | 67.393 (5.2%) | 71.167 (5.3%) | 2.656 | 5.3% | — |
| delete | 5 | 196.399 (6.0%) | 200.364 (6.1%) | 2.720 | 2.0% | — |

