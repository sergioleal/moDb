/**
 * Uma conexão TCP com um servidor Ring0 (ou, como deve ser em produção, com um modb-proxy), pelo protocolo
 * nativo (PROTOCOLO_CLIENTES.md): Hello → HelloOk → Authenticate (token, minor ≥ 2) → OpCall/OpResult.
 *
 * As respostas de uma conexão saem na ordem dos pedidos; várias chamadas podem estar a caminho ao mesmo tempo
 * (casadas pelo call_id), mas o servidor as executa uma a uma. A concorrência vem de várias conexões (pool.ts).
 */
import { connect as netConnect, type Socket } from "node:net";
import { ModbConnectionError, ModbError } from "./errors.ts";
import { decodeValue, encodeValue, type ModbValue } from "./value.ts";

export const PROTOCOL_MAJOR = 1;
export const PROTOCOL_MINOR = 3;
export const MAX_FRAME_BYTES = 16 * 1024 * 1024;

const T_HELLO = 1;
const T_HELLO_OK = 2;
const T_OP_CALL = 9;
const T_OP_RESULT = 10;
const T_AUTHENTICATE = 17;
const T_AUTHENTICATE_OK = 18;

export interface ConnectionOptions {
  host: string;
  port: number;
  /** Nome do banco; vazio = o único do servidor. */
  database?: string;
  /** Token do modb-proxy (mecanismo "token"); sem ele, a conexão fica anônima. */
  token?: string;
  /** Tempo máximo para abrir, negociar e autenticar (ms). */
  connectTimeoutMs?: number;
}

/** Opções de uma chamada, minor 3 (ADR-029). */
export interface WireCallOptions {
  /** Em nome de quem: só atrás de um modb-proxy, com um token que tenha a role `delegate`. */
  actingAs?: string;
  /** Atributos do delegado (ex.: `{ email }`), que a proc vê. */
  actingAttributes?: Record<string, string>;
  /** Torna a escrita repetível: com a mesma chave, o servidor devolve o resultado já confirmado. */
  idempotencyKey?: string;
}

export interface ServerInfo {
  major: number;
  minor: number;
  maxFrameBytes: number;
  idleTimeoutMs: number;
  /** Mecanismos de autenticação que o proxy exige (vazio = não exige). */
  mechanisms: string[];
  /** Quem o proxy diz que somos, depois do Authenticate. */
  principal: string;
}

const str = (s: string) => {
  const b = Buffer.from(s, "utf8");
  const out = Buffer.allocUnsafe(4 + b.length);
  out.writeUInt32LE(b.length, 0);
  b.copy(out, 4);
  return out;
};

const frame = (type: number, payload: Buffer) => {
  const head = Buffer.allocUnsafe(5);
  head.writeUInt32LE(payload.length + 1, 0);
  head[4] = type;
  return Buffer.concat([head, payload]);
};

interface Pending {
  callId: number;
  resolve: (body: Buffer) => void;
  reject: (err: Error) => void;
}

export class Connection {
  readonly info: ServerInfo;
  /** Instante (Date.now) da última resposta recebida; o pool usa para evitar conexões perto do idle timeout. */
  lastUsed = Date.now();

  private readonly socket: Socket;
  private chunks: Buffer[] = [];
  private buffered = 0;
  private readonly pending: Pending[] = [];
  private nextId = 1;
  private closedError: Error | null = null;

  private constructor(socket: Socket, info: ServerInfo) {
    this.socket = socket;
    this.info = info;
  }

  get closed(): boolean {
    return this.closedError !== null;
  }

  /** Chamadas enviadas e ainda sem resposta. */
  get inFlight(): number {
    return this.pending.length;
  }

  static async open(options: ConnectionOptions): Promise<Connection> {
    const timeoutMs = options.connectTimeoutMs ?? 10_000;
    const socket = netConnect({ host: options.host, port: options.port, noDelay: true });
    const handshake = new Handshake(socket);
    const timer = setTimeout(() => handshake.fail(new ModbConnectionError(`tempo esgotado conectando em ${options.host}:${options.port}`)), timeoutMs);
    try {
      await handshake.connected;
      const hello = Buffer.concat([u16(PROTOCOL_MAJOR), str(options.database ?? ""), Buffer.from([1, 0]), u16(PROTOCOL_MINOR)]);
      socket.write(frame(T_HELLO, hello));
      const [type, body] = await handshake.nextFrame();
      if (type !== T_HELLO_OK) throw new ModbConnectionError(`esperava HelloOk, veio a mensagem ${type}`);
      const info = parseHelloOk(body);
      if (info.major !== PROTOCOL_MAJOR) throw new ModbConnectionError(`versão de protocolo incompatível: ${info.major}`);
      if (options.token !== undefined) {
        if (info.minor < 2) throw new ModbConnectionError("o servidor não aceita autenticação (protocolo minor < 2)");
        const token = Buffer.from(options.token, "utf8");
        const payload = Buffer.concat([u32(1), str("token"), u32(token.length), token]);
        socket.write(frame(T_AUTHENTICATE, payload));
        const [atype, abody] = await handshake.nextFrame();
        if (atype !== T_AUTHENTICATE_OK) throw new ModbConnectionError(`esperava AuthenticateOk, veio a mensagem ${atype}`);
        info.principal = parseAuthenticateOk(abody);
      }
      clearTimeout(timer);
      const conn = new Connection(socket, info);
      handshake.handOver(conn);
      return conn;
    } catch (err) {
      clearTimeout(timer);
      socket.destroy();
      throw err instanceof ModbError || err instanceof ModbConnectionError
        ? err
        : new ModbConnectionError(`não foi possível conectar em ${options.host}:${options.port}: ${(err as Error).message}`, { cause: err });
    }
  }

