"""--stop-on-stdin-eof no servidor e no proxy (PLANO_PROPOSTAS_REGISTRY R3).

Uso: stdin_eof_test.py <notas-server> <modb-proxy> <clients/python>

Um supervisor sem sinais (o caso do Windows) abre o processo com um pipe na
entrada padrão e pede a parada limpa fechando o pipe: o processo imprime
`stopped` e sai com 0, e o que foi confirmado continua no banco. Sem a opção, o
fim da entrada não para nada. O socket local fica na pasta de trabalho (a do
build): no Windows, AF_UNIX sob %LOCALAPPDATA% (e o %TEMP% padrão) falha.
"""
import os
import subprocess
import sys
import time
from pathlib import Path

server_exe, proxy_exe, client_dir = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, client_dir)
from modb_client import Client  # noqa: E402

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print("FAIL:", message)


def start(args: list[str], stdin) -> tuple[subprocess.Popen, int]:
    proc = subprocess.Popen(args, stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    line = proc.stdout.readline()
    if not line.startswith("READY "):
        proc.kill()
        sys.exit(f"sem READY de {args[0]}: {line!r}")
    return proc, int(line.split()[1])


def stop_by_closing_stdin(proc: subprocess.Popen, name: str) -> None:
    proc.stdin.close()
    try:
        code = proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        check(False, f"{name}: não parou ao fechar a entrada padrão")
        return
    rest = proc.stdout.read()
    check(code == 0, f"{name}: sai com 0 ao fechar a entrada padrão (saiu com {code})")
    check("stopped" in rest, f"{name}: imprime 'stopped' (parada limpa): {rest!r}")


work = Path.cwd() / f"stdin-eof-{os.getpid()}"
work.mkdir(exist_ok=True)
db = str(work / "notas.modb")
base = [server_exe, "--db", db, "--log", "off"]

# Servidor com TCP direto: cria uma nota, para pelo fim da entrada, reabre.
server, port = start(base + ["--port", "0", "--stop-on-stdin-eof", "on"], subprocess.PIPE)
with Client("127.0.0.1", port) as c:
    c.call("notas.criar", {"texto": "antes da parada"})
stop_by_closing_stdin(server, "notas-server")

server, port = start(base + ["--port", "0"], subprocess.DEVNULL)
with Client("127.0.0.1", port) as c:
    notas = c.call("notas.listar", {})
check([n["texto"] for n in notas] == ["antes da parada"], f"a nota confirmada continua depois da parada: {notas}")
# Sem a opção, a entrada em /dev/null (fim imediato) não para o servidor.
time.sleep(1.0)
check(server.poll() is None, "sem --stop-on-stdin-eof, o fim da entrada não para o servidor")
server.kill()
server.wait()

# Proxy na frente de um engine local.
sock = str(work / "engine.sock")
engine, _ = start(base + ["--local", sock], subprocess.DEVNULL)
proxy, proxy_port = start([proxy_exe, "--engine", sock, "--port", "0", "--stop-on-stdin-eof", "on"], subprocess.PIPE)
with Client("127.0.0.1", proxy_port) as c:
    check(len(c.call("notas.listar", {})) == 1, "o proxy atende antes da parada")
stop_by_closing_stdin(proxy, "modb-proxy")
engine.kill()
engine.wait()

for leftover in work.iterdir():
    leftover.unlink()
work.rmdir()
if failures == 0:
    print("parada pelo fim da entrada padrão: servidor e proxy ok")
sys.exit(1 if failures else 0)
