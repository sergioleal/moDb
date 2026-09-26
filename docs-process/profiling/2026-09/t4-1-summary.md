# T4.1 mixed_oltp 100k 4:1 vs 10:1

## load.mixed_oltp.embedded.100k --reads-per-write 4

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 63.976 (6.2%) | 170.678 (5.0%) | 481 | 62.5% | 38.3% |
| mixed_oltp | 3 | 3.066 (12.6%) | 3.066 (12.6%) | 5.934 | 0.0% | 95.0% |

### load.mixed_oltp.embedded.100k --reads-per-write 4 · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 3.532 (12.5%) | 0.001 | 0.0 | 5.672.800 |
| `wal_append` | folha | 1.408 (8.3%) | 0.060 | 481.2 | 276.900 |
| `object_encode` | folha | 935 (2.2%) | 1.000 | 359.5 | 162.000 |
| `page_file_sync` | folha | 835 (10.2%) | 0.002 | 0.0 | 2.208.100 |
| `wal_sync` | folha | 809 (14.4%) | 0.002 | 0.0 | 1.298.000 |
| `persist_root` | folha | 620 (1.9%) | 1.000 | 0.0 | 73.300 |
| `buffer_pool_writeback` | folha | 449 (25.9%) | 0.001 | 479.1 | 901.600 |
| `buffer_pool_hit` | folha | 415 (2.4%) | 3.048 | 24965.9 | 57.500 |
| `heap_page_write` | folha | 260 (7.7%) | 1.048 | 8582.0 | 137.400 |
| `object_bind` | folha | 219 (2.3%) | 1.000 | 7.0 | 32.300 |
| `heap_candidate_scan` | folha | 60 (3.4%) | 1.000 | 1.0 | 8.500 |

### load.mixed_oltp.embedded.100k --reads-per-write 4 · mixed_oltp — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 286.154 (11.1%) | 0.200 | 0.0 | 34.630.100 |
| `page_file_sync` | folha | 125.173 (11.2%) | 0.401 | 0.0 | 11.785.600 |
| `wal_sync` | folha | 119.231 (8.8%) | 0.401 | 0.0 | 32.869.700 |
| `buffer_pool_miss` | folha | 27.903 (30.4%) | 1.316 | 43121.1 | 1.376.800 |
| `wal_append` | folha | 25.832 (12.5%) | 1.109 | 5838.1 | 8.873.900 |
| `identity_lookup` | envelope | 17.207 (30.6%) | 1.050 | 1.1 | 1.391.500 |
| `heap_record_read` | envelope | 13.808 (29.6%) | 1.050 | 394.6 | 1.154.200 |
| `buffer_pool_writeback` | folha | 11.588 (25.7%) | 0.200 | 5799.9 | 1.191.800 |
| `object_decode` | folha | 857 (21.3%) | 0.950 | 357.0 | 231.700 |
| `buffer_pool_hit` | folha | 837 (32.9%) | 2.392 | 19597.7 | 102.600 |
| `materialize` | folha | 656 (25.2%) | 0.800 | 5.6 | 302.900 |
| `object_encode` | folha | 385 (27.1%) | 0.150 | 54.1 | 119.300 |
| `persist_root` | folha | 292 (24.3%) | 0.150 | 0.0 | 276.400 |
| `object_bind` | folha | 201 (27.8%) | 0.150 | 1.1 | 85.600 |
| `heap_page_write` | folha | 171 (25.7%) | 0.157 | 1287.4 | 100.900 |
| `heap_candidate_scan` | folha | 118 (43.0%) | 0.150 | 0.1 | 77.800 |

## load.mixed_oltp.embedded.100k --reads-per-write 10

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 59.754 (7.5%) | 162.716 (9.4%) | 481 | 63.2% | 39.6% |
| mixed_oltp | 3 | 5.868 (12.6%) | 5.870 (12.6%) | 2.723 | 0.0% | 94.2% |

### load.mixed_oltp.embedded.100k --reads-per-write 10 · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 4.087 (13.6%) | 0.001 | 0.0 | 25.944.300 |
| `wal_append` | folha | 1.452 (8.0%) | 0.060 | 481.2 | 1.408.900 |
| `wal_sync` | folha | 1.048 (18.9%) | 0.002 | 0.0 | 12.788.500 |
| `page_file_sync` | folha | 1.032 (17.8%) | 0.002 | 0.0 | 10.165.300 |
| `object_encode` | folha | 954 (6.7%) | 1.000 | 359.5 | 59.700 |
| `persist_root` | folha | 652 (8.6%) | 1.000 | 0.0 | 37.800 |
| `buffer_pool_writeback` | folha | 517 (26.8%) | 0.001 | 479.1 | 1.473.600 |
| `buffer_pool_hit` | folha | 434 (7.8%) | 3.048 | 24965.9 | 106.500 |
| `heap_page_write` | folha | 270 (11.1%) | 1.048 | 8582.0 | 78.300 |
| `object_bind` | folha | 231 (9.2%) | 1.000 | 7.0 | 38.700 |
| `heap_candidate_scan` | folha | 63 (8.3%) | 1.000 | 1.0 | 14.400 |

### load.mixed_oltp.embedded.100k --reads-per-write 10 · mixed_oltp — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 134.264 (9.2%) | 0.090 | 0.0 | 38.767.100 |
| `page_file_sync` | folha | 58.337 (8.5%) | 0.180 | 0.0 | 15.335.000 |
| `wal_sync` | folha | 55.567 (7.1%) | 0.180 | 0.0 | 20.732.300 |
| `buffer_pool_miss` | folha | 27.132 (23.6%) | 1.280 | 41949.4 | 1.552.600 |
| `identity_lookup` | envelope | 16.730 (23.9%) | 1.023 | 1.0 | 1.567.700 |
| `heap_record_read` | envelope | 13.401 (23.0%) | 1.023 | 384.1 | 1.326.700 |
| `wal_append` | folha | 12.151 (11.4%) | 0.499 | 2626.8 | 6.119.900 |
| `buffer_pool_writeback` | folha | 6.097 (26.4%) | 0.090 | 2609.6 | 1.334.400 |
| `buffer_pool_hit` | folha | 818 (26.2%) | 2.039 | 16701.0 | 299.300 |
| `object_decode` | folha | 754 (16.6%) | 0.978 | 367.2 | 363.400 |
| `materialize` | folha | 607 (20.2%) | 0.910 | 6.4 | 375.500 |
| `object_encode` | folha | 200 (25.1%) | 0.068 | 24.3 | 86.300 |
| `persist_root` | folha | 146 (20.2%) | 0.068 | 0.0 | 106.000 |
| `object_bind` | folha | 104 (25.3%) | 0.068 | 0.5 | 72.300 |
| `heap_page_write` | folha | 85 (22.4%) | 0.071 | 579.6 | 64.600 |
| `heap_candidate_scan` | folha | 70 (40.9%) | 0.068 | 0.1 | 80.200 |