  /** Chama uma proc. Rejeita com ModbError (erro da proc) ou ModbConnectionError (conexão). */
  call(proc: string, args: Record<string, unknown> = {}, options: WireCallOptions = {}): Promise<ModbValue> {
    if (this.closedError) return Promise.reject(this.closedError);
    if ((options.actingAs || options.idempotencyKey) && this.info.minor < 3) {
      return Promise.reject(new ModbError(1, "o servidor não aceita delegação nem chave de idempotência (protocolo minor < 3)"));
    }
    const callId = this.nextId;
    this.nextId = this.nextId >= 0xffff_ffff ? 1 : this.nextId + 1;
    const encoded = encodeValue(args);
    const payload = Buffer.concat([u32(callId), str(proc), u32(encoded.length), encoded, extension(options)]);
    if (payload.length + 1 > this.info.maxFrameBytes) {
      return Promise.reject(new ModbError(21, `pedido de ${payload.length} bytes excede o limite do servidor`));
    }
    return new Promise<Buffer>((resolve, reject) => {
      this.pending.push({ callId, resolve, reject });
      this.socket.write(frame(T_OP_CALL, payload));
    }).then(decodeValue);
  }

  close(): void {
    this.fail(new ModbConnectionError("conexão fechada pelo cliente"));
  }

  /** @internal Recebe os bytes depois do handshake. */
  feed(chunk: Buffer): void {
    this.chunks.push(chunk);
    this.buffered += chunk.length;
    try {
      for (let f = takeFrame(this); f; f = takeFrame(this)) this.onFrame(f[0], f[1]);
    } catch (err) {
      this.fail(err as Error);
    }
  }

  /** @internal */
  fail(err: Error): void {
    if (this.closedError) return;
    this.closedError = err instanceof ModbConnectionError ? err : new ModbConnectionError(err.message, { cause: err });
    this.socket.destroy();
    for (const p of this.pending.splice(0)) p.reject(this.closedError);
  }

  private onFrame(type: number, body: Buffer) {
    if (type !== T_OP_RESULT) throw new ModbConnectionError(`mensagem inesperada do servidor: ${type}`);
    const callId = body.readUInt32LE(0);
    const ok = body[4];
    const p = this.pending.shift();
    if (!p || p.callId !== callId) throw new ModbConnectionError("OpResult fora de ordem (call_id não confere)");
    this.lastUsed = Date.now();
    if (ok) {
      const n = body.readUInt32LE(5);
      p.resolve(body.subarray(9, 9 + n));
    } else {
      const code = body.readUInt16LE(5);
      const n = body.readUInt32LE(7);
      // Minor 3: o detail do erro, se veio (detail_len u32 | Value).
      let detail: unknown;
      const at = 11 + n;
      if (body.length - at >= 4) {
        const d = body.readUInt32LE(at);
        detail = decodeValue(body.subarray(at + 4, at + 4 + d));
      }
      p.reject(new ModbError(code, body.toString("utf8", 11, 11 + n), detail));
    }
  }

  /** @internal */
  _take(n: number): Buffer | null {
    if (this.buffered < n) return null;
    const all = this.chunks.length === 1 ? this.chunks[0]! : Buffer.concat(this.chunks);
    const out = all.subarray(0, n);
    const rest = all.subarray(n);
    this.chunks = rest.length ? [rest] : [];
    this.buffered = rest.length;
    return out;
  }
  /** @internal */
  _peekLength(): number | null {
    if (this.buffered < 4) return null;
    const all = this.chunks.length === 1 ? this.chunks[0]! : Buffer.concat(this.chunks);
    this.chunks = [all];
    return all.readUInt32LE(0);
  }
}

interface FrameSource {
  _take(n: number): Buffer | null;
  _peekLength(): number | null;
}

function takeFrame(src: FrameSource): [number, Buffer] | null {
  const length = src._peekLength();
  if (length === null) return null;
  if (length === 0 || length > MAX_FRAME_BYTES) throw new ModbConnectionError(`frame com tamanho inválido: ${length}`);
  const whole = src._take(4 + length);
  if (!whole) return null;
  return [whole[4]!, whole.subarray(5)];
}

