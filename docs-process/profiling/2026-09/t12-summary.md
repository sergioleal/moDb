# T12 workloads restantes

## load.range_scan_sweep.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 65.773 (1.0%) | 117.509 (1.9%) | 483 | 44.0% | 31.1% |
| scan_0.01pct_index_scan | 3 | 16.042 (33.6%) | — | 0 | 0.0% | — |
| scan_0.1pct_index_scan | 3 | 205.069 (34.1%) | — | 0 | 0.0% | — |
| scan_1pct_index_scan | 3 | 323.756 (30.5%) | — | 0 | 0.0% | — |
| scan_10pct_index_scan | 3 | 400.568 (0.8%) | — | 0 | 0.0% | — |
| scan_100pct_index_scan | 3 | 401.060 (1.6%) | — | 0 | 0.0% | — |

### load.range_scan_sweep.embedded.10k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.067 (4.3%) | 0.001 | 0.0 | 2.641.100 |
| `object_encode` | folha | 990 (1.8%) | 1.000 | 357.5 | 30.300 |
| `wal_append` | folha | 950 (5.8%) | 0.060 | 480.4 | 228.300 |
| `persist_root` | folha | 635 (0.6%) | 1.000 | 0.0 | 25.400 |
| `wal_sync` | folha | 629 (4.7%) | 0.001 | 0.0 | 1.064.700 |
| `buffer_pool_writeback` | folha | 466 (9.6%) | 0.001 | 478.4 | 682.700 |
| `buffer_pool_hit` | folha | 427 (1.6%) | 3.047 | 24965.1 | 20.500 |
| `heap_page_write` | folha | 341 (0.6%) | 1.048 | 8581.9 | 104.900 |
| `object_bind` | folha | 239 (1.1%) | 1.000 | 7.0 | 30.000 |
| `heap_candidate_scan` | folha | 50 (1.5%) | 1.000 | 1.0 | 5.800 |

## load.cascade_delete.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create_hierarchy | 3 | 0 (0.0%) | — | 0 | 0.0% | — |
| cascade_delete | 3 | 0 (0.0%) | — | 0 | 0.0% | — |

## load.blob_lifecycle.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 0 (0.0%) | — | 0 | 0.0% | — |
| read | 3 | 0 (0.0%) | — | 0 | 0.0% | — |
| update_grow | 3 | 0 (0.0%) | — | 0 | 0.0% | — |
| update_shrink | 3 | 0 (0.0%) | — | 0 | 0.0% | — |
| delete | 3 | 0 (0.0%) | — | 0 | 0.0% | — |

## load.oversubscribed_churn.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 67.208 (9.5%) | 122.669 (11.3%) | 483 | 45.1% | 30.4% |
| delete | 3 | 188.620 (4.3%) | 193.587 (4.3%) | 837 | 2.6% | 67.3% |

### load.oversubscribed_churn.embedded.10k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 1.865 (16.0%) | 0.001 | 0.0 | 2.621.800 |
| `object_encode` | folha | 1.018 (8.1%) | 1.000 | 357.5 | 48.100 |
| `wal_append` | folha | 823 (15.1%) | 0.060 | 480.4 | 206.100 |
| `persist_root` | folha | 656 (9.4%) | 1.000 | 0.0 | 26.000 |
| `wal_sync` | folha | 628 (12.8%) | 0.001 | 0.0 | 1.145.000 |
| `buffer_pool_hit` | folha | 450 (10.9%) | 3.046 | 24955.3 | 16.800 |
| `buffer_pool_writeback` | folha | 396 (23.2%) | 0.001 | 478.4 | 686.700 |
| `heap_page_write` | folha | 269 (10.0%) | 1.048 | 8581.9 | 62.800 |
| `object_bind` | folha | 239 (12.7%) | 1.000 | 7.0 | 33.200 |
| `heap_candidate_scan` | folha | 51 (10.4%) | 1.000 | 1.0 | 3.700 |
| `buffer_pool_miss` | folha | 25 (21.9%) | 0.001 | 18.0 | 135.000 |

