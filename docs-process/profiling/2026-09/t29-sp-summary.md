# T29 stage-profile

## @build/ab-pre29-sp/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 2 | 64.067 (5.2%) | 109.765 (6.9%) | 481 | 41.6% | 32.8% |
| read | 2 | 128.196 (3.1%) | 601.474 (6.3%) | 481 | 78.7% | 13.9% |
| update_inplace | 2 | 89.342 (1.8%) | 98.222 (2.4%) | 954 | 9.0% | 55.2% |
| update_grow | 2 | 81.068 (1.6%) | 89.083 (1.6%) | 1.721 | 9.0% | 55.1% |
| update_shrink | 2 | 68.219 (3.0%) | 72.188 (2.9%) | 2.656 | 5.5% | 69.9% |
| delete | 2 | 211.508 (8.7%) | 216.368 (8.8%) | 2.720 | 2.2% | 69.5% |

### @build/ab-pre29-sp/modb_load.exe load.crud_full.embedded.100k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.506 (15.8%) | 0.001 | 0.0 | 12.974.800 |
| `wal_append` | folha | 1.053 (15.6%) | 0.060 | 481.2 | 426.300 |
| `object_encode` | folha | 984 (3.3%) | 1.000 | 359.5 | 112.300 |
| `wal_sync` | folha | 692 (16.9%) | 0.001 | 0.0 | 1.484.100 |
| `persist_root` | folha | 658 (0.9%) | 1.000 | 0.0 | 235.700 |
| `buffer_pool_writeback` | folha | 640 (20.4%) | 0.001 | 479.1 | 1.450.600 |
| `buffer_pool_hit` | folha | 434 (1.2%) | 3.048 | 24965.9 | 39.100 |
| `heap_page_write` | folha | 284 (7.3%) | 1.048 | 8582.0 | 90.800 |
| `object_bind` | folha | 234 (3.8%) | 1.000 | 7.0 | 36.900 |
| `page_file_sync` | folha | 95 (21.6%) | 0.000 | 0.0 | 10.647.300 |
| `heap_candidate_scan` | folha | 65 (3.3%) | 1.000 | 1.0 | 18.200 |

### @build/ab-pre29-sp/modb_load.exe load.crud_full.embedded.100k · read — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `heap_record_read` | envelope | 454 (16.2%) | 1.000 | 375.5 | 177.000 |
| `object_decode` | folha | 391 (4.3%) | 1.000 | 375.5 | 23.300 |
| `buffer_pool_miss` | folha | 343 (21.6%) | 0.013 | 441.1 | 192.200 |
| `materialize` | folha | 275 (0.4%) | 1.000 | 7.0 | 45.500 |
| `identity_lookup` | envelope | 228 (4.9%) | 1.000 | 1.0 | 194.200 |
| `buffer_pool_hit` | folha | 79 (0.4%) | 2.987 | 0.0 | 122.600 |

### @build/ab-pre29-sp/modb_load.exe load.crud_full.embedded.100k · update_inplace — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.667 (4.0%) | 0.001 | 0.0 | 26.817.700 |
| `wal_append` | folha | 1.014 (4.4%) | 0.059 | 473.0 | 399.700 |
| `object_encode` | folha | 1.007 (4.5%) | 1.000 | 359.5 | 79.000 |
| `persist_root` | folha | 708 (8.8%) | 1.000 | 0.0 | 51.800 |
| `wal_sync` | folha | 685 (13.3%) | 0.001 | 0.0 | 2.378.700 |
| `heap_record_read` | envelope | 625 (2.0%) | 2.000 | 750.9 | 239.300 |
| `buffer_pool_writeback` | folha | 597 (4.5%) | 0.001 | 471.0 | 1.067.300 |
| `buffer_pool_hit` | folha | 492 (5.9%) | 9.034 | 16774.1 | 33.300 |
| `identity_lookup` | envelope | 403 (4.8%) | 2.000 | 2.0 | 147.900 |
| `object_decode` | folha | 400 (4.9%) | 1.000 | 375.5 | 52.000 |
| `page_file_sync` | folha | 344 (15.8%) | 0.000 | 0.0 | 23.904.400 |
| `buffer_pool_miss` | folha | 331 (0.3%) | 0.013 | 441.1 | 237.900 |
| `heap_page_write` | folha | 284 (1.3%) | 1.048 | 8582.1 | 26.900 |
| `object_bind` | folha | 232 (4.2%) | 1.000 | 7.0 | 89.000 |
| `heap_candidate_scan` | folha | 86 (3.4%) | 1.000 | 1.0 | 47.500 |

