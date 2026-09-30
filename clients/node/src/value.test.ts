import assert from "node:assert/strict";
import { test } from "node:test";
import { decodeValue, encodeValue, Id } from "./value.ts";

test("exemplo do protocolo: {id: id 5}", () => {
  // PROTOCOLO_CLIENTES.md §4: 01 07 01000000 02000000 6964 05 0500000000000000
  const bytes = encodeValue({ id: new Id(5) });
  assert.equal(bytes.toString("hex"), "0107010000000200000069640505" + "00000000000000");
  assert.deepEqual(decodeValue(bytes), { id: 5 });
});

test("ida e volta de todos os tipos", () => {
  const v = { n: null, t: true, f: false, i: -42, big: 2n ** 60n, r: 1.5, s: "ação ☕", l: [1, "a", [null]], m: { a: { b: 2 } } };
  assert.deepEqual(decodeValue(encodeValue(v)), { ...v, big: 2n ** 60n });
});

test("chaves com undefined são omitidas", () => {
  assert.deepEqual(decodeValue(encodeValue({ a: 1, b: undefined })), { a: 1 });
});

test("bytes vazios são null; lixo depois do valor é erro", () => {
  assert.equal(decodeValue(Buffer.alloc(0)), null);
  assert.throws(() => decodeValue(Buffer.from([1, 0, 0])), /sobrando/);
  assert.throws(() => decodeValue(Buffer.from([2, 0])), /versão/);
});

test("aninhamento acima de 64 é recusado", () => {
  let deep: unknown = 1;
  for (let i = 0; i < 70; i++) deep = [deep];
  assert.throws(() => encodeValue(deep as never), /aninhado/);
});
