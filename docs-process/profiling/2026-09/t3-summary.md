# T3 remedição stage-profile

## load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 61.162 (3.4%) | 160.963 (5.2%) | 483 | 62.0% | 37.9% |
| mixed_oltp | 5 | 6.569 (7.6%) | 6.575 (7.6%) | 2.687 | 0.1% | 94.2% |

### load.mixed_oltp.embedded.10k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 3.595 (7.1%) | 0.001 | 0.0 | 6.033.400 |
| `wal_append` | folha | 1.443 (6.5%) | 0.060 | 480.4 | 1.503.500 |
| `object_encode` | folha | 960 (3.5%) | 1.000 | 357.5 | 31.700 |
| `wal_sync` | folha | 927 (4.6%) | 0.002 | 0.0 | 1.477.300 |
| `page_file_sync` | folha | 833 (14.9%) | 0.002 | 0.0 | 2.164.100 |
| `persist_root` | folha | 630 (4.2%) | 1.000 | 0.0 | 73.900 |
| `buffer_pool_hit` | folha | 428 (4.5%) | 3.047 | 24965.1 | 23.000 |
| `buffer_pool_writeback` | folha | 362 (14.7%) | 0.001 | 478.4 | 690.600 |
| `heap_page_write` | folha | 336 (6.3%) | 1.048 | 8581.9 | 68.600 |
| `object_bind` | folha | 234 (5.8%) | 1.000 | 7.0 | 37.900 |
| `heap_candidate_scan` | folha | 47 (4.3%) | 1.000 | 1.0 | 11.000 |

### load.mixed_oltp.embedded.10k · mixed_oltp — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 142.100 (8.0%) | 0.089 | 0.0 | 7.767.400 |
| `page_file_sync` | folha | 61.515 (6.4%) | 0.178 | 0.0 | 5.400.500 |
| `wal_sync` | folha | 58.578 (10.1%) | 0.178 | 0.0 | 2.640.100 |
| `wal_append` | folha | 14.194 (5.9%) | 0.492 | 2590.4 | 995.400 |
| `buffer_pool_writeback` | folha | 5.371 (11.4%) | 0.089 | 2573.4 | 588.000 |
| `buffer_pool_hit` | folha | 2.592 (6.8%) | 3.313 | 27137.1 | 302.000 |
| `heap_record_read` | envelope | 2.510 (8.5%) | 1.022 | 381.8 | 142.900 |
| `identity_lookup` | envelope | 2.200 (10.1%) | 1.022 | 1.0 | 304.200 |
| `object_decode` | folha | 719 (11.5%) | 0.978 | 365.3 | 91.600 |
| `materialize` | folha | 548 (12.9%) | 0.911 | 6.4 | 320.300 |
| `object_encode` | folha | 202 (11.5%) | 0.067 | 23.9 | 89.400 |
| `persist_root` | folha | 123 (11.7%) | 0.067 | 0.0 | 42.500 |
| `object_bind` | folha | 89 (12.9%) | 0.067 | 0.5 | 67.800 |
| `heap_page_write` | folha | 65 (12.4%) | 0.070 | 571.3 | 19.200 |
| `heap_candidate_scan` | folha | 50 (20.6%) | 0.067 | 0.1 | 37.400 |

## load.snapshot_hold.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 55.622 (6.8%) | 145.580 (13.2%) | 483 | 61.5% | 39.3% |
| hold | 5 | 805 (7.3%) | 945 (7.6%) | 0 | 14.9% | 94.7% |

### load.snapshot_hold.embedded.10k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 4.218 (10.6%) | 0.001 | 0.0 | 5.694.900 |
| `wal_append` | folha | 1.559 (7.3%) | 0.060 | 480.4 | 232.600 |
| `wal_sync` | folha | 1.131 (24.8%) | 0.002 | 0.0 | 1.607.500 |
| `object_encode` | folha | 1.067 (10.6%) | 1.000 | 357.5 | 97.800 |
| `page_file_sync` | folha | 1.017 (25.0%) | 0.002 | 0.0 | 1.823.700 |
| `persist_root` | folha | 683 (13.3%) | 1.000 | 0.0 | 99.300 |
| `buffer_pool_writeback` | folha | 472 (10.0%) | 0.001 | 478.4 | 901.500 |
| `buffer_pool_hit` | folha | 471 (13.1%) | 3.047 | 24965.1 | 33.500 |
| `heap_page_write` | folha | 360 (7.1%) | 1.048 | 8581.9 | 86.300 |
| `object_bind` | folha | 267 (16.4%) | 1.000 | 7.0 | 48.100 |
| `heap_candidate_scan` | folha | 51 (10.0%) | 1.000 | 1.0 | 10.600 |

