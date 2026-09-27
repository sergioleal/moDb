"""Cliente Python de referência contra o notas-server de verdade (ADR-026):
TCP e anel de memória compartilhada, valores, erros e o anel entre processos.

    python python_client_test.py <notas-server> <diretório de clients/python>
"""

import os
import subprocess
import sys
import tempfile
import time

server_exe, client_dir = os.path.abspath(sys.argv[1]), sys.argv[2]
sys.path.insert(0, client_dir)
from modb_client import Client, Id, ModbError, decode_value, encode_value  # noqa: E402

total = falhas = 0


def verificar(ok, o_que):
    global total, falhas
    total += 1
    if not ok:
        falhas += 1
        print("FALHOU:", o_que, file=sys.stderr)


def erro(fn):
    try:
        fn()
    except ModbError as e:
        return e
    return None


# --- Value v1 sem servidor ---
for v in [None, True, False, 0, -7, 2**62, 1.5, "", "café", [1, "a", None], {"a": {"b": [Id(3)]}}, Id(42)]:
    verificar(decode_value(encode_value(v)) == v and type(decode_value(encode_value(v))) is type(v),
              f"Value ida e volta: {v!r}")
verificar(encode_value({"id": Id(5)}) == bytes([1, 7, 1, 0, 0, 0, 2, 0, 0, 0]) + b"id" + bytes([5, 5, 0, 0, 0, 0, 0, 0, 0]),
          "bytes do Value iguais aos da especificação")

db = os.path.join(tempfile.gettempdir(), f"modb-python-client-{os.getpid()}-{time.time_ns()}.modb")
proc = subprocess.Popen([server_exe, "--db", db, "--port", "0", "--log", "off"], stdout=subprocess.PIPE, text=True)
try:
    linha = proc.stdout.readline().strip()
    verificar(linha.startswith("READY "), "o servidor avisa READY <porta>")
    porta = int(linha.split()[1])

    for modo in ("tcp", "anel"):
        with Client("127.0.0.1", porta) as c:
            verificar(c.server_minor >= 1, f"[{modo}] o servidor anuncia minor >= 1")
            if modo == "anel":
                c.attach_shared_memory()
                verificar(c.shm is not None, "[anel] attach_shared_memory")
            criada = c.call("notas.criar", {"texto": f"nota pelo {modo}"})
            verificar(isinstance(criada.get("id"), Id), f"[{modo}] notas.criar devolve um Id")
            lida = c.call("notas.ler", {"id": criada["id"]})
            verificar(lida == {"id": criada["id"], "texto": f"nota pelo {modo}"}, f"[{modo}] notas.ler")
            verificar(c.call("notas.ler", {"id": int(criada["id"])})["texto"] == f"nota pelo {modo}",
                      f"[{modo}] id como int comum também serve")
            e = erro(lambda: c.call("notas.criar", {"texto": ""}))
            verificar(e is not None and e.message == "a nota não pode ser vazia", f"[{modo}] erro de regra com mensagem")
            e = erro(lambda: c.call("notas.criar", {"texto": f"nota pelo {modo}"}))
            verificar(e is not None and "já existe" in e.message, f"[{modo}] conflict")
            e = erro(lambda: c.call("nao.existe"))
            verificar(e is not None and "not found" in e.message, f"[{modo}] proc desconhecida")
            procs = c.call("sys.procs")
            verificar(any(p["name"] == "notas.criar" for p in procs), f"[{modo}] sys.procs")
            certas = sum(1 for _ in range(300) if c.call("notas.listar", {"contem": modo}) and True)
            verificar(certas == 300, f"[{modo}] 300 chamadas seguidas")

    # O que cada cliente escreveu, o outro vê (mesmo banco, outro transporte).
    with Client("127.0.0.1", porta) as c:
        todas = c.call("notas.listar")
        verificar(sorted(n["texto"] for n in todas) == ["nota pelo anel", "nota pelo tcp"],
                  "as duas notas estão no banco")

    # Servidor morre com o cliente no anel: erro, não espera eterna.
    c = Client("127.0.0.1", porta)
    c.attach_shared_memory()
    proc.kill()
    proc.wait()
    inicio = time.monotonic()
    e = erro(lambda: c.call("notas.listar"))
    verificar(e is not None and time.monotonic() - inicio < 10,
              "servidor morto com o cliente no anel: erro logo, pela linha de vida TCP")
    c.close()
finally:
    if proc.poll() is None:
        proc.kill()
    for f in (db, db + ".wal"):
        try:
            os.remove(f)
        except OSError:
            pass

print(f"{total - falhas}/{total} verificações ok")
sys.exit(0 if falhas == 0 else 1)