### @build/ab-pre29-sp/modb_load.exe load.crud_full.embedded.100k · update_grow — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 3.337 (1.6%) | 0.001 | 0.0 | 32.795.700 |
| `wal_append` | folha | 1.491 (0.5%) | 0.095 | 766.8 | 361.300 |
| `object_encode` | folha | 984 (0.6%) | 1.000 | 615.5 | 150.100 |
| `buffer_pool_writeback` | folha | 763 (1.9%) | 0.001 | 763.7 | 1.436.100 |
| `wal_sync` | folha | 760 (3.4%) | 0.001 | 0.0 | 2.512.300 |
| `persist_root` | folha | 666 (0.7%) | 1.000 | 0.0 | 62.300 |
| `heap_record_read` | envelope | 552 (5.6%) | 2.000 | 750.9 | 154.900 |
| `identity_lookup` | envelope | 478 (1.5%) | 2.000 | 2.0 | 182.300 |
| `buffer_pool_hit` | folha | 474 (0.2%) | 9.066 | 17066.7 | 41.000 |
| `object_decode` | folha | 387 (1.4%) | 1.000 | 375.5 | 38.800 |
| `buffer_pool_miss` | folha | 364 (8.8%) | 0.018 | 582.3 | 181.900 |
| `heap_page_write` | folha | 304 (3.9%) | 1.083 | 8874.7 | 101.000 |
| `page_file_sync` | folha | 294 (2.2%) | 0.000 | 0.0 | 29.631.800 |
| `object_bind` | folha | 226 (1.5%) | 1.000 | 7.0 | 22.700 |
| `heap_candidate_scan` | folha | 87 (2.5%) | 1.000 | 0.9 | 34.700 |

### @build/ab-pre29-sp/modb_load.exe load.crud_full.embedded.100k · update_shrink — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 4.883 (4.7%) | 0.001 | 0.0 | 52.043.200 |
| `buffer_pool_miss` | folha | 2.120 (0.9%) | 0.099 | 3235.0 | 856.200 |
| `heap_candidate_try` | envelope | 2.082 (2.3%) | 0.167 | 0.0 | 866.900 |
| `wal_append` | folha | 1.923 (1.9%) | 0.116 | 934.4 | 662.100 |
| `buffer_pool_writeback` | folha | 1.195 (1.3%) | 0.001 | 930.5 | 6.605.000 |
| `wal_sync` | folha | 1.026 (15.9%) | 0.001 | 0.0 | 5.155.400 |
| `object_encode` | folha | 1.001 (3.6%) | 1.000 | 167.5 | 366.700 |
| `heap_record_read` | envelope | 832 (4.5%) | 2.000 | 1262.9 | 429.600 |
| `page_file_sync` | folha | 691 (12.6%) | 0.000 | 0.0 | 49.972.300 |
| `persist_root` | folha | 681 (1.3%) | 1.000 | 0.0 | 62.600 |
| `identity_lookup` | envelope | 504 (1.4%) | 2.000 | 2.0 | 443.300 |
| `buffer_pool_hit` | folha | 470 (1.2%) | 8.921 | 15955.5 | 50.700 |
| `object_decode` | folha | 436 (2.2%) | 1.000 | 631.5 | 53.900 |
| `heap_page_write` | folha | 367 (2.1%) | 1.020 | 8354.4 | 134.400 |
| `object_bind` | folha | 241 (9.4%) | 1.000 | 7.0 | 54.800 |
| `heap_candidate_scan` | folha | 92 (2.7%) | 1.000 | 1.0 | 41.700 |

### @build/ab-pre29-sp/modb_load.exe load.crud_full.embedded.100k · delete — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `heap_record_read` | envelope | 1.614 (13.7%) | 1.000 | 183.5 | 248.300 |
| `buffer_pool_miss` | folha | 1.564 (13.4%) | 0.081 | 2666.9 | 612.000 |
| `tx_commit` | envelope | 1.082 (10.5%) | 0.001 | 0.0 | 16.935.200 |
| `wal_sync` | folha | 514 (17.1%) | 0.001 | 0.0 | 1.263.400 |
| `object_decode` | folha | 426 (12.3%) | 1.000 | 183.5 | 36.800 |
| `identity_lookup` | envelope | 343 (2.5%) | 1.000 | 1.0 | 613.900 |
| `buffer_pool_hit` | folha | 247 (0.9%) | 4.919 | 8192.0 | 32.900 |
| `wal_append` | folha | 211 (6.9%) | 0.010 | 64.9 | 312.100 |
| `buffer_pool_writeback` | folha | 181 (16.2%) | 0.001 | 64.6 | 429.100 |
| `page_file_sync` | folha | 157 (5.8%) | 0.000 | 0.0 | 16.037.500 |