/** Lê os frames do handshake antes de a Connection existir e depois passa o socket para ela. */
class Handshake implements FrameSource {
  readonly connected: Promise<void>;
  private chunks: Buffer[] = [];
  private buffered = 0;
  private waiter: { resolve: (f: [number, Buffer]) => void; reject: (e: Error) => void } | null = null;
  private error: Error | null = null;
  private owner: Connection | null = null;
  private readonly socket: Socket;

  constructor(socket: Socket) {
    this.socket = socket;
    this.connected = new Promise((resolve, reject) => {
      socket.once("connect", () => resolve());
      socket.once("error", reject);
    });
    socket.on("data", (chunk: Buffer) => {
      if (this.owner) return this.owner.feed(chunk);
      this.chunks.push(chunk);
      this.buffered += chunk.length;
      this.pump();
    });
    socket.on("error", (err) => this.fail(new ModbConnectionError(err.message, { cause: err })));
    socket.on("close", () => this.fail(new ModbConnectionError("o servidor fechou a conexão")));
  }

  nextFrame(): Promise<[number, Buffer]> {
    return new Promise((resolve, reject) => {
      if (this.error) return reject(this.error);
      this.waiter = { resolve, reject };
      this.pump();
    });
  }

  fail(err: Error) {
    if (this.owner) return this.owner.fail(err);
    this.error ??= err;
    this.socket.destroy();
    const w = this.waiter;
    this.waiter = null;
    w?.reject(this.error);
  }

  handOver(conn: Connection) {
    this.owner = conn;
    const rest = this._take(this.buffered);
    if (rest && rest.length) conn.feed(rest);
  }

  private pump() {
    if (!this.waiter) return;
    try {
      const f = takeFrame(this);
      if (!f) return;
      const w = this.waiter;
      this.waiter = null;
      w.resolve(f);
    } catch (err) {
      this.fail(err as Error);
    }
  }

  _take(n: number): Buffer | null {
    if (this.buffered < n) return null;
    const all = Buffer.concat(this.chunks);
    this.chunks = n < all.length ? [all.subarray(n)] : [];
    this.buffered = all.length - n;
    return all.subarray(0, n);
  }
  _peekLength(): number | null {
    if (this.buffered < 4) return null;
    const all = Buffer.concat(this.chunks);
    this.chunks = [all];
    return all.readUInt32LE(0);
  }
}

function parseHelloOk(b: Buffer): ServerInfo {
  // version u16 | baseline u64 | codec u8 | max_frame_bytes u32 | max_streams u16 | max_expansion u16 |
  // idle_timeout_ms u32 | minor u16 | [minor ≥ 2 e o proxy exige autenticação: mechanisms u8 n, string[n]]
  if (b.length < 23) throw new ModbConnectionError("HelloOk truncado");
  const info: ServerInfo = {
    major: b.readUInt16LE(0),
    maxFrameBytes: b.readUInt32LE(11),
    idleTimeoutMs: b.readUInt32LE(19),
    minor: b.length >= 25 ? b.readUInt16LE(23) : 0,
    mechanisms: [],
    principal: "",
  };
  if (info.minor >= 2 && b.length > 25) {
    let off = 26;
    for (let i = 0; i < b[25]!; i++) {
      const n = b.readUInt32LE(off);
      info.mechanisms.push(b.toString("utf8", off + 4, off + 4 + n));
      off += 4 + n;
    }
  }
  return info;
}

function parseAuthenticateOk(b: Buffer): string {
  // request_id u32 | ok u8 | code u16 | message string | principal string
  const ok = b[4];
  const code = b.readUInt16LE(5);
  const n = b.readUInt32LE(7);
  const message = b.toString("utf8", 11, 11 + n);
  if (!ok) throw new ModbError(code, message || "credencial recusada");
  const m = b.readUInt32LE(11 + n);
  return b.toString("utf8", 15 + n, 15 + n + m);
}

/**
 * Extensão do minor 3 no fim do OpCall (ADR-029), só quando há o que mandar:
 *   flags u8 | [1: acting_as string | n u8 | (chave, valor)*] | [2: idempotency_key string]
 */
function extension(options: WireCallOptions): Buffer {
  const flags = (options.actingAs ? 1 : 0) | (options.idempotencyKey ? 2 : 0);
  if (!flags) return Buffer.alloc(0);
  const parts = [Buffer.from([flags])];
  if (options.actingAs) {
    const attributes = Object.entries(options.actingAttributes ?? {});
    if (attributes.length > 32) throw new RangeError("no máximo 32 atributos de delegação");
    parts.push(str(options.actingAs), Buffer.from([attributes.length]));
    for (const [key, value] of attributes) parts.push(str(key), str(value));
  }
  if (options.idempotencyKey) {
    if (Buffer.byteLength(options.idempotencyKey, "utf8") > 128) throw new RangeError("chave de idempotência acima de 128 bytes");
    parts.push(str(options.idempotencyKey));
  }
  return Buffer.concat(parts);
}

function u16(v: number) {
  const b = Buffer.allocUnsafe(2);
  b.writeUInt16LE(v, 0);
  return b;
}
function u32(v: number) {
  const b = Buffer.allocUnsafe(4);
  b.writeUInt32LE(v, 0);
  return b;
}