### load.snapshot_hold.embedded.10k · hold — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 1.194.538 (7.7%) | 0.767 | 0.0 | 10.258.500 |
| `page_file_sync` | folha | 517.475 (7.5%) | 1.533 | 0.0 | 3.064.200 |
| `wal_sync` | folha | 502.321 (7.6%) | 1.533 | 0.0 | 4.309.300 |
| `wal_append` | folha | 111.519 (7.7%) | 3.954 | 19969.5 | 5.167.700 |
| `buffer_pool_writeback` | folha | 41.549 (11.1%) | 0.767 | 19832.8 | 727.600 |
| `identity_lookup` | envelope | 3.678 (11.7%) | 2.000 | 2.0 | 468.900 |
| `buffer_pool_hit` | folha | 3.326 (13.0%) | 9.320 | 76349.4 | 457.600 |
| `heap_record_read` | envelope | 3.148 (10.4%) | 2.000 | 746.8 | 375.500 |
| `object_decode` | folha | 2.049 (7.0%) | 1.667 | 622.4 | 75.600 |
| `object_encode` | folha | 1.471 (12.3%) | 0.433 | 155.0 | 64.900 |
| `persist_root` | folha | 824 (16.5%) | 0.433 | 0.0 | 106.300 |
| `object_bind` | folha | 745 (13.1%) | 0.433 | 3.0 | 333.800 |
| `heap_page_write` | folha | 410 (16.7%) | 0.454 | 3718.3 | 33.800 |
| `materialize` | folha | 355 (17.1%) | 1.000 | 7.0 | 141.700 |
| `heap_candidate_scan` | folha | 346 (18.5%) | 0.433 | 0.4 | 14.400 |

## load.crud_full.embedded.100k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 5 | 57.798 (2.2%) | 153.046 (4.1%) | 481 | 62.2% | 39.8% |
| read | 5 | 113.102 (2.8%) | 372.576 (4.3%) | 481 | 69.6% | 15.9% |
| update_inplace | 5 | 65.048 (7.2%) | 100.427 (5.7%) | 954 | 35.3% | 57.4% |
| update_grow | 5 | 56.053 (5.9%) | 91.710 (5.9%) | 1.721 | 38.9% | 57.8% |
| update_shrink | 5 | 42.513 (10.6%) | 78.521 (9.4%) | 2.656 | 45.9% | 71.4% |
| delete | 5 | 128.531 (8.9%) | 188.756 (9.6%) | 2.720 | 31.9% | 66.2% |

### load.crud_full.embedded.100k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 4.254 (3.7%) | 0.001 | 0.0 | 10.569.000 |
| `wal_append` | folha | 1.626 (4.1%) | 0.060 | 481.2 | 1.290.600 |
| `page_file_sync` | folha | 995 (13.1%) | 0.002 | 0.0 | 2.827.800 |
| `wal_sync` | folha | 994 (5.6%) | 0.002 | 0.0 | 2.874.800 |
| `object_encode` | folha | 978 (3.1%) | 1.000 | 359.5 | 213.700 |
| `persist_root` | folha | 661 (4.9%) | 1.000 | 0.0 | 69.800 |
| `buffer_pool_writeback` | folha | 597 (6.1%) | 0.001 | 479.1 | 1.763.100 |
| `buffer_pool_hit` | folha | 450 (3.6%) | 3.048 | 24965.9 | 60.900 |
| `heap_page_write` | folha | 285 (3.4%) | 1.048 | 8582.0 | 198.900 |
| `object_bind` | folha | 238 (3.8%) | 1.000 | 7.0 | 62.200 |
| `heap_candidate_scan` | folha | 65 (2.4%) | 1.000 | 1.0 | 42.900 |

### load.crud_full.embedded.100k · read — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `heap_record_read` | envelope | 872 (6.2%) | 1.000 | 375.5 | 375.000 |
| `identity_lookup` | envelope | 792 (4.2%) | 1.000 | 1.0 | 405.800 |
| `buffer_pool_hit` | folha | 448 (3.0%) | 2.987 | 24465.7 | 45.800 |
| `object_decode` | folha | 402 (3.5%) | 1.000 | 375.5 | 93.900 |
| `materialize` | folha | 290 (4.9%) | 1.000 | 7.0 | 119.500 |
| `buffer_pool_miss` | folha | 271 (13.2%) | 0.013 | 441.1 | 371.800 |

### load.crud_full.embedded.100k · update_inplace — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 4.427 (13.4%) | 0.001 | 0.0 | 25.028.500 |
| `heap_record_read` | envelope | 1.605 (6.4%) | 2.000 | 750.9 | 286.000 |
| `wal_append` | folha | 1.588 (4.6%) | 0.059 | 473.0 | 6.951.600 |
| `identity_lookup` | envelope | 1.563 (5.0%) | 2.000 | 2.0 | 247.600 |
| `buffer_pool_hit` | folha | 1.379 (5.8%) | 9.034 | 74007.8 | 244.700 |
| `page_file_sync` | folha | 1.211 (21.5%) | 0.002 | 0.0 | 4.564.900 |
| `wal_sync` | folha | 1.057 (19.5%) | 0.002 | 0.0 | 12.157.000 |
| `object_encode` | folha | 1.057 (6.2%) | 1.000 | 359.5 | 450.100 |
| `persist_root` | folha | 703 (6.1%) | 1.000 | 0.0 | 110.000 |
| `buffer_pool_writeback` | folha | 530 (15.1%) | 0.001 | 471.0 | 1.755.500 |
| `object_decode` | folha | 433 (5.3%) | 1.000 | 375.5 | 82.200 |
| `buffer_pool_miss` | folha | 306 (9.5%) | 0.013 | 441.1 | 207.400 |
| `heap_page_write` | folha | 275 (5.9%) | 1.048 | 8582.1 | 268.100 |
| `object_bind` | folha | 242 (6.9%) | 1.000 | 7.0 | 46.200 |
| `heap_candidate_scan` | folha | 83 (3.9%) | 1.000 | 1.0 | 34.500 |

