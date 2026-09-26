# T7.1 stage-profile

## load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 2 | 69.715 (6.5%) | 127.348 (8.8%) | 483 | 45.2% | 30.2% |
| mixed_oltp | 2 | 20.264 (3.9%) | 20.311 (4.0%) | 2.687 | 0.2% | 90.7% |

### load.mixed_oltp.embedded.10k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 1.792 (19.4%) | 0.001 | 0.0 | 2.520.000 |
| `object_encode` | folha | 945 (2.7%) | 1.000 | 357.5 | 13.000 |
| `wal_append` | folha | 815 (16.9%) | 0.060 | 480.4 | 182.500 |
| `persist_root` | folha | 617 (2.7%) | 1.000 | 0.0 | 9.000 |
| `wal_sync` | folha | 572 (15.3%) | 0.001 | 0.0 | 944.300 |
| `buffer_pool_hit` | folha | 417 (2.8%) | 3.047 | 24965.1 | 7.900 |
| `buffer_pool_writeback` | folha | 390 (30.0%) | 0.001 | 478.4 | 667.100 |
| `heap_page_write` | folha | 315 (9.4%) | 1.048 | 8581.9 | 62.600 |
| `object_bind` | folha | 221 (3.4%) | 1.000 | 7.0 | 9.000 |
| `heap_candidate_scan` | folha | 48 (12.9%) | 1.000 | 1.0 | 10.600 |

### load.mixed_oltp.embedded.10k · mixed_oltp — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 42.507 (3.5%) | 0.089 | 0.0 | 7.540.100 |
| `wal_sync` | folha | 28.064 (1.5%) | 0.089 | 0.0 | 2.240.500 |
| `wal_append` | folha | 6.973 (11.0%) | 0.492 | 2590.4 | 6.794.500 |
| `buffer_pool_writeback` | folha | 4.121 (4.9%) | 0.089 | 2573.4 | 419.200 |
| `page_file_sync` | folha | 2.501 (4.0%) | 0.003 | 0.0 | 3.982.300 |
| `buffer_pool_hit` | folha | 1.841 (12.9%) | 3.313 | 27137.1 | 20.500 |
| `heap_record_read` | envelope | 1.781 (8.9%) | 1.022 | 381.8 | 238.400 |
| `identity_lookup` | envelope | 1.446 (10.6%) | 1.022 | 1.0 | 40.200 |
| `object_decode` | folha | 553 (2.9%) | 0.978 | 365.3 | 19.900 |
| `materialize` | folha | 412 (2.7%) | 0.911 | 6.4 | 20.000 |
| `object_encode` | folha | 135 (5.9%) | 0.067 | 23.9 | 14.700 |
| `persist_root` | folha | 79 (6.0%) | 0.067 | 0.0 | 15.400 |
| `object_bind` | folha | 58 (4.9%) | 0.067 | 0.5 | 10.400 |
| `heap_page_write` | folha | 38 (3.9%) | 0.070 | 571.3 | 15.500 |
| `heap_candidate_scan` | folha | 31 (6.9%) | 0.067 | 0.1 | 44.400 |

## load.snapshot_hold.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 2 | 64.948 (5.7%) | 114.162 (10.0%) | 483 | 43.0% | 33.4% |
| hold | 2 | 2.677 (0.7%) | 3.181 (0.1%) | 0 | 15.8% | 92.0% |

### load.snapshot_hold.embedded.10k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.575 (33.5%) | 0.001 | 0.0 | 10.151.400 |
| `wal_append` | folha | 1.281 (46.0%) | 0.060 | 480.4 | 8.205.400 |
| `object_encode` | folha | 963 (2.2%) | 1.000 | 357.5 | 66.200 |
| `wal_sync` | folha | 775 (35.9%) | 0.001 | 0.0 | 2.318.200 |
| `persist_root` | folha | 621 (0.9%) | 1.000 | 0.0 | 12.500 |
| `buffer_pool_writeback` | folha | 500 (1.4%) | 0.001 | 478.4 | 668.500 |
| `buffer_pool_hit` | folha | 422 (0.6%) | 3.047 | 24965.1 | 8.200 |
| `heap_page_write` | folha | 332 (0.9%) | 1.048 | 8581.9 | 66.200 |
| `object_bind` | folha | 232 (0.2%) | 1.000 | 7.0 | 21.400 |
| `heap_candidate_scan` | folha | 48 (2.8%) | 1.000 | 1.0 | 6.400 |

### load.snapshot_hold.embedded.10k · hold — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 345.010 (0.7%) | 0.767 | 0.0 | 5.438.200 |
| `wal_sync` | folha | 242.264 (0.9%) | 0.767 | 0.0 | 5.251.800 |
| `wal_append` | folha | 56.398 (0.4%) | 3.954 | 19969.5 | 2.117.900 |
| `buffer_pool_writeback` | folha | 30.601 (0.5%) | 0.767 | 19832.8 | 371.700 |
| `page_file_sync` | folha | 8.538 (1.5%) | 0.024 | 0.0 | 1.785.100 |
| `identity_lookup` | envelope | 2.143 (0.8%) | 2.000 | 2.0 | 31.200 |
| `heap_record_read` | envelope | 2.018 (0.3%) | 2.000 | 746.8 | 23.600 |
| `buffer_pool_hit` | folha | 1.939 (2.0%) | 9.320 | 76349.4 | 41.600 |
| `object_decode` | folha | 1.344 (0.4%) | 1.667 | 622.4 | 42.200 |
| `object_encode` | folha | 881 (0.4%) | 0.433 | 155.0 | 34.100 |
| `persist_root` | folha | 463 (0.6%) | 0.433 | 0.0 | 15.400 |
| `object_bind` | folha | 456 (2.6%) | 0.433 | 3.0 | 83.600 |
| `materialize` | folha | 290 (8.2%) | 1.000 | 7.0 | 218.400 |
| `heap_page_write` | folha | 221 (1.2%) | 0.454 | 3718.3 | 7.900 |
| `heap_candidate_scan` | folha | 176 (5.4%) | 0.433 | 0.4 | 4.900 |

