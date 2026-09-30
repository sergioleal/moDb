/**
 * Códigos de erro do Ring0 que um cliente de procs encontra no OpResult (modb::ErrorCode, include/modb/error.hpp;
 * a tabela e o que fazer com cada um estão em docs/PROTOCOLO_CLIENTES.md §3). Os números são estáveis
 * (docs/COMPATIBILIDADE.md), e o teste modb.error_codes confere esta tabela contra o header.
 */
export const ErrorCode = {
  invalid_argument: 1,
  value_too_large: 21,
  record_not_found: 30,
  snapshot_conflict: 44,
  protocol_error: 45,
  frame_too_large: 46,
  connection_closed: 47,
  operation_not_found: 48,
  incompatible_protocol_version: 50,
  conflict: 70,
  internal_error: 71,
  operation_timeout: 72,
  unauthenticated: 73,
  permission_denied: 74,
} as const;

/** Erro devolvido pelo servidor (OpResult/AuthenticateOk com ok = 0). */
export class ModbError extends Error {
  readonly code: number;
  constructor(code: number, message: string) {
    super(message);
    this.name = "ModbError";
    this.code = code;
  }
}

/**
 * A conexão caiu, não abriu ou o protocolo quebrou: o servidor está indisponível ou fechou a conexão
 * (idle timeout, reinício). Uma leitura pode ser repetida; uma escrita, não (pode ter sido confirmada).
 */
export class ModbConnectionError extends Error {
  constructor(message: string, options?: { cause?: unknown }) {
    super(message, options);
    this.name = "ModbConnectionError";
  }
}
