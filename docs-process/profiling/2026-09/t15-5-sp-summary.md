# T15.5 stage-profile

## load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 2 | 61.640 (1.3%) | 105.060 (0.4%) | 481 | 41.3% | 32.8% |
| read | 2 | 120.432 (2.5%) | 484.532 (4.0%) | 481 | 75.1% | 15.1% |
| update_inplace | 2 | 77.848 (14.0%) | 84.479 (13.9%) | 954 | 7.9% | 54.0% |
| update_grow | 2 | 72.364 (0.1%) | 78.703 (0.1%) | 1.721 | 8.1% | 54.5% |
| update_shrink | 2 | 65.264 (0.6%) | 68.842 (0.5%) | 2.656 | 5.2% | 67.9% |
| delete | 2 | 192.526 (5.7%) | 196.646 (5.7%) | 2.720 | 2.1% | 65.2% |

### load.crud_full.embedded.100k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.563 (3.3%) | 0.001 | 0.0 | 14.625.500 |
| `wal_append` | folha | 1.047 (8.3%) | 0.060 | 481.2 | 479.600 |
| `object_encode` | folha | 1.016 (4.0%) | 1.000 | 359.5 | 57.800 |
| `wal_sync` | folha | 703 (3.7%) | 0.001 | 0.0 | 4.565.800 |
| `persist_root` | folha | 689 (2.5%) | 1.000 | 0.0 | 34.200 |
| `buffer_pool_writeback` | folha | 669 (3.6%) | 0.001 | 479.1 | 1.219.800 |
| `buffer_pool_hit` | folha | 464 (2.9%) | 3.048 | 24965.9 | 70.100 |
| `heap_page_write` | folha | 300 (0.2%) | 1.048 | 8582.0 | 94.400 |
| `object_bind` | folha | 248 (3.7%) | 1.000 | 7.0 | 24.000 |
| `page_file_sync` | folha | 117 (0.4%) | 0.000 | 0.0 | 11.439.600 |
| `heap_candidate_scan` | folha | 68 (6.6%) | 1.000 | 1.0 | 19.700 |

### load.crud_full.embedded.100k · read — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `identity_lookup` | envelope | 547 (4.8%) | 1.000 | 1.0 | 291.600 |
| `heap_record_read` | envelope | 490 (6.8%) | 1.000 | 375.5 | 389.400 |
| `object_decode` | folha | 406 (2.5%) | 1.000 | 375.5 | 70.200 |
| `buffer_pool_miss` | folha | 356 (7.8%) | 0.013 | 441.1 | 278.000 |
| `materialize` | folha | 289 (3.6%) | 1.000 | 7.0 | 49.000 |
| `buffer_pool_hit` | folha | 206 (4.2%) | 2.987 | 8192.0 | 386.400 |

### load.crud_full.embedded.100k · update_inplace — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.847 (7.5%) | 0.001 | 0.0 | 32.314.800 |
| `identity_lookup` | envelope | 1.086 (14.4%) | 2.000 | 2.0 | 131.200 |
| `object_encode` | folha | 1.078 (17.4%) | 1.000 | 359.5 | 115.900 |
| `wal_append` | folha | 1.073 (8.9%) | 0.059 | 473.0 | 371.500 |
| `buffer_pool_hit` | folha | 918 (14.7%) | 9.034 | 41350.1 | 65.900 |
| `persist_root` | folha | 740 (17.6%) | 1.000 | 0.0 | 60.300 |
| `heap_record_read` | envelope | 721 (14.6%) | 2.000 | 750.9 | 193.500 |
| `wal_sync` | folha | 665 (2.7%) | 0.001 | 0.0 | 2.718.100 |
| `buffer_pool_writeback` | folha | 656 (4.8%) | 0.001 | 471.0 | 1.012.400 |
| `object_decode` | folha | 447 (18.5%) | 1.000 | 375.5 | 37.400 |
| `page_file_sync` | folha | 424 (14.5%) | 0.000 | 0.0 | 29.294.700 |
| `buffer_pool_miss` | folha | 357 (12.6%) | 0.013 | 441.1 | 192.400 |
| `heap_page_write` | folha | 295 (11.1%) | 1.048 | 8582.1 | 25.500 |
| `object_bind` | folha | 252 (19.3%) | 1.000 | 7.0 | 29.800 |
| `heap_candidate_scan` | folha | 91 (9.2%) | 1.000 | 1.0 | 16.700 |