### load.oversubscribed_churn.embedded.10k · delete — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 1.552 (10.5%) | 0.001 | 0.0 | 3.017.200 |
| `heap_record_read` | envelope | 1.198 (3.4%) | 1.000 | 373.5 | 124.200 |
| `buffer_pool_miss` | folha | 1.182 (2.9%) | 0.098 | 3203.1 | 135.300 |
| `identity_lookup` | envelope | 626 (0.2%) | 1.000 | 1.0 | 135.700 |
| `wal_append` | folha | 599 (7.2%) | 0.045 | 354.6 | 212.500 |
| `wal_sync` | folha | 549 (18.9%) | 0.001 | 0.0 | 1.047.800 |
| `buffer_pool_hit` | folha | 472 (3.0%) | 4.902 | 24576.0 | 36.800 |
| `object_decode` | folha | 388 (3.8%) | 1.000 | 373.5 | 43.800 |
| `buffer_pool_writeback` | folha | 385 (27.9%) | 0.001 | 353.1 | 1.735.000 |

## load.restart_recovery.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 58.051 (10.2%) | 100.690 (11.1%) | 483 | 42.3% | 30.9% |
| restart_recovery | 3 | 0 (0.0%) | — | 0 | 0.0% | — |

### load.restart_recovery.embedded.10k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.168 (13.1%) | 0.001 | 0.0 | 3.545.600 |
| `object_encode` | folha | 1.178 (13.0%) | 1.000 | 357.5 | 49.100 |
| `wal_append` | folha | 931 (15.9%) | 0.060 | 480.4 | 385.300 |
| `persist_root` | folha | 769 (12.7%) | 1.000 | 0.0 | 47.400 |
| `wal_sync` | folha | 701 (16.0%) | 0.001 | 0.0 | 2.155.900 |
| `buffer_pool_hit` | folha | 519 (16.0%) | 3.047 | 24965.1 | 18.200 |
| `buffer_pool_writeback` | folha | 509 (11.4%) | 0.001 | 478.4 | 916.600 |
| `heap_page_write` | folha | 380 (9.2%) | 1.048 | 8581.9 | 120.100 |
| `object_bind` | folha | 302 (15.0%) | 1.000 | 7.0 | 111.900 |
| `heap_candidate_scan` | folha | 62 (6.2%) | 1.000 | 1.0 | 13.400 |

## load.create_only.loopback.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 108.832 (8.5%) | — | 483 | 0.0% | — |

## load.crud_full.loopback.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|

## load.create_only.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 61.144 (7.7%) | 108.077 (7.6%) | 483 | 43.4% | 30.5% |

### load.create_only.embedded.10k · create — estágios

| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |
|---|---|---|---|---|---|
| `tx_commit` | envelope | 2.114 (6.7%) | 0.001 | 0.0 | 2.876.500 |
| `object_encode` | folha | 1.052 (10.1%) | 1.000 | 357.5 | 32.800 |
| `wal_append` | folha | 950 (5.7%) | 0.060 | 480.4 | 229.300 |
| `persist_root` | folha | 699 (9.1%) | 1.000 | 0.0 | 29.300 |
| `wal_sync` | folha | 640 (5.1%) | 0.001 | 0.0 | 1.061.500 |
| `buffer_pool_writeback` | folha | 498 (13.8%) | 0.001 | 478.4 | 897.600 |
| `buffer_pool_hit` | folha | 487 (9.9%) | 3.047 | 24965.1 | 74.400 |
| `heap_page_write` | folha | 359 (8.0%) | 1.048 | 8581.9 | 82.200 |
| `object_bind` | folha | 273 (8.6%) | 1.000 | 7.0 | 22.700 |
| `heap_candidate_scan` | folha | 53 (9.3%) | 1.000 | 1.0 | 9.300 |

