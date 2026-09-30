/**
 * Pool de conexões com o modb-proxy, no modelo do gateway da biblioteca (web/gateway.cpp):
 *
 * - N conexões; cada chamada vai para a com menos chamadas a caminho (o servidor atende procs de leitura de
 *   conexões diferentes em paralelo; numa conexão, uma de cada vez, em ordem).
 * - Conexões abrem sob demanda e reabrem depois de cair: o servidor fecha as ociosas (idle timeout) e o proxy
 *   fecha todas quando perde o link com o engine.
 * - Perto do idle timeout do servidor, a conexão pode estar sendo fechada: é descartada antes de usar. Numa
 *   escrita, só se reaproveita uma conexão usada há pouco (WRITE_REUSE_MS).
 * - Leituras que falham por conexão caída numa conexão reaproveitada são repetidas uma vez numa conexão nova.
 *   Escritas nunca: o servidor pode ter confirmado antes de a conexão cair.
 */
import { Connection, type ConnectionOptions } from "./connection.ts";
import { ModbConnectionError } from "./errors.ts";
import type { ModbValue } from "./value.ts";

export interface PoolOptions extends ConnectionOptions {
  /** Conexões simultâneas (padrão 8). */
  size?: number;
}

export interface CallOptions {
  /** A proc só lê: pode ser repetida numa conexão nova se a conexão caiu. */
  read?: boolean;
}

const WRITE_REUSE_MS = 1000;

interface Slot {
  conn: Connection | null;
  opening: Promise<Connection> | null;
}

export class Pool {
  private readonly options: ConnectionOptions;
  private readonly slots: Slot[];
  private closed = false;

  constructor(options: PoolOptions) {
    const { size = 8, ...rest } = options;
    if (size < 1) throw new RangeError("o pool precisa de ao menos uma conexão");
    this.options = rest;
    this.slots = Array.from({ length: size }, () => ({ conn: null, opening: null }));
  }

  async call(proc: string, args: Record<string, unknown> = {}, opts: CallOptions = {}): Promise<ModbValue> {
    const read = opts.read ?? false;
    const slot = this.pick();
    const reused = this.usable(slot, read);
    const conn = reused ?? (await this.open(slot));
    try {
      return await conn.call(proc, args);
    } catch (err) {
      if (!(err instanceof ModbConnectionError)) throw err;
      if (slot.conn === conn) slot.conn = null;
      if (read && reused) return (await this.open(slot)).call(proc, args);
      throw err;
    }
  }

  /** Abre uma conexão e confere a autenticação (sonda de prontidão). */
  async ping(): Promise<void> {
    const slot = this.pick();
    if (!this.usable(slot, true)) await this.open(slot);
  }

  close(): void {
    this.closed = true;
    for (const s of this.slots) {
      s.conn?.close();
      s.conn = null;
    }
  }

  private pick(): Slot {
    let best = this.slots[0]!;
    let bestLoad = Infinity;
    for (const s of this.slots) {
      // Uma conexão fechada ou por abrir conta como livre, mas perde para uma aberta sem nada a caminho.
      const load = s.conn && !s.conn.closed ? s.conn.inFlight : s.opening ? 0.5 : 0.25;
      if (load < bestLoad) {
        best = s;
        bestLoad = load;
      }
      if (load === 0) break;
    }
    return best;
  }

  /** A conexão do slot, se ainda serve para esta chamada; senão descarta e devolve null. */
  private usable(slot: Slot, read: boolean): Connection | null {
    const conn = slot.conn;
    if (!conn) return null;
    if (conn.closed) {
      slot.conn = null;
      return null;
    }
    if (conn.inFlight > 0) return conn; // em uso agora: não está ociosa
    const idle = Date.now() - conn.lastUsed;
    const limit = conn.info.idleTimeoutMs;
    if ((limit > 0 && idle > (limit * 3) / 4) || (!read && idle > WRITE_REUSE_MS)) {
      conn.close();
      slot.conn = null;
      return null;
    }
    return conn;
  }

  private open(slot: Slot): Promise<Connection> {
    if (this.closed) return Promise.reject(new ModbConnectionError("pool fechado"));
    slot.opening ??= Connection.open(this.options)
      .then((conn) => {
        slot.conn = conn;
        return conn;
      })
      .finally(() => {
        slot.opening = null;
      });
    return slot.opening;
  }
}
