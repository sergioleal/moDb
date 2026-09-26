# T12 cascade/blob

## load.cascade_delete.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create_hierarchy | 3 | 185.960 (0.5%) | — | 0 | 0.0% | — |
| cascade_delete | 3 | 517.397 (5.5%) | — | 0 | 0.0% | — |

## load.blob_lifecycle.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 18 (1.4%) | — | 0 | 0.0% | — |
| read | 3 | 73 (1.3%) | — | 0 | 0.0% | — |
| update_grow | 3 | 12 (5.5%) | — | 0 | 0.0% | — |
| update_shrink | 3 | 17 (8.4%) | — | 0 | 0.0% | — |
| delete | 3 | 31 (6.4%) | — | 0 | 0.0% | — |

