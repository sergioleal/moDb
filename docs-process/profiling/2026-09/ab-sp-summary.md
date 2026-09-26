# T9 stage-profile

## load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 2 | 70.137 (3.5%) | 124.026 (5.2%) | 483 | 43.4% | 33.3% |
| mixed_oltp | 2 | 21.188 (2.5%) | 21.232 (2.5%) | 2.687 | 0.2% | 91.6% |

### load.mixed_oltp.embedded.10k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.246 (6.6%) | 0.001 | 0.0 | 2.649.200 |
| `wal_append` | folha | 1.334 (2.7%) | 0.060 | 480.4 | 150.900 |
| `object_encode` | folha | 925 (1.3%) | 1.000 | 357.5 | 19.300 |
| `persist_root` | folha | 606 (1.7%) | 1.000 | 0.0 | 9.300 |
| `wal_sync` | folha | 538 (8.9%) | 0.001 | 0.0 | 712.800 |
| `buffer_pool_hit` | folha | 411 (1.7%) | 3.047 | 24965.1 | 7.700 |
| `buffer_pool_writeback` | folha | 360 (16.7%) | 0.001 | 478.4 | 543.300 |
| `heap_page_write` | folha | 310 (2.3%) | 1.048 | 8581.9 | 52.400 |
| `object_bind` | folha | 222 (2.5%) | 1.000 | 7.0 | 17.800 |
| `heap_candidate_scan` | folha | 44 (0.5%) | 1.000 | 1.0 | 6.000 |

### load.mixed_oltp.embedded.10k · mixed_oltp — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 41.524 (2.3%) | 0.089 | 0.0 | 6.627.100 |
| `wal_sync` | folha | 26.059 (1.6%) | 0.089 | 0.0 | 6.371.600 |
| `wal_append` | folha | 9.093 (3.5%) | 0.492 | 2590.4 | 305.600 |
| `buffer_pool_writeback` | folha | 3.534 (4.1%) | 0.089 | 2573.4 | 363.800 |
| `page_file_sync` | folha | 2.113 (3.8%) | 0.003 | 0.0 | 3.655.400 |
| `heap_record_read` | envelope | 1.295 (4.6%) | 1.022 | 381.8 | 49.100 |
| `buffer_pool_hit` | folha | 1.288 (7.5%) | 3.313 | 27137.1 | 43.400 |
| `identity_lookup` | envelope | 1.182 (6.9%) | 1.022 | 1.0 | 44.900 |
| `object_decode` | folha | 510 (1.8%) | 0.978 | 365.3 | 18.200 |
| `materialize` | folha | 362 (2.4%) | 0.911 | 6.4 | 13.000 |
| `object_encode` | folha | 119 (6.0%) | 0.067 | 23.9 | 13.200 |
| `persist_root` | folha | 68 (4.9%) | 0.067 | 0.0 | 11.600 |
| `object_bind` | folha | 52 (4.1%) | 0.067 | 0.5 | 8.500 |
| `heap_page_write` | folha | 34 (3.9%) | 0.070 | 571.3 | 5.800 |
| `heap_candidate_scan` | folha | 22 (3.7%) | 0.067 | 0.1 | 7.500 |

## load.snapshot_hold.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 2 | 67.142 (6.6%) | 117.526 (9.3%) | 483 | 42.8% | 33.5% |
| hold | 2 | 2.725 (0.7%) | 3.242 (0.8%) | 0 | 15.9% | 92.5% |

### load.snapshot_hold.embedded.10k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.394 (11.7%) | 0.001 | 0.0 | 3.289.400 |
| `wal_append` | folha | 1.446 (12.9%) | 0.060 | 480.4 | 239.100 |
| `object_encode` | folha | 968 (5.0%) | 1.000 | 357.5 | 24.600 |
| `persist_root` | folha | 627 (4.0%) | 1.000 | 0.0 | 31.300 |
| `wal_sync` | folha | 523 (11.6%) | 0.001 | 0.0 | 682.700 |
| `buffer_pool_hit` | folha | 427 (4.2%) | 3.047 | 24965.1 | 10.000 |
| `buffer_pool_writeback` | folha | 406 (7.1%) | 0.001 | 478.4 | 568.600 |
| `heap_page_write` | folha | 327 (6.8%) | 1.048 | 8581.9 | 55.500 |
| `object_bind` | folha | 237 (3.1%) | 1.000 | 7.0 | 12.500 |
| `heap_candidate_scan` | folha | 47 (0.7%) | 1.000 | 1.0 | 6.500 |

### load.snapshot_hold.embedded.10k · hold — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 340.216 (0.7%) | 0.767 | 0.0 | 2.660.200 |
| `wal_sync` | folha | 224.233 (3.1%) | 0.767 | 0.0 | 1.899.600 |
| `wal_append` | folha | 74.188 (5.5%) | 3.954 | 19969.5 | 583.100 |
| `buffer_pool_writeback` | folha | 27.731 (0.9%) | 0.767 | 19832.8 | 341.200 |
| `page_file_sync` | folha | 7.843 (1.3%) | 0.024 | 0.0 | 1.975.300 |
| `identity_lookup` | envelope | 1.987 (1.1%) | 2.000 | 2.0 | 58.800 |
| `heap_record_read` | envelope | 1.773 (1.7%) | 2.000 | 746.8 | 62.600 |
| `buffer_pool_hit` | folha | 1.720 (0.1%) | 9.320 | 76349.4 | 41.000 |
| `object_decode` | folha | 1.273 (0.1%) | 1.667 | 622.4 | 18.100 |
| `object_encode` | folha | 875 (2.5%) | 0.433 | 155.0 | 36.800 |
| `persist_root` | folha | 426 (0.4%) | 0.433 | 0.0 | 14.100 |
| `object_bind` | folha | 417 (3.3%) | 0.433 | 3.0 | 62.900 |
| `materialize` | folha | 270 (1.6%) | 1.000 | 7.0 | 15.800 |
| `heap_page_write` | folha | 208 (1.7%) | 0.454 | 3718.3 | 5.800 |
| `heap_candidate_scan` | folha | 172 (6.8%) | 0.433 | 0.4 | 7.500 |

