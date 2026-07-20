# Exemplos por fase

Use uma pasta por fase para manter claro quando cada capacidade ficou disponivel.
As fases 00-07 mostram as fundacoes locais que o servidor passa a expor depois;
as fases 08-13 mostram o consumo remoto e as extensoes publicas. Prefira
exemplos curtos, com um `main` unico, que mostrem o fluxo de uma aplicacao
consumidora real.

## Alvos

| Fase | Alvo CMake |
|---|---|
| 00 | `ring0_server_phase_00_version_compatibility` |
| 01 | `ring0_server_phase_01_bind_type` |
| 02 | `ring0_server_phase_02_persist_reopen` |
| 03 | `ring0_server_phase_03_handle_update` |
| 04 | `ring0_server_phase_04_relationships` |
| 05 | `ring0_server_phase_05_transaction_recovery` |
| 06 | `ring0_server_phase_06_snapshot_read` |
| 07 | `ring0_server_phase_07_streaming_query` |
| 08 | `ring0_server_phase_08_connect_query` |
| 09 | `ring0_server_phase_09_call_operation` |
| 10 | `ring0_server_phase_10_handshake_capabilities` |
| 11 | `ring0_server_phase_11_open_facade` |
| 12 | `ring0_server_phase_12_graph_traversal` |
| 13 | `ring0_server_phase_13_async_io` |
