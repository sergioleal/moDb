/**
 * O cliente Node contra um servidor de verdade: notas-server em --local atrás de um modb-proxy com token
 * (PLANO_PROPOSTAS_REGISTRY R6). Roda no CTest como modb.node_client:
 *
 *   MODB_NOTAS_SERVER=<notas-server> MODB_PROXY=<modb-proxy> node --test test/integration.test.ts
 *
 * O socket local fica na pasta de trabalho (a do build): no Windows, AF_UNIX sob %LOCALAPPDATA% (e o %TEMP%
 * padrão) falha. Os processos param limpo pelo fim da entrada padrão (--stop-on-stdin-eof on).
 */
import assert from "node:assert/strict";
import { type ChildProcess, execFileSync, spawn } from "node:child_process";
import { randomBytes } from "node:crypto";
import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { createInterface } from "node:readline";
import { after, before, test } from "node:test";
import { Connection, ErrorCode, Id, ModbConnectionError, ModbError, Pool } from "../src/index.ts";

const serverExe = process.env.MODB_NOTAS_SERVER;
const proxyExe = process.env.MODB_PROXY;
if (!serverExe || !proxyExe) throw new Error("defina MODB_NOTAS_SERVER e MODB_PROXY");

const work = join(process.cwd(), `node-client-${process.pid}`);
const sock = join(work, "engine.sock");
const secret = join(work, "link.secret");
const tokens = join(work, "proxy.tokens");
const token = randomBytes(24).toString("hex");

interface Running {
  child: ChildProcess;
  port: number;
}

/** Sobe um executável e espera a linha READY <porta>. */
function startReady(exe: string, args: string[]): Promise<Running> {
  const child = spawn(exe, [...args, "--stop-on-stdin-eof", "on"], { stdio: ["pipe", "pipe", "inherit"] });
  return new Promise((resolve, reject) => {
    const lines = createInterface({ input: child.stdout! });
    child.once("exit", (code) => reject(new Error(`${exe} saiu com ${code} antes do READY`)));
    lines.on("line", (line) => {
      if (line.startsWith("READY ")) resolve({ child, port: Number(line.split(" ")[1]) });
    });
  });
}

/** Parada limpa: fecha a entrada padrão e espera sair. */
function stop(r: Running): Promise<number | null> {
  return new Promise((resolve) => {
    if (r.child.exitCode !== null) return resolve(r.child.exitCode);
    r.child.once("exit", (code) => resolve(code));
    r.child.stdin!.end();
  });
}

let engine: Running;
let proxy: Running;
let proxyPort = 0;
const startProxy = (port: number) =>
  startReady(proxyExe, ["--engine", sock, "--secret-file", secret, "--tokens", tokens, "--port", String(port)]);

before(async () => {
  mkdirSync(work, { recursive: true });
  writeFileSync(secret, randomBytes(32).toString("hex"));
  writeFileSync(tokens, execFileSync(proxyExe, ["hash-token", token, "web", "leitor,escritor"]));
  engine = await startReady(serverExe, ["--db", join(work, "notas.modb"), "--local", sock, "--secret-file", secret, "--log", "off"]);
  proxy = await startProxy(0);
  proxyPort = proxy.port;
});

after(async () => {
  await stop(proxy);
  await stop(engine);
  rmSync(work, { recursive: true, force: true });
});

test("sem token, o proxy recusa com unauthenticated (73)", async () => {
  const conn = await Connection.open({ host: "127.0.0.1", port: proxyPort });
  try {
    assert.deepEqual(conn.info.mechanisms, ["token"]);
    await assert.rejects(conn.call("notas.listar"), (e: unknown) => e instanceof ModbError && e.code === ErrorCode.unauthenticated);
  } finally {
    conn.close();
  }
});

test("token errado é recusado na abertura", async () => {
  await assert.rejects(
    Connection.open({ host: "127.0.0.1", port: proxyPort, token: "nao-e-este" }),
    (e: unknown) => e instanceof ModbError && e.code === ErrorCode.unauthenticated,
  );
});

test("com token: principal, escrita, leitura e os erros das procs", async () => {
  const conn = await Connection.open({ host: "127.0.0.1", port: proxyPort, token });
  try {
    assert.equal(conn.info.principal, "web");
    const criada = (await conn.call("notas.criar", { texto: "via node" })) as { id: number };
    assert.equal(typeof criada.id, "number");
    assert.deepEqual(await conn.call("notas.ler", { id: new Id(criada.id) }), { id: criada.id, texto: "via node" });

    const codigo = (e: unknown) => (e instanceof ModbError ? e.code : -1);
    const erros = await Promise.allSettled([
      conn.call("notas.criar", { texto: "" }),
      conn.call("notas.ler", { id: new Id(999_999) }),
      conn.call("nao.existe"),
      conn.call("notas.criar", { texto: "via node" }),
    ]);
    assert.deepEqual(
      erros.map((r) => (r.status === "rejected" ? codigo(r.reason) : "ok")),
      [ErrorCode.invalid_argument, ErrorCode.record_not_found, ErrorCode.operation_not_found, ErrorCode.conflict],
    );
  } finally {
    conn.close();
  }
});

test("pool: chamadas em paralelo em várias conexões", async () => {
  const pool = new Pool({ host: "127.0.0.1", port: proxyPort, token, size: 4 });
  try {
    const listas = await Promise.all(Array.from({ length: 20 }, () => pool.call("notas.listar", {}, { read: true })));
    for (const l of listas) assert.ok(Array.isArray(l) && l.length >= 1);
  } finally {
    pool.close();
  }
});

test("queda do proxy: escrita não é repetida; leitura volta quando o proxy volta", async () => {
  const pool = new Pool({ host: "127.0.0.1", port: proxyPort, token, size: 2 });
  try {
    const antes = ((await pool.call("notas.listar", {}, { read: true })) as unknown[]).length;
    assert.equal(await stop(proxy), 0, "o proxy para limpo pelo fim da entrada padrão");

    await assert.rejects(pool.call("notas.criar", { texto: "durante a queda" }), ModbConnectionError);
    await assert.rejects(pool.call("notas.listar", {}, { read: true }), ModbConnectionError);

    proxy = await startProxy(proxyPort);
    const depois = (await pool.call("notas.listar", {}, { read: true })) as { texto: string }[];
    assert.equal(depois.length, antes, "a escrita que falhou não foi feita nem repetida");
    assert.ok(!depois.some((n) => n.texto === "durante a queda"));
  } finally {
    pool.close();
  }
});
