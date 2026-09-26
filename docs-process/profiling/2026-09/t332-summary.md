# T33.2 A/B sem GC

## @C:/Users/SERGIO~1.FON/AppData/Local/Temp/claude/C--tmp-apps-cpp-moDb2/461740ed-dd74-4d2f-bb07-a0b6eb13fca1/scratchpad/wt-332/build/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 75.530 (1.2%) | 141.762 (2.4%) | 481 | 46.7% | — |
| read | 5 | 140.682 (1.8%) | 873.421 (0.4%) | 481 | 83.9% | — |
| update_inplace | 5 | 124.964 (2.5%) | 140.210 (2.7%) | 954 | 10.9% | — |
| update_grow | 5 | 104.545 (3.3%) | 117.331 (3.7%) | 1.721 | 10.9% | — |
| update_shrink | 5 | 95.778 (1.8%) | 103.192 (1.8%) | 2.656 | 7.2% | — |
| delete | 5 | 300.346 (1.8%) | 309.598 (1.8%) | 2.720 | 3.0% | — |

## @build/relwithdebinfo/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 75.674 (1.2%) | 142.550 (1.1%) | 481 | 46.9% | — |
| read | 5 | 140.976 (1.1%) | 852.489 (2.7%) | 481 | 83.5% | — |
| update_inplace | 5 | 124.414 (7.1%) | 139.755 (7.8%) | 954 | 10.9% | — |
| update_grow | 5 | 107.185 (1.4%) | 120.848 (1.5%) | 1.721 | 11.3% | — |
| update_shrink | 5 | 95.247 (1.2%) | 102.688 (1.3%) | 2.656 | 7.2% | — |
| delete | 5 | 298.373 (1.2%) | 307.320 (1.2%) | 2.720 | 2.9% | — |

## @C:/Users/SERGIO~1.FON/AppData/Local/Temp/claude/C--tmp-apps-cpp-moDb2/461740ed-dd74-4d2f-bb07-a0b6eb13fca1/scratchpad/wt-332/build/modb_load.exe load.create_delete_interleaved.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 72.780 (2.3%) | 134.038 (3.1%) | 481 | 45.7% | — |
| delete | 5 | 223.178 (3.1%) | 228.568 (3.1%) | 837 | 2.4% | — |

## @build/relwithdebinfo/modb_load.exe load.create_delete_interleaved.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 72.849 (0.1%) | 133.885 (0.3%) | 481 | 45.6% | — |
| delete | 5 | 218.300 (1.5%) | 223.413 (1.5%) | 837 | 2.3% | — |