### load.crud_full.embedded.100k · update_grow — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 5.711 (6.3%) | 0.001 | 0.0 | 12.288.700 |
| `wal_append` | folha | 2.484 (4.7%) | 0.095 | 766.8 | 1.595.100 |
| `identity_lookup` | envelope | 1.659 (5.6%) | 2.000 | 2.0 | 307.400 |
| `heap_record_read` | envelope | 1.585 (5.4%) | 2.000 | 750.9 | 249.400 |
| `buffer_pool_hit` | folha | 1.385 (7.3%) | 9.066 | 74265.1 | 245.100 |
| `page_file_sync` | folha | 1.301 (7.1%) | 0.002 | 0.0 | 5.615.700 |
| `object_encode` | folha | 1.103 (5.6%) | 1.000 | 615.5 | 244.700 |
| `wal_sync` | folha | 1.072 (7.4%) | 0.002 | 0.0 | 6.205.300 |
| `buffer_pool_writeback` | folha | 803 (11.4%) | 0.001 | 763.7 | 1.971.100 |
| `persist_root` | folha | 708 (5.5%) | 1.000 | 0.0 | 65.000 |
| `object_decode` | folha | 441 (4.3%) | 1.000 | 375.5 | 242.300 |
| `buffer_pool_miss` | folha | 375 (11.3%) | 0.018 | 582.3 | 305.200 |
| `heap_page_write` | folha | 319 (5.3%) | 1.083 | 8874.7 | 208.600 |
| `object_bind` | folha | 265 (4.6%) | 1.000 | 7.0 | 40.300 |
| `heap_candidate_scan` | folha | 91 (2.0%) | 1.000 | 0.9 | 26.800 |

### load.crud_full.embedded.100k · update_shrink — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 9.871 (12.9%) | 0.001 | 0.0 | 56.299.700 |
| `page_file_sync` | folha | 4.090 (18.7%) | 0.002 | 0.0 | 27.172.700 |
| `wal_append` | folha | 3.078 (3.2%) | 0.116 | 934.4 | 551.300 |
| `buffer_pool_miss` | folha | 2.388 (7.2%) | 0.099 | 3235.0 | 629.500 |
| `heap_candidate_try` | envelope | 2.387 (5.7%) | 0.167 | 0.0 | 647.400 |
| `heap_record_read` | envelope | 1.993 (10.8%) | 2.000 | 1262.9 | 440.500 |
| `identity_lookup` | envelope | 1.783 (8.1%) | 2.000 | 2.0 | 275.700 |
| `buffer_pool_hit` | folha | 1.464 (8.2%) | 8.921 | 73081.5 | 131.500 |
| `wal_sync` | folha | 1.349 (22.3%) | 0.002 | 0.0 | 7.455.400 |
| `buffer_pool_writeback` | folha | 1.287 (16.0%) | 0.001 | 930.5 | 8.478.000 |
| `object_encode` | folha | 1.227 (13.8%) | 1.000 | 167.5 | 141.900 |
| `persist_root` | folha | 787 (12.0%) | 1.000 | 0.0 | 115.800 |
| `object_decode` | folha | 512 (11.0%) | 1.000 | 631.5 | 121.100 |
| `heap_page_write` | folha | 390 (7.9%) | 1.020 | 8354.4 | 131.700 |
| `object_bind` | folha | 281 (13.6%) | 1.000 | 7.0 | 57.900 |
| `heap_candidate_scan` | folha | 95 (5.5%) | 1.000 | 1.0 | 62.700 |

### load.crud_full.embedded.100k · delete — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.334 (8.9%) | 0.001 | 0.0 | 4.933.400 |
| `heap_record_read` | envelope | 2.208 (11.2%) | 1.000 | 183.5 | 440.700 |
| `buffer_pool_miss` | folha | 1.589 (11.3%) | 0.081 | 2666.9 | 437.200 |
| `identity_lookup` | envelope | 998 (7.6%) | 1.000 | 1.0 | 411.600 |
| `page_file_sync` | folha | 964 (9.7%) | 0.002 | 0.0 | 1.730.300 |
| `wal_sync` | folha | 842 (10.5%) | 0.002 | 0.0 | 1.282.400 |
| `buffer_pool_hit` | folha | 816 (7.6%) | 4.919 | 40293.3 | 221.100 |
| `object_decode` | folha | 482 (13.0%) | 1.000 | 183.5 | 187.500 |
| `wal_append` | folha | 317 (6.2%) | 0.010 | 64.9 | 1.912.900 |
| `buffer_pool_writeback` | folha | 174 (11.5%) | 0.001 | 64.6 | 516.000 |