## @build/stage-profile/modb_load.exe load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 2 | 65.299 (0.4%) | 112.475 (0.8%) | 481 | 41.9% | 32.8% |
| read | 2 | 124.324 (1.6%) | 573.516 (5.1%) | 481 | 78.3% | 14.3% |
| update_inplace | 2 | 93.282 (2.9%) | 102.215 (3.0%) | 954 | 8.7% | 56.2% |
| update_grow | 2 | 81.489 (1.0%) | 89.592 (1.1%) | 1.721 | 9.0% | 54.4% |
| update_shrink | 2 | 74.253 (4.2%) | 78.902 (4.4%) | 2.656 | 5.9% | 69.0% |
| delete | 2 | 219.608 (1.1%) | 224.686 (1.1%) | 2.720 | 2.3% | 68.8% |

### @build/stage-profile/modb_load.exe load.crud_full.embedded.100k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.532 (2.8%) | 0.001 | 0.0 | 15.971.200 |
| `wal_append` | folha | 1.021 (7.2%) | 0.060 | 481.2 | 367.100 |
| `object_encode` | folha | 979 (0.8%) | 1.000 | 359.5 | 47.400 |
| `wal_sync` | folha | 758 (15.5%) | 0.001 | 0.0 | 6.536.700 |
| `persist_root` | folha | 654 (0.5%) | 1.000 | 0.0 | 62.500 |
| `buffer_pool_writeback` | folha | 605 (3.8%) | 0.001 | 479.1 | 1.492.100 |
| `buffer_pool_hit` | folha | 307 (1.1%) | 3.048 | 16384.0 | 43.000 |
| `heap_page_write` | folha | 273 (0.9%) | 1.048 | 8582.0 | 63.600 |
| `object_bind` | folha | 235 (2.1%) | 1.000 | 7.0 | 107.700 |
| `page_file_sync` | folha | 122 (4.3%) | 0.000 | 0.0 | 12.131.000 |
| `heap_candidate_scan` | folha | 63 (2.0%) | 1.000 | 1.0 | 12.800 |

### @build/stage-profile/modb_load.exe load.crud_full.embedded.100k · read — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `heap_record_read` | envelope | 508 (7.4%) | 1.000 | 375.5 | 287.200 |
| `buffer_pool_miss` | folha | 399 (10.0%) | 0.013 | 441.1 | 567.600 |
| `object_decode` | folha | 387 (2.9%) | 1.000 | 375.5 | 64.600 |
| `materialize` | folha | 285 (4.7%) | 1.000 | 7.0 | 126.300 |
| `identity_lookup` | envelope | 241 (7.1%) | 1.000 | 1.0 | 593.400 |
| `buffer_pool_hit` | folha | 78 (0.9%) | 2.987 | 0.0 | 26.700 |

### @build/stage-profile/modb_load.exe load.crud_full.embedded.100k · update_inplace — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.828 (11.0%) | 0.001 | 0.0 | 24.981.800 |
| `wal_append` | folha | 1.017 (11.7%) | 0.059 | 473.0 | 2.289.200 |
| `object_encode` | folha | 954 (1.0%) | 1.000 | 359.5 | 93.200 |
| `wal_sync` | folha | 757 (20.0%) | 0.001 | 0.0 | 5.100.900 |
| `persist_root` | folha | 655 (1.1%) | 1.000 | 0.0 | 44.800 |
| `buffer_pool_writeback` | folha | 611 (8.2%) | 0.001 | 471.0 | 1.235.700 |
| `heap_record_read` | envelope | 599 (2.5%) | 2.000 | 750.9 | 225.400 |
| `page_file_sync` | folha | 417 (2.7%) | 0.000 | 0.0 | 22.100.100 |
| `identity_lookup` | envelope | 384 (1.0%) | 2.000 | 2.0 | 215.300 |
| `object_decode` | folha | 381 (0.9%) | 1.000 | 375.5 | 47.100 |
| `buffer_pool_hit` | folha | 345 (0.2%) | 9.034 | 8192.0 | 22.300 |
| `buffer_pool_miss` | folha | 325 (2.9%) | 0.013 | 441.1 | 225.000 |
| `heap_page_write` | folha | 268 (0.8%) | 1.048 | 8582.1 | 30.600 |
| `object_bind` | folha | 221 (1.4%) | 1.000 | 7.0 | 42.000 |
| `heap_candidate_scan` | folha | 81 (2.9%) | 1.000 | 1.0 | 25.300 |