### load.crud_full.embedded.100k · update_grow — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 3.591 (1.0%) | 0.001 | 0.0 | 31.744.500 |
| `wal_append` | folha | 1.583 (1.6%) | 0.095 | 766.8 | 630.400 |
| `identity_lookup` | envelope | 1.103 (3.1%) | 2.000 | 2.0 | 133.600 |
| `object_encode` | folha | 994 (1.3%) | 1.000 | 615.5 | 102.800 |
| `buffer_pool_writeback` | folha | 912 (2.4%) | 0.001 | 763.7 | 1.822.600 |
| `buffer_pool_hit` | folha | 833 (1.4%) | 9.066 | 41642.7 | 58.700 |
| `wal_sync` | folha | 777 (0.5%) | 0.001 | 0.0 | 2.430.400 |
| `heap_record_read` | envelope | 657 (0.9%) | 2.000 | 750.9 | 225.500 |
| `persist_root` | folha | 657 (1.6%) | 1.000 | 0.0 | 59.800 |
| `buffer_pool_miss` | folha | 429 (0.2%) | 0.018 | 582.3 | 225.000 |
| `object_decode` | folha | 404 (3.6%) | 1.000 | 375.5 | 35.100 |
| `heap_page_write` | folha | 324 (1.0%) | 1.083 | 8874.7 | 56.700 |
| `page_file_sync` | folha | 285 (2.3%) | 0.000 | 0.0 | 28.464.700 |
| `object_bind` | folha | 241 (1.6%) | 1.000 | 7.0 | 32.400 |
| `heap_candidate_scan` | folha | 91 (0.8%) | 1.000 | 0.9 | 28.200 |

### load.crud_full.embedded.100k · update_shrink — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 4.621 (2.1%) | 0.001 | 0.0 | 40.449.400 |
| `heap_candidate_try` | envelope | 2.256 (5.5%) | 0.167 | 0.0 | 501.300 |
| `buffer_pool_miss` | folha | 2.236 (2.5%) | 0.099 | 3235.0 | 859.600 |
| `wal_append` | folha | 1.840 (0.4%) | 0.116 | 934.4 | 408.300 |
| `buffer_pool_writeback` | folha | 1.318 (1.4%) | 0.001 | 930.5 | 7.277.700 |
| `identity_lookup` | envelope | 1.104 (0.6%) | 2.000 | 2.0 | 860.400 |
| `object_encode` | folha | 987 (1.1%) | 1.000 | 167.5 | 82.000 |
| `wal_sync` | folha | 861 (6.8%) | 0.001 | 0.0 | 3.801.800 |
| `heap_record_read` | envelope | 853 (3.0%) | 2.000 | 1262.9 | 377.000 |
| `buffer_pool_hit` | folha | 821 (0.3%) | 8.921 | 40531.5 | 84.400 |
| `persist_root` | folha | 667 (0.1%) | 1.000 | 0.0 | 453.400 |
| `page_file_sync` | folha | 553 (1.6%) | 0.000 | 0.0 | 38.329.700 |
| `object_decode` | folha | 431 (1.2%) | 1.000 | 631.5 | 25.800 |
| `heap_page_write` | folha | 374 (3.2%) | 1.020 | 8354.4 | 86.100 |
| `object_bind` | folha | 232 (1.5%) | 1.000 | 7.0 | 33.000 |
| `heap_candidate_scan` | folha | 91 (1.6%) | 1.000 | 1.0 | 26.000 |

### load.crud_full.embedded.100k · delete — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `heap_record_read` | envelope | 1.663 (12.6%) | 1.000 | 183.5 | 189.100 |
| `buffer_pool_miss` | folha | 1.571 (12.1%) | 0.081 | 2666.9 | 772.500 |
| `tx_commit` | envelope | 938 (5.5%) | 0.001 | 0.0 | 13.798.700 |
| `identity_lookup` | envelope | 628 (1.5%) | 1.000 | 1.0 | 774.200 |
| `buffer_pool_hit` | folha | 488 (0.8%) | 4.919 | 24576.0 | 43.100 |
| `wal_sync` | folha | 451 (5.9%) | 0.001 | 0.0 | 914.600 |
| `object_decode` | folha | 412 (0.1%) | 1.000 | 183.5 | 71.800 |
| `wal_append` | folha | 171 (4.2%) | 0.010 | 64.9 | 314.800 |
| `buffer_pool_writeback` | folha | 169 (12.4%) | 0.001 | 64.6 | 939.600 |
| `page_file_sync` | folha | 130 (3.2%) | 0.000 | 0.0 | 12.973.600 |

