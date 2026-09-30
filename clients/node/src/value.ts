/**
 * Value v1 do Ring0 (PROTOCOLO_CLIENTES.md §4): argumentos e resultados das procs.
 *
 *   versão u8 (=1) | tag u8 | conteúdo
 *   0 null | 1 bool u8 | 2 integer i64 | 3 real f64 | 4 text u32+utf8 | 5 id u64 |
 *   6 list u32 + valores | 7 map u32 + (u32+chave utf8, valor)...
 *
 * JS → Value: null/undefined → null; boolean; number inteiro seguro ou bigint → integer; outro number → real;
 * string → text; Id → id; array → list; objeto simples → map (chaves com undefined são omitidas).
 * Value → JS: integer e id viram number quando cabem em Number.MAX_SAFE_INTEGER, senão bigint.
 */

export const VALUE_VERSION = 1;
const MAX_DEPTH = 64;

/** Id de objeto do Ring0 (tag 5). Na volta, ids chegam como number; use Id para mandar um id explícito. */
export class Id {
  readonly value: bigint;
  constructor(value: number | bigint) {
    const v = BigInt(value);
    if (v < 0n || v > 0xffff_ffff_ffff_ffffn) throw new RangeError(`id fora do intervalo u64: ${value}`);
    this.value = v;
  }
}

export type ModbValue = null | boolean | number | bigint | string | Id | ModbValue[] | { [key: string]: ModbValue | undefined };

class Writer {
  private buf = Buffer.allocUnsafe(256);
  private pos = 0;

  private ensure(n: number) {
    if (this.pos + n <= this.buf.length) return;
    let size = this.buf.length * 2;
    while (size < this.pos + n) size *= 2;
    const next = Buffer.allocUnsafe(size);
    this.buf.copy(next, 0, 0, this.pos);
    this.buf = next;
  }
  u8(v: number) {
    this.ensure(1);
    this.buf[this.pos++] = v;
  }
  u32(v: number) {
    this.ensure(4);
    this.buf.writeUInt32LE(v, this.pos);
    this.pos += 4;
  }
  i64(v: bigint) {
    this.ensure(8);
    this.buf.writeBigInt64LE(v, this.pos);
    this.pos += 8;
  }
  u64(v: bigint) {
    this.ensure(8);
    this.buf.writeBigUInt64LE(v, this.pos);
    this.pos += 8;
  }
  f64(v: number) {
    this.ensure(8);
    this.buf.writeDoubleLE(v, this.pos);
    this.pos += 8;
  }
  text(s: string) {
    const n = Buffer.byteLength(s, "utf8");
    this.u32(n);
    this.ensure(n);
    this.buf.write(s, this.pos, "utf8");
    this.pos += n;
  }
  done(): Buffer {
    return this.buf.subarray(0, this.pos);
  }
}

const I64_MIN = -(2n ** 63n);
const I64_MAX = 2n ** 63n - 1n;

function write(w: Writer, v: ModbValue | undefined, depth: number): void {
  if (depth > MAX_DEPTH) throw new RangeError("valor aninhado demais (máximo 64)");
  if (v === null || v === undefined) return w.u8(0);
  switch (typeof v) {
    case "boolean":
      w.u8(1);
      return w.u8(v ? 1 : 0);
    case "number":
      if (Number.isSafeInteger(v)) {
        w.u8(2);
        return w.i64(BigInt(v));
      }
      w.u8(3);
      return w.f64(v);
    case "bigint":
      if (v < I64_MIN || v > I64_MAX) throw new RangeError(`inteiro fora do intervalo i64: ${v}`);
      w.u8(2);
      return w.i64(v);
    case "string":
      w.u8(4);
      return w.text(v);
  }
  if (v instanceof Id) {
    w.u8(5);
    return w.u64(v.value);
  }
  if (Array.isArray(v)) {
    w.u8(6);
    w.u32(v.length);
    for (const item of v) write(w, item, depth + 1);
    return;
  }
  if (typeof v === "object" && (Object.getPrototypeOf(v) === Object.prototype || Object.getPrototypeOf(v) === null)) {
    const entries = Object.entries(v).filter(([, item]) => item !== undefined);
    w.u8(7);
    w.u32(entries.length);
    for (const [k, item] of entries) {
      w.text(k);
      write(w, item, depth + 1);
    }
    return;
  }
  throw new TypeError(`não dá para codificar ${Object.prototype.toString.call(v)} como Value do Ring0`);
}

export function encodeValue(v: unknown): Buffer {
  const w = new Writer();
  w.u8(VALUE_VERSION);
  write(w, v as ModbValue, 0);
  return w.done();
}

class Reader {
  pos = 0;
  readonly buf: Buffer;
  constructor(buf: Buffer) {
    this.buf = buf;
  }
  need(n: number) {
    if (this.pos + n > this.buf.length) throw new RangeError("Value truncado");
  }
  u8() {
    this.need(1);
    return this.buf[this.pos++]!;
  }
  u32() {
    this.need(4);
    const v = this.buf.readUInt32LE(this.pos);
    this.pos += 4;
    return v;
  }
  text() {
    const n = this.u32();
    this.need(n);
    const s = this.buf.toString("utf8", this.pos, this.pos + n);
    this.pos += n;
    return s;
  }
}

const toNumber = (v: bigint) => (v >= BigInt(Number.MIN_SAFE_INTEGER) && v <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(v) : v);

function read(r: Reader, depth: number): ModbValue {
  if (depth > MAX_DEPTH) throw new RangeError("valor aninhado demais (máximo 64)");
  const tag = r.u8();
  switch (tag) {
    case 0:
      return null;
    case 1:
      return r.u8() !== 0;
    case 2: {
      r.need(8);
      const v = r.buf.readBigInt64LE(r.pos);
      r.pos += 8;
      return toNumber(v);
    }
    case 3: {
      r.need(8);
      const v = r.buf.readDoubleLE(r.pos);
      r.pos += 8;
      return v;
    }
    case 4:
      return r.text();
    case 5: {
      r.need(8);
      const v = r.buf.readBigUInt64LE(r.pos);
      r.pos += 8;
      return toNumber(v);
    }
    case 6: {
      const n = r.u32();
      if (n > r.buf.length - r.pos) throw new RangeError("contagem de lista maior que os bytes restantes");
      const out: ModbValue[] = [];
      for (let i = 0; i < n; i++) out.push(read(r, depth + 1));
      return out;
    }
    case 7: {
      const n = r.u32();
      if (n > r.buf.length - r.pos) throw new RangeError("contagem de mapa maior que os bytes restantes");
      const out: Record<string, ModbValue> = Object.create(null);
      for (let i = 0; i < n; i++) {
        const k = r.text();
        out[k] = read(r, depth + 1);
      }
      return { ...out };
    }
  }
  throw new RangeError(`tag de Value desconhecida: ${tag}`);
}

/** Bytes vazios = null (a proc não devolveu nada). */
export function decodeValue(buf: Buffer): ModbValue {
  if (buf.length === 0) return null;
  if (buf[0] !== VALUE_VERSION) throw new RangeError(`versão de Value desconhecida: ${buf[0]}`);
  const r = new Reader(buf);
  r.pos = 1;
  const v = read(r, 0);
  if (r.pos !== buf.length) throw new RangeError("bytes sobrando depois do Value");
  return v;
}