### @build/stage-profile/modb_load.exe load.crud_full.embedded.100k · update_grow — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 3.347 (0.2%) | 0.001 | 0.0 | 33.875.700 |
| `wal_append` | folha | 1.496 (3.5%) | 0.095 | 766.8 | 507.800 |
| `object_encode` | folha | 986 (0.8%) | 1.000 | 615.5 | 77.900 |
| `buffer_pool_writeback` | folha | 789 (0.5%) | 0.001 | 763.7 | 1.578.900 |
| `wal_sync` | folha | 733 (3.7%) | 0.001 | 0.0 | 2.374.200 |
| `persist_root` | folha | 656 (1.5%) | 1.000 | 0.0 | 67.500 |
| `heap_record_read` | envelope | 549 (1.4%) | 2.000 | 750.9 | 123.600 |
| `identity_lookup` | envelope | 476 (0.1%) | 2.000 | 2.0 | 196.900 |
| `object_decode` | folha | 386 (0.1%) | 1.000 | 375.5 | 47.600 |
| `buffer_pool_miss` | folha | 365 (1.5%) | 0.018 | 582.3 | 196.200 |
| `buffer_pool_hit` | folha | 345 (0.3%) | 9.066 | 8192.0 | 67.900 |
| `page_file_sync` | folha | 301 (6.7%) | 0.000 | 0.0 | 31.140.400 |
| `heap_page_write` | folha | 297 (2.0%) | 1.083 | 8874.7 | 92.400 |
| `object_bind` | folha | 231 (0.5%) | 1.000 | 7.0 | 201.700 |
| `heap_candidate_scan` | folha | 88 (0.3%) | 1.000 | 0.9 | 10.900 |

### @build/stage-profile/modb_load.exe load.crud_full.embedded.100k · update_shrink — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 4.329 (5.1%) | 0.001 | 0.0 | 43.723.100 |
| `buffer_pool_miss` | folha | 1.968 (10.8%) | 0.099 | 3229.1 | 808.300 |
| `heap_candidate_try` | envelope | 1.948 (11.3%) | 0.167 | 0.0 | 502.400 |
| `wal_append` | folha | 1.780 (1.0%) | 0.116 | 934.4 | 472.700 |
| `buffer_pool_writeback` | folha | 1.116 (12.7%) | 0.001 | 930.5 | 6.950.600 |
| `object_encode` | folha | 961 (0.8%) | 1.000 | 167.5 | 70.600 |
| `wal_sync` | folha | 819 (5.6%) | 0.001 | 0.0 | 3.516.900 |
| `heap_record_read` | envelope | 750 (5.3%) | 2.000 | 1262.9 | 362.500 |
| `persist_root` | folha | 654 (2.3%) | 1.000 | 0.0 | 112.500 |
| `page_file_sync` | folha | 574 (2.2%) | 0.000 | 0.0 | 41.288.500 |
| `identity_lookup` | envelope | 479 (2.4%) | 2.000 | 2.0 | 808.800 |
| `object_decode` | folha | 405 (0.5%) | 1.000 | 631.5 | 48.100 |
| `heap_page_write` | folha | 347 (7.1%) | 1.020 | 8354.4 | 134.300 |
| `buffer_pool_hit` | folha | 344 (0.4%) | 8.921 | 8192.0 | 235.400 |
| `object_bind` | folha | 220 (0.6%) | 1.000 | 7.0 | 32.200 |
| `heap_candidate_scan` | folha | 115 (0.4%) | 1.000 | 1.5 | 26.100 |

### @build/stage-profile/modb_load.exe load.crud_full.embedded.100k · delete — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `heap_record_read` | envelope | 1.594 (2.0%) | 1.000 | 183.5 | 159.500 |
| `buffer_pool_miss` | folha | 1.544 (2.0%) | 0.081 | 2666.2 | 853.900 |
| `tx_commit` | envelope | 961 (2.0%) | 0.001 | 0.0 | 14.701.000 |
| `wal_sync` | folha | 454 (0.1%) | 0.001 | 0.0 | 920.200 |
| `object_decode` | folha | 398 (0.4%) | 1.000 | 183.5 | 108.800 |
| `identity_lookup` | envelope | 334 (0.9%) | 1.000 | 1.0 | 855.700 |
| `buffer_pool_hit` | folha | 245 (1.2%) | 4.919 | 8192.0 | 45.500 |
| `wal_append` | folha | 199 (3.9%) | 0.010 | 64.9 | 310.500 |
| `buffer_pool_writeback` | folha | 164 (10.2%) | 0.001 | 64.6 | 494.200 |
| `page_file_sync` | folha | 127 (7.8%) | 0.000 | 0.0 | 13.136.100 |

