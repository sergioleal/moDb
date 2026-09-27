#!/usr/bin/env python3
"""Pacote de performance do moDb: monta, envia por SSH, roda e traz os resultados.

O mesmo pacote roda em Linux e em Windows (loadtests/perfpack/README.md):
binários prontos dos dois sistemas, o fonte do mesmo commit, as suítes e dois
runners equivalentes (run.sh e run.ps1). Os resultados voltam com sha256
conferido e são indexados na série histórica (load-history/series.jsonl), onde
`modb_load trend/report/gate` e o dashboard os comparam entre máquinas.

Cada máquina é um ambiente de loadtests/environments.json; host, usuário, SO e
diretório remoto saem de lá (bloco `perfpack`, ou `connection` para kind=ssh).
Senha de SSH nunca passa por aqui: use chave, ou o OpenSSH pergunta.

Uso:
    python scripts/perfpack.py build [--allow-dirty] [--no-linux] [--no-windows]
    python scripts/perfpack.py deploy --environment ID [--package ID]
    python scripts/perfpack.py run    --environment ID [--suite standard|smoke]
                                      [--repeat N] [--only SUBSTR] [--build] [--detach]
    python scripts/perfpack.py fetch  --environment ID [--no-index]
    python scripts/perfpack.py all    --environment ID [opções de run]
    python scripts/perfpack.py status --environment ID
    python scripts/perfpack.py import DIR... [--no-index]
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import glob
import hashlib
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / "build" / "perfpack"
STATE_FILE = DIST / "state.json"
CATALOG = ROOT / "loadtests" / "environments.json"
HISTORY = ROOT / "load-history" / "series.jsonl"
FETCHED = ROOT / "load-results" / "remote"
PACK_SOURCE = "loadtests/perfpack"

# Mesmas opções nos dois sistemas e na compilação de reserva no destino
# (run.sh/run.ps1 as leem de PACKAGE.env): o build de referência do profiling,
# sem testes nem o HTML do treinamento (que exigiria python no destino).
CMAKE_ARGS = ("-DCMAKE_BUILD_TYPE=RelWithDebInfo -DMODB_ENABLE_PROFILING=ON "
              "-DBUILD_TESTING=OFF -DMODB_BUILD_TRAINING=OFF")
# Só no binário Linux pronto: libstdc++/libgcc estáticas, para depender apenas
# da glibc do destino (>= a do Ubuntu 24.04, 2.39). O Windows leva as DLLs.
LINUX_STATIC = "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc"
WINDOWS_DLLS = ("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll")
SOURCE_EXCLUDES = ("docs-process", "*.mp3", "*.mp4", "*.wav")
# Os binários prontos vão sem a informação de depuração (126 MB -> poucos MB):
# o código de máquina é o mesmo, e a tabela de símbolos fica (perf ainda mostra
# nomes de função). A compilação de reserva no destino não passa por isso.
STRIP = "--strip-debug"
SAFE_ID = re.compile(r"^[A-Za-z0-9._-]+$")
# Um caso longo fica minutos sem imprimir nada; sem keepalive, NAT e firewalls no
# caminho derrubam a conexão ociosa e o `run` local trava sem saber que o remoto
# terminou (aconteceu com snapshot_hold 1M).
SSH_OPTS = ("-o", "ServerAliveInterval=30", "-o", "ServerAliveCountMax=6")


def die(message: str) -> "NoReturn":  # type: ignore[name-defined]
    sys.stderr.write(f"perfpack: {message}\n")
    raise SystemExit(2)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True,
                          text=True, encoding="utf-8").stdout.strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_state() -> dict:
    try:
        return json.loads(STATE_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_state(state: dict) -> None:
    STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    STATE_FILE.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")


# --- identidade do fonte -------------------------------------------------------

def source_identity(allow_dirty: bool) -> dict:
    """Commit, árvore e id do pacote para o estado atual do repositório."""
    head = git("rev-parse", "HEAD")
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    treeish = head
    if dirty:
        if not allow_dirty:
            die("há alterações não commitadas: commit antes (o pacote identifica o fonte pelo commit) "
                "ou use --allow-dirty (o resultado sai marcado git_dirty=true)")
        # `stash create` faz um commit solto com índice + árvore de trabalho sem
        # mexer em nada; arquivos não rastreados ficam de fora.
        treeish = git("stash", "create") or head
    tree = git("rev-parse", f"{treeish}^{{tree}}")
    package_id = f"modb-perfpack-{head[:12]}" + (f"-dirty-{tree[:8]}" if dirty else "")
    return {"commit": head, "branch": branch, "dirty": dirty, "treeish": treeish, "tree": tree,
            "package_id": package_id}


# --- build ---------------------------------------------------------------------

def find_windows_toolchain() -> dict[str, str]:
    """cmake, ninja e g++ do PATH; senão os do CLion instalado (a toolchain do projeto)."""
    found = {tool: shutil.which(tool) for tool in ("cmake", "ninja", "g++")}
    if all(found.values()):
        return found  # type: ignore[return-value]
    for clion in sorted(glob.glob(r"C:\Program Files\JetBrains\CLion*"), reverse=True):
        base = Path(clion) / "bin"
        candidates = {
            "cmake": base / "cmake" / "win" / "x64" / "bin" / "cmake.exe",
            "ninja": base / "ninja" / "win" / "x64" / "ninja.exe",
            "g++": base / "mingw" / "bin" / "g++.exe",
        }
        if all(p.exists() for p in candidates.values()):
            return {k: str(v) for k, v in candidates.items()}
    die("não achei cmake/ninja/g++ (nem no PATH nem num CLion instalado); use --no-windows")


def build_windows(archive: Path, tree: str, out_dir: Path) -> str:
    tools = find_windows_toolchain()
    work = DIST / "_build" / f"windows-{tree[:16]}"
    binary = work / "out" / "modb_load.exe"
    if not binary.exists():
        shutil.rmtree(work, ignore_errors=True)
        (work / "src").mkdir(parents=True)
        with tarfile.open(archive) as tar:
            tar.extractall(work / "src")
        env = dict(os.environ)
        env["PATH"] = os.pathsep.join(str(Path(t).parent) for t in tools.values()) + os.pathsep + env["PATH"]
        print("perfpack: compilando modb_load para Windows...")
        subprocess.run([tools["cmake"], "-S", str(work / "src"), "-B", str(work / "out"), "-G", "Ninja",
                        f"-DCMAKE_MAKE_PROGRAM={tools['ninja']}", f"-DCMAKE_CXX_COMPILER={tools['g++']}",
                        *CMAKE_ARGS.split()], check=True, env=env, stdout=subprocess.DEVNULL)
        subprocess.run([tools["cmake"], "--build", str(work / "out"), "--target", "modb_load"],
                       check=True, env=env, stdout=subprocess.DEVNULL)
    out_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, out_dir / "modb_load.exe")
    compiler_bin = Path(tools["g++"]).parent
    subprocess.run([str(compiler_bin / "strip.exe"), STRIP, str(out_dir / "modb_load.exe")], check=True)
    for dll in WINDOWS_DLLS:
        source = binary.parent / dll
        if not source.exists():
            source = compiler_bin / dll
        if not source.exists():
            die(f"DLL do runtime MinGW não encontrada: {dll}")
        shutil.copy2(source, out_dir / dll)
    version = subprocess.run([tools["g++"], "--version"], capture_output=True, text=True).stdout
    return version.splitlines()[0] if version else "g++ ?"


def wsl_command(distro: str) -> list[str]:
    # O script vai pelo stdin: o wsl.exe reinterpreta as aspas da linha de
    # comando do Windows e quebra aspas aninhadas.
    return ["wsl.exe", "-d", distro, "--", "bash", "-l", "-s"]


def feed_stdin(process: subprocess.Popen, script: str) -> None:
    # Em bytes, direto no buffer: o modo texto do Windows trocaria \n por \r\n,
    # e o bash leria cada CR como parte do comando.
    assert process.stdin is not None
    process.stdin.buffer.write(script.encode("utf-8"))  # type: ignore[attr-defined]
    process.stdin.close()


def wsl(distro: str, script: str, capture_output: bool = False, **kwargs) -> subprocess.CompletedProcess:
    kwargs.setdefault("text", True)
    pipe = subprocess.PIPE if capture_output else None
    process = subprocess.Popen(wsl_command(distro), stdin=subprocess.PIPE, stdout=pipe, stderr=pipe, **kwargs)
    feed_stdin(process, script)
    stdout, stderr = process.communicate()
    return subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)


def wsl_path(distro: str, windows_path: Path) -> str:
    result = wsl(distro, f"wslpath -a {shlex.quote(str(windows_path))}", capture_output=True, text=True)
    if result.returncode != 0:
        die(f"wslpath falhou para {windows_path}: {result.stderr.strip()}")
    return result.stdout.strip()


def build_linux(archive: Path, tree: str, out_dir: Path, distro: str) -> str:
    out_dir.mkdir(parents=True, exist_ok=True)
    src = f"$HOME/.cache/modb-perfpack/src-{tree[:16]}"
    out = f"$HOME/.cache/modb-perfpack/linux-{tree[:16]}"
    # cmake/ninja de um venv do usuário (sem sudo), se existir; senão o do PATH.
    script = f"""
set -euo pipefail
if [ -d "$HOME/.venvs/modb-build/bin" ]; then export PATH="$HOME/.venvs/modb-build/bin:$PATH"; fi
for t in cmake ninja g++; do command -v $t >/dev/null || {{ echo "falta $t no WSL" >&2; exit 3; }}; done
if [ ! -x "{out}/modb_load" ]; then
  rm -rf "{src}" "{out}"; mkdir -p "{src}"
  tar -xzf {shlex.quote(wsl_path(distro, archive))} -C "{src}"
  cmake -S "{src}" -B "{out}" -G Ninja {CMAKE_ARGS} "{LINUX_STATIC}" >/dev/null
  cmake --build "{out}" --target modb_load >/dev/null
fi
cp "{out}/modb_load" {shlex.quote(wsl_path(distro, out_dir))}/modb_load
strip {STRIP} {shlex.quote(wsl_path(distro, out_dir))}/modb_load
g++ --version | head -n 1
"""
    print(f"perfpack: compilando modb_load para Linux no WSL ({distro})...")
    result = wsl(distro, script, capture_output=True, text=True)
    if result.returncode != 0:
        die(f"build Linux no WSL falhou:\n{result.stderr[-3000:]}")
    return result.stdout.strip().splitlines()[-1]


def pack_files(treeish: str) -> dict[str, bytes]:
    """Runners, suítes e README do pacote, lidos do commit (nunca da árvore de trabalho)."""
    files: dict[str, bytes] = {}
    names = git("ls-tree", "-r", "--name-only", treeish, PACK_SOURCE).splitlines()
    if not names:
        die(f"{PACK_SOURCE} não existe em {treeish[:12]}: faça commit (ou git add) dos arquivos do pacote")
    for name in names:
        data = subprocess.run(["git", "show", f"{treeish}:{name}"], cwd=ROOT, check=True,
                              capture_output=True).stdout
        files[name[len(PACK_SOURCE) + 1:]] = data
    files["environments.json"] = subprocess.run(
        ["git", "show", f"{treeish}:loadtests/environments.json"], cwd=ROOT, check=True,
        capture_output=True).stdout
    return files


def cmd_build(args: argparse.Namespace) -> Path:
    ident = source_identity(args.allow_dirty)
    pid = ident["package_id"]
    staging = DIST / pid
    tarball = DIST / f"{pid}.tar.gz"
    shutil.rmtree(staging, ignore_errors=True)
    (staging / "src").mkdir(parents=True)

    archive = staging / "src" / "modb-src.tar.gz"
    # Só o que o build usa: sem docs-process (processo, 60 MB de áudio) nem mídia.
    git("archive", "--format=tar.gz", "-o", str(archive), ident["treeish"], "--", ".",
        *(f":(exclude){pattern}" for pattern in SOURCE_EXCLUDES))

    for rel, data in pack_files(ident["treeish"]).items():
        target = staging / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    toolchains = {}
    if not args.no_windows:
        toolchains["windows-x86_64"] = build_windows(archive, ident["tree"], staging / "bin" / "windows-x86_64")
    if not args.no_linux:
        if args.linux_binary:
            (staging / "bin" / "linux-x86_64").mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.linux_binary, staging / "bin" / "linux-x86_64" / "modb_load")
            toolchains["linux-x86_64"] = "fornecido (--linux-binary)"
        else:
            toolchains["linux-x86_64"] = build_linux(archive, ident["tree"], staging / "bin" / "linux-x86_64",
                                                     args.wsl_distro)

    built_at = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    env_lines = [
        f"PACKAGE_ID={pid}",
        f"MODB_GIT_COMMIT={ident['commit']}",
        f"MODB_GIT_BRANCH={ident['branch']}",
        f"MODB_GIT_DIRTY={1 if ident['dirty'] else 0}",
        f"MODB_GIT_TREE={ident['tree']}",
        f"MODB_PERFPACK_CMAKE_ARGS={CMAKE_ARGS}",
        f"BUILT_AT={built_at}",
    ]
    (staging / "PACKAGE.env").write_text("\n".join(env_lines) + "\n", encoding="utf-8", newline="\n")
    manifest = {
        "schema": "modb.perfpack.package", "schema_version": 1, "package_id": pid,
        "git_commit": ident["commit"], "git_branch": ident["branch"], "git_dirty": ident["dirty"],
        "git_tree": ident["tree"], "built_at": built_at, "cmake_args": CMAKE_ARGS,
        "linux_extra_link": LINUX_STATIC, "prebuilt_strip": STRIP, "source_excludes": list(SOURCE_EXCLUDES),
        "toolchains": toolchains,
        "files": [{"path": p.relative_to(staging).as_posix(), "sha256": sha256(p), "bytes": p.stat().st_size}
                  for p in sorted(staging.rglob("*")) if p.is_file()],
    }
    (staging / "PACKAGE.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    def executable(info: tarfile.TarInfo) -> tarfile.TarInfo:
        name = info.name.split("/", 1)[-1]
        info.mode = 0o755 if (info.isdir() or name in ("run.sh", "bin/linux-x86_64/modb_load")) else 0o644
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        return info

    with tarfile.open(tarball, "w:gz") as tar:
        tar.add(staging, arcname=pid, filter=executable)
    size = tarball.stat().st_size
    print(f"perfpack: pacote {tarball} ({size / 1e6:.1f} MB)")
    for platform, compiler in toolchains.items():
        print(f"  {platform}: {compiler}")
    state = load_state()
    state["last_built"] = pid
    save_state(state)
    return tarball


# --- ambientes e transportes ---------------------------------------------------

def load_environment(env_id: str, catalog: Path) -> dict:
    if not SAFE_ID.match(env_id):
        die(f"id de ambiente inválido: {env_id}")
    data = json.loads(catalog.read_text(encoding="utf-8"))
    for entry in data.get("environments", []):
        if entry.get("id") == env_id:
            return entry
    known = ", ".join(e.get("id", "?") for e in data.get("environments", []))
    die(f"ambiente '{env_id}' não está em {catalog}. Conhecidos: {known}")


class Transport:
    """Executa comandos e copia arquivos no destino; caminhos relativos ao home dele."""

    os_name = "linux"

    def __init__(self, remote_dir: str) -> None:
        self.remote_dir = remote_dir

    def run(self, script: str, capture: bool = False) -> subprocess.CompletedProcess:
        raise NotImplementedError

    def put(self, local: Path, remote: str) -> None:
        raise NotImplementedError

    def get_dir(self, remote: str, local_parent: Path) -> None:
        raise NotImplementedError

    # Comandos equivalentes nos dois sistemas.
    def home_path(self, rel: str) -> str:
        """Expressão de shell para <remote_dir>/<rel> no destino."""
        base = self.remote_dir
        if self.os_name == "windows":
            full = base if re.match(r"^[A-Za-z]:[\\/]", base) else f"$HOME\\{base}"
            return f"{full}\\{rel}".replace("/", "\\") if rel else full.replace("/", "\\")
        full = base if base.startswith("/") else f"$HOME/{base}"
        return f"{full}/{rel}" if rel else full

    def ps(self, script: str) -> list[str]:
        # Sem barra de progresso: ela sai como CLIXML no stdout de uma sessão remota.
        # E saída em UTF-8 (o padrão do PowerShell 5.1 é a página de código do console).
        script = ("$ProgressPreference = 'SilentlyContinue'; "
                  "[Console]::OutputEncoding = [Text.Encoding]::UTF8; $OutputEncoding = [Text.Encoding]::UTF8; "
                  + script)
        encoded = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
        return ["powershell", "-NoProfile", "-NonInteractive", "-OutputFormat", "Text", "-ExecutionPolicy", "Bypass",
                "-EncodedCommand", encoded]


class SshTransport(Transport):
    def __init__(self, remote_dir: str, os_name: str, host: str, user: str, port: int) -> None:
        super().__init__(remote_dir)
        self.os_name = os_name
        self.target = f"{user}@{host}" if user else host
        self.port = str(port)

    def remote_command(self, script: str) -> str:
        if self.os_name == "windows":
            return " ".join(self.ps(script))
        return "bash -c " + shlex.quote(script)

    def run(self, script: str, capture: bool = False) -> subprocess.CompletedProcess:
        return subprocess.run(["ssh", *SSH_OPTS, "-p", self.port, self.target, self.remote_command(script)],
                              capture_output=capture, text=True, encoding="utf-8", errors="replace")

    def scp_path(self, rel: str) -> str:
        # scp resolve caminho relativo a partir do home remoto, nos dois sistemas.
        return f"{self.target}:{self.remote_dir.rstrip('/')}/{rel}"

    def put(self, local: Path, remote: str) -> None:
        subprocess.run(["scp", "-q", *SSH_OPTS, "-P", self.port, str(local), self.scp_path(remote)], check=True)

    def get_dir(self, remote: str, local_parent: Path) -> None:
        subprocess.run(["scp", "-q", "-r", *SSH_OPTS, "-P", self.port, self.scp_path(remote), str(local_parent)], check=True)


class WslTransport(Transport):
    """Linux do WSL nesta máquina: o mesmo caminho do SSH, sem SSH."""

    def __init__(self, remote_dir: str, distro: str) -> None:
        super().__init__(remote_dir)
        self.distro = distro

    def run(self, script: str, capture: bool = False) -> subprocess.CompletedProcess:
        return wsl(self.distro, script, capture_output=capture, text=True, encoding="utf-8", errors="replace")

    def put(self, local: Path, remote: str) -> None:
        dest = self.home_path(remote)
        result = self.run(f'mkdir -p "$(dirname "{dest}")" && cp {shlex.quote(wsl_path(self.distro, local))} "{dest}"')
        if result.returncode != 0:
            die(f"cópia para o WSL falhou: {local}")

    def get_dir(self, remote: str, local_parent: Path) -> None:
        result = self.run(f'cp -r "{self.home_path(remote)}" {shlex.quote(wsl_path(self.distro, local_parent))}/')
        if result.returncode != 0:
            die(f"cópia do WSL falhou: {remote}")


class LocalTransport(Transport):
    """Windows desta máquina: o mesmo caminho do SSH, sem SSH."""

    os_name = "windows"

    def run(self, script: str, capture: bool = False) -> subprocess.CompletedProcess:
        return subprocess.run(self.ps(script), capture_output=capture, text=True, encoding="utf-8",
                              errors="replace", cwd=Path.home())

    def local(self, rel: str) -> Path:
        base = Path(self.remote_dir)
        return (base if base.is_absolute() else Path.home() / base) / rel

    def put(self, local: Path, remote: str) -> None:
        dest = self.local(remote)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(local, dest)

    def get_dir(self, remote: str, local_parent: Path) -> None:
        source = self.local(remote)
        shutil.copytree(source, local_parent / source.name)


def make_transport(entry: dict) -> Transport:
    cfg = dict(entry.get("perfpack") or {})
    conn = entry.get("connection") or {}
    kind = cfg.get("transport") or ("ssh" if entry.get("kind") == "ssh" else "")
    os_name = cfg.get("os") or entry.get("os_hint") or "linux"
    remote_dir = cfg.get("remote_dir") or "modb-perfpack"
    if os_name not in ("linux", "windows"):
        die(f"ambiente {entry['id']}: SO '{os_name}' não suportado (linux ou windows)")
    if kind == "ssh":
        host = cfg.get("host") or conn.get("host")
        if not host:
            die(f"ambiente {entry['id']}: sem host (connection.host ou perfpack.host)")
        user = cfg.get("user") or conn.get("default_user") or ""
        return SshTransport(remote_dir, os_name, host, user, int(cfg.get("port") or conn.get("port") or 22))
    if kind == "wsl":
        return WslTransport(remote_dir, cfg.get("distro") or "Ubuntu-24.04")
    if kind == "local":
        if os.name != "nt":
            die("transporte 'local' só existe no Windows; no Linux rode run.sh direto")
        return LocalTransport(remote_dir)
    die(f"ambiente {entry['id']}: sem bloco 'perfpack' com 'transport' (ssh, wsl ou local)")


# --- deploy / run / status / fetch ---------------------------------------------

def resolve_package(args: argparse.Namespace, env_id: str) -> str:
    if getattr(args, "package", None):
        return args.package
    state = load_state()
    pid = state.get("deployed", {}).get(env_id) or state.get("last_built")
    if not pid:
        die("nenhum pacote montado: rode 'perfpack.py build' (ou passe --package)")
    return pid


def cmd_deploy(args: argparse.Namespace) -> None:
    entry = load_environment(args.environment, args.environments_file)
    transport = make_transport(entry)
    pid = args.package or load_state().get("last_built")
    if not pid:
        die("nenhum pacote montado: rode 'perfpack.py build' antes")
    tarball = DIST / f"{pid}.tar.gz"
    if not tarball.exists():
        die(f"pacote não encontrado: {tarball}")
    print(f"perfpack: enviando {pid} para {args.environment} ({transport.os_name})...")
    if transport.os_name == "windows":
        prep = f"New-Item -ItemType Directory -Force -Path \"{transport.home_path('')}\" | Out-Null"
    else:
        prep = f'mkdir -p "{transport.home_path("")}"'
    if transport.run(prep).returncode != 0:
        die("não consegui criar o diretório remoto")
    transport.put(tarball, f"{pid}.tar.gz")
    # O catálogo vai à parte (e é reenviado a cada run): um ambiente cadastrado
    # depois do build continua valendo sem remontar o pacote.
    transport.put(args.environments_file, "environments.json")
    if transport.os_name == "windows":
        unpack = (f"$d = \"{transport.home_path('')}\"; "
                  f"if (Test-Path \"$d\\{pid}\") {{ Remove-Item -Recurse -Force \"$d\\{pid}\" }}; "
                  # O tar do Windows (bsdtar): um GNU tar do PATH (Git, MSYS) lê 'C:' como host.
                  f"& \"$env:SystemRoot\\System32\\tar.exe\" -xzf \"$d\\{pid}.tar.gz\" -C \"$d\"; "
                  f"if ($LASTEXITCODE) {{ exit $LASTEXITCODE }}; "
                  f"Remove-Item \"$d\\{pid}.tar.gz\"")
    else:
        unpack = (f'd="{transport.home_path("")}"; rm -rf "$d/{pid}" && tar -xzf "$d/{pid}.tar.gz" -C "$d" '
                  f'&& rm "$d/{pid}.tar.gz"')
    if transport.run(unpack).returncode != 0:
        die("falha ao extrair o pacote no destino")
    state = load_state()
    state.setdefault("deployed", {})[args.environment] = pid
    save_state(state)
    print(f"perfpack: {pid} instalado em {args.environment}:{transport.remote_dir}/{pid}")


def cmd_run(args: argparse.Namespace) -> str | None:
    entry = load_environment(args.environment, args.environments_file)
    transport = make_transport(entry)
    pid = resolve_package(args, args.environment)
    transport.put(args.environments_file, "environments.json")
    if transport.os_name == "windows":
        parts = [f"Set-Location \"{transport.home_path('')}\";",
                 f"& \".\\{pid}\\run.ps1\" -Environment '{args.environment}' -Suite '{args.suite}'",
                 "-ResultsDir results -EnvironmentsFile environments.json"]
        if args.repeat:
            parts.append(f"-Repeat {int(args.repeat)}")
        if args.only:
            parts.append(f"-Only '{args.only}'")
        if args.build:
            parts.append("-Build")
        if args.detach:
            die("--detach não é suportado em Windows: o OpenSSH do Windows encerra os processos da "
                "sessão ao desconectar. Rode sem --detach (ou direto na máquina) e depois 'fetch'.")
        script = " ".join(parts) + "; exit $LASTEXITCODE"
    else:
        cmd = ["bash", f"{pid}/run.sh", "--environment", args.environment, "--suite", args.suite,
               "--results-dir", "results", "--environments-file", "environments.json"]
        if args.repeat:
            cmd += ["--repeat", str(int(args.repeat))]
        if args.only:
            cmd += ["--only", args.only]
        if args.build:
            cmd.append("--build")
        line = " ".join(shlex.quote(c) for c in cmd)
        if args.detach:
            # Sobrevive à queda da conexão; o resultado fica em results/ até o fetch.
            stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            # Um comando por linha: `a && b & c` poria a lista inteira (cd incluído)
            # numa subshell em segundo plano, que morre com a sessão.
            line = (f"mkdir -p results || exit 1\n"
                    f"nohup setsid {line} > results/detached-{stamp}.log 2>&1 < /dev/null &\n"
                    f"echo \"perfpack: rodando em segundo plano (pid $!); log em results/detached-{stamp}.log\"")
        script = f'cd "{transport.home_path("")}" || exit 1\n{line}'
    print(f"perfpack: rodando {pid} em {args.environment} (suíte {args.suite})...")
    command, stdin_script = transport_command(transport, script)
    process = subprocess.Popen(
        command, stdin=subprocess.PIPE if stdin_script else subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", **transport_popen_kwargs(transport))
    if stdin_script:
        feed_stdin(process, stdin_script)
    run_name = None
    assert process.stdout is not None
    for out_line in process.stdout:
        # O PowerShell serializa o progresso da própria inicialização ("preparando
        # módulos") como CLIXML quando a saída não é um console: ruído, não dado.
        if out_line.startswith(("#< CLIXML", "<Objs ")):
            continue
        sys.stdout.write(out_line)
        sys.stdout.flush()
        match = re.match(r"^PERFPACK_RUN (\S+) (\S+)", out_line.strip())
        if match:
            run_name = match.group(1)
    code = process.wait()
    if code != 0 and not run_name:
        die(f"execução remota falhou (código {code})")
    return run_name


def transport_command(transport: Transport, script: str) -> tuple[list[str], str | None]:
    """Comando para executar `script` no destino e o que mandar no stdin dele."""
    if isinstance(transport, SshTransport):
        return ["ssh", *SSH_OPTS, "-p", transport.port, transport.target, transport.remote_command(script)], None
    if isinstance(transport, WslTransport):
        return wsl_command(transport.distro), script
    return transport.ps(script), None


def transport_popen_kwargs(transport: Transport) -> dict:
    return {"cwd": Path.home()} if isinstance(transport, LocalTransport) else {}


def run_environment(run_name: str) -> str:
    """Ambiente no nome de uma rodada: <AAAAMMDDTHHMMSSZ>-<ambiente>-<commit12>."""
    return run_name[17:-13] if len(run_name) > 30 else ""


def remote_runs(transport: Transport) -> list[tuple[str, str]]:
    """(nome, estado) de cada resultado no destino; estado = conteúdo de DONE ou 'running'."""
    if transport.os_name == "windows":
        script = (f"$r = \"{transport.home_path('results')}\"; if (Test-Path $r) {{ "
                  "Get-ChildItem -LiteralPath $r -Directory | Sort-Object Name | ForEach-Object { "
                  "$d = Join-Path $_.FullName 'DONE'; "
                  "if (Test-Path $d) { $s = (Get-Content $d -TotalCount 1) } else { $s = 'running' }; "
                  "Write-Output \"$($_.Name) $s\" } }")
    else:
        script = (f'r="{transport.home_path("results")}"; [ -d "$r" ] || exit 0; cd "$r"; '
                  'for d in */; do d="${d%/}"; [ "$d" = "*" ] && break; '
                  'if [ -f "$d/DONE" ]; then s=$(head -n 1 "$d/DONE"); else s=running; fi; echo "$d $s"; done')
    result = transport.run(script, capture=True)
    if result.returncode != 0:
        die(f"não consegui listar os resultados remotos: {result.stderr.strip()}")
    runs = []
    for line in result.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) == 2 and SAFE_ID.match(parts[0]):
            runs.append((parts[0], parts[1]))
    return runs


def cmd_status(args: argparse.Namespace) -> None:
    entry = load_environment(args.environment, args.environments_file)
    transport = make_transport(entry)
    local_dir = FETCHED / args.environment
    runs = remote_runs(transport)
    if not runs:
        print(f"perfpack: nenhum resultado em {args.environment}")
    for name, state in runs:
        if run_environment(name) != args.environment:
            fetched = f"de outro ambiente ({run_environment(name)}): o fetch ignora"
        else:
            fetched = "trazido" if (local_dir / name).exists() else "a trazer" if state != "running" else ""
        print(f"  {name}  {state}  {fetched}")


def verify_run(run_dir: Path) -> dict:
    """Confere manifest.json contra os arquivos (sha256, tamanho, nada faltando ou sobrando)."""
    manifest_path = run_dir / "manifest.json"
    if not manifest_path.exists() or not (run_dir / "DONE").exists():
        die(f"{run_dir}: resultado incompleto (sem manifest.json ou DONE)")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if manifest.get("schema") != "modb.perfpack.run":
        die(f"{manifest_path}: não é um manifesto de perfpack")
    listed = {f["path"]: f for f in manifest.get("files", [])}
    present = {p.relative_to(run_dir).as_posix() for p in run_dir.rglob("*") if p.is_file()}
    present -= {"manifest.json", "DONE"}
    if missing := sorted(set(listed) - present):
        die(f"{run_dir}: arquivos do manifesto ausentes: {missing}")
    if extra := sorted(present - set(listed)):
        die(f"{run_dir}: arquivos fora do manifesto: {extra}")
    for rel, info in listed.items():
        path = run_dir / rel
        if path.stat().st_size != info["bytes"] or sha256(path) != info["sha256"]:
            die(f"{run_dir}: {rel} não confere com o manifesto (transferência corrompida?)")
    return manifest


def find_modb_load(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    for candidate in ("build/relwithdebinfo/modb_load.exe", "build/relwithdebinfo/modb_load",
                      "build/release/modb_load.exe", "build/release/modb_load"):
        if (ROOT / candidate).exists():
            return ROOT / candidate
    die("modb_load local não encontrado para indexar: compile o preset relwithdebinfo ou passe --modb-load")


def index_run(run_dir: Path, manifest: dict, args: argparse.Namespace) -> None:
    raws = sorted(run_dir / f["path"] for f in manifest["files"]
                  if f["path"].startswith("raw/") and f["path"].endswith(".jsonl"))
    binary = find_modb_load(args.modb_load)
    totals = [0, 0, 0]  # novos, duplicatas, rejeitados
    for raw in raws:
        result = subprocess.run([str(binary), "index", str(raw), "--history-file", str(args.history_file),
                                 "--environments-file", str(args.environments_file)],
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        counts = re.search(r"(\d+) ponto\(s\) novo\(s\), (\d+) duplicata\(s\), (\d+) rejeitado", result.stdout)
        if not counts:
            die(f"indexação de {raw.name} falhou (código {result.returncode}):\n{result.stdout}{result.stderr}")
        for i in range(3):
            totals[i] += int(counts.group(i + 1))
        if result.stderr.strip():
            sys.stderr.write(f"perfpack: {raw.name}:\n{result.stderr}")
    print(f"perfpack: série histórica: {totals[0]} ponto(s) novo(s), {totals[1]} duplicata(s), "
          f"{totals[2]} rejeitado(s) ({len(raws)} arquivo(s) de {run_dir.name})")


def store_and_index(source: Path, env_id: str | None, args: argparse.Namespace) -> None:
    manifest = verify_run(source)
    env_id = env_id or manifest["environment"]
    dest = FETCHED / env_id / manifest["run_name"]
    if dest.exists():
        print(f"perfpack: {manifest['run_name']} já estava em {dest.relative_to(ROOT)}")
    else:
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, dest)
        print(f"perfpack: {manifest['run_name']} ({manifest['status']}, {manifest['executions']} execuções, "
              f"{manifest['failures']} falha(s)) -> {dest.relative_to(ROOT)}")
    if not args.no_index:
        index_run(dest, manifest, args)


def cmd_fetch(args: argparse.Namespace) -> None:
    entry = load_environment(args.environment, args.environments_file)
    transport = make_transport(entry)
    # Um mesmo diretório remoto pode ter rodadas feitas com outro --environment
    # (à mão, por exemplo): elas pertencem ao ambiente delas, não a este.
    everything = remote_runs(transport)
    runs = [(name, state) for name, state in everything if run_environment(name) == args.environment]
    if other := [name for name, _ in everything if run_environment(name) != args.environment]:
        print(f"perfpack: ignorando rodadas de outro ambiente: {', '.join(other)}")
    pending = [name for name, state in runs if state != "running" and not (FETCHED / args.environment / name).exists()]
    running = [name for name, state in runs if state == "running"]
    if running:
        print(f"perfpack: ainda rodando (fica para o próximo fetch): {', '.join(running)}")
    if not pending:
        print("perfpack: nada novo para trazer")
        return
    for name in pending:
        with tempfile.TemporaryDirectory(prefix="perfpack-") as tmp:
            transport.get_dir(f"results/{name}", Path(tmp))
            store_and_index(Path(tmp) / name, args.environment, args)
    # A descrição da máquina lista as rodadas: atualiza sem tocar na máquina.
    if write_machine_doc(entry):
        print(f"perfpack: {MACHINES.relative_to(ROOT)}/{args.environment}.md atualizado com as rodadas")


def cmd_import(args: argparse.Namespace) -> None:
    for directory in args.dirs:
        store_and_index(Path(directory).resolve(), None, args)


MACHINES = ROOT / "load-history" / "machines"


def parse_sections(text: str) -> dict[str, str]:
    sections: dict[str, str] = {}
    current = None
    for line in text.splitlines():
        if line.startswith("### "):
            current = line[4:].strip()
            sections[current] = ""
        elif current is not None:
            sections[current] += line + "\n"
    return {k: v.strip() for k, v in sections.items()}


def kv(text: str, sep: str = ":") -> dict[str, str]:
    out = {}
    for line in text.splitlines():
        if sep in line:
            key, value = line.split(sep, 1)
            out[key.strip()] = value.strip().strip('"')
    return out


def machine_summary(sec: dict[str, str]) -> dict:
    os_rel = kv(sec.get("os_release", ""), "=")
    cpu = kv(sec.get("lscpu", ""))
    dmi = kv(sec.get("dmi", ""))
    cloud = kv(sec.get("cloud_metadata", ""))
    mem = kv(sec.get("meminfo", ""))
    steal = kv(sec.get("cpu_steal", ""))
    freq = kv(sec.get("cpufreq", ""))
    mount = sec.get("work_filesystem", "").splitlines()
    vulns = kv(sec.get("cpu_vulnerabilities", ""))
    mitigated = sorted(k for k, v in vulns.items() if v.lower().startswith("mitigation"))
    vulnerable = sorted(k for k, v in vulns.items() if v.lower().startswith("vulnerable"))
    return {
        "os": os_rel.get("PRETTY_NAME", ""),
        "kernel": (sec.get("uname", "").split() + ["", "", ""])[2],
        "glibc": sec.get("glibc", ""),
        "virtualization": sec.get("virtualization", ""),
        "hypervisor_vendor": cpu.get("Hypervisor vendor", ""),
        "platform": f"{dmi.get('sys_vendor', '')} {dmi.get('product_name', '')}".strip(),
        "cloud_region": cloud.get("region", ""),
        "cloud_droplet_id": cloud.get("id", ""),
        "cpu_model": cpu.get("Model name", ""),
        "vcpus": cpu.get("CPU(s)", ""),
        "threads_per_core": cpu.get("Thread(s) per core", ""),
        "cores_per_socket": cpu.get("Core(s) per socket", ""),
        "sockets": cpu.get("Socket(s)", ""),
        "numa_nodes": cpu.get("NUMA node(s)", ""),
        "cpu_mhz": cpu.get("CPU MHz", "") or cpu.get("BogoMIPS", "") and f"BogoMIPS {cpu.get('BogoMIPS')}",
        "l1d": cpu.get("L1d cache", ""), "l2": cpu.get("L2 cache", ""),
        "l3": cpu.get("L3 cache", "") or "não exposto pela VM",
        "cpu_flags": sec.get("cpu_flags", ""),
        "governor": freq.get("governor", ""),
        "clocksource": sec.get("clocksource", ""),
        "steal_pct_5s": steal.get("steal_pct_5s", ""),
        "ram": mem.get("MemTotal", ""),
        "swap": mem.get("SwapTotal", ""),
        "transparent_hugepage": sec.get("transparent_hugepage", ""),
        "block_queue": sec.get("block_queue", ""),
        "work_mount": mount[-1] if mount else "",
        "work_df": mount[1] if len(mount) > 1 else "",
        "mitigations": mitigated,
        "vulnerable": vulnerable,
    }


def kib_to_gib(text: str) -> str:
    match = re.match(r"(\d+)\s*kB", text or "")
    return f"{int(match.group(1)) / 2**20:.1f} GiB" if match else (text or "—")


def environment_runs(env_id: str) -> list[dict]:
    """Rodadas deste ambiente já trazidas, pelos manifestos em load-results/remote/<ambiente>/."""
    runs = []
    for manifest in sorted((FETCHED / env_id).glob("*/manifest.json")):
        try:
            m = json.loads(manifest.read_text(encoding="utf-8-sig"))
        except ValueError:
            continue
        runs.append(m)
    return runs


def machine_markdown(env: dict, summary: dict, collected_at: str, sections: dict[str, str]) -> str:
    s = summary
    rows = [
        ("Ambiente", f"`{env['id']}` — {env.get('label', '')}"),
        ("host_class / device_class", f"`{env.get('host_class', '')}` / `{env.get('device_class', '')}`"),
        ("Coletado em", collected_at),
        ("Plataforma", f"{s['platform']} ({s['virtualization']}; hipervisor {s['hypervisor_vendor'] or '?'})"),
        ("Região / droplet", f"{s['cloud_region'] or '—'} / {s['cloud_droplet_id'] or '—'}"),
        ("SO", f"{s['os']}, kernel {s['kernel']}, {s['glibc']}"),
        ("CPU", f"{s['cpu_model']}"),
        ("vCPUs", f"{s['vcpus']} ({s['sockets']} socket × {s['cores_per_socket']} núcleos × "
                  f"{s['threads_per_core']} thread/núcleo; {s['numa_nodes']} nó NUMA)"),
        ("Caches", f"L1d {s['l1d']} · L2 {s['l2']} · L3 {s['l3']}"),
        ("Extensões", s["cpu_flags"]),
        ("Frequência", f"governor {s['governor']}; clocksource {s['clocksource']}"),
        ("CPU steal (5 s ocioso)", f"{s['steal_pct_5s']}%"),
        ("Memória", f"{kib_to_gib(s['ram'])} (swap {kib_to_gib(s['swap'])}; THP {s['transparent_hugepage']})"),
        ("Disco (fila)", s["block_queue"].replace("\n", "<br>")),
        ("FS do diretório de trabalho", f"{s['work_df']}<br>{s['work_mount']}"),
        ("Mitigações ativas", ", ".join(s["mitigations"]) or "—"),
        ("Vulnerável (sem mitigação)", ", ".join(s["vulnerable"]) or "—"),
    ]
    lines = [
        f"# Máquina `{env['id']}`",
        "",
        "Descrição da máquina em que os resultados deste ambiente foram medidos",
        f"(`environment = {env['id']}` em [`series.jsonl`](../series.jsonl) e em",
        f"`load-results/remote/{env['id']}/`). Gerado por `scripts/perfpack.py machine`",
        "a partir de `loadtests/perfpack/machine-info.sh`; a saída crua de cada comando",
        f"está em [`{env['id']}.json`]({env['id']}.json). Se a máquina mudar (kernel, plano, droplet",
        "recriado), gere de novo: o histórico do git guarda as versões anteriores.",
        "",
        "| | |",
        "|---|---|",
        *[f"| {k} | {v} |" for k, v in rows],
        "",
        "## Como ler os números deste ambiente",
        "",
        "- O disco (fila, cache de escrita, sistema de arquivos) limita os casos de",
        "  commit pequeno (`mixed_oltp`, `snapshot_hold`), um `fsync` por commit; os com",
        "  lote de 1000 objetos (`create_*`, `crud_full`) quase não o sentem.",
        "- A coleta só lê o sistema: nada aqui foi medido com carga.",
        "- `steal` perto de zero confirma CPU dedicada; se subir numa coleta futura, os",
        "  resultados do período ficam suspeitos de vizinhos barulhentos.",
        "- Compare commits **dentro** deste ambiente; entre ambientes, só a forma das",
        "  curvas, nunca o valor absoluto.",
        "",
        "## Rodadas medidas nesta máquina",
        "",
        "Cada rodada está em `load-results/remote/" + env["id"] + "/<rodada>/` (brutos, fora do git)",
        "e seus pontos em `series.jsonl` (`run_id` de cada arquivo em `raw/`). Esta lista é",
        "regerada a cada `perfpack.py machine` e a cada `fetch`.",
        "",
        "| rodada | commit | suíte | execuções | falhas | início (UTC) | binário |",
        "|---|---|---|---|---|---|---|",
        *[f"| `{r['run_name']}` | `{r['git_commit'][:12]}`{' (dirty)' if r.get('git_dirty') else ''} | "
          f"{r['suite']} × {r['repeat']} | {r['executions']} | {r['failures']} | {r['started_at']} | "
          f"{r['binary_origin']} |" for r in environment_runs(env["id"])],
        "",
        "## Saída crua",
        "",
    ]
    for name, body in sections.items():
        lines += [f"<details><summary><code>{name}</code></summary>", "", "```", body, "```", "", "</details>", ""]
    return "\n".join(lines)


def write_machine_doc(entry: dict) -> bool:
    """(Re)gera <ambiente>.md a partir do .json coletado; False se nunca foi coletado."""
    source = MACHINES / f"{entry['id']}.json"
    if not source.exists():
        return False
    data = json.loads(source.read_text(encoding="utf-8"))
    summary = machine_summary(data["sections"])  # recalculado: o resumo pode ter melhorado
    (MACHINES / f"{entry['id']}.md").write_text(
        machine_markdown(entry, summary, data.get("collected_at", ""), data["sections"]), encoding="utf-8")
    return True


def cmd_machine(args: argparse.Namespace) -> None:
    entry = load_environment(args.environment, args.environments_file)
    transport = make_transport(entry)
    if transport.os_name != "linux":
        die("o coletor de máquina ainda só existe para Linux (machine-info.sh)")
    collector = (ROOT / PACK_SOURCE / "machine-info.sh").read_text(encoding="utf-8").replace("\r\n", "\n")
    script = f'set -- "{transport.home_path("")}"\n' + collector.split("\n", 1)[1]
    print(f"perfpack: coletando a descrição de {args.environment} (só leitura; ~5 s de amostra de steal)...")
    result = transport.run(script, capture=True)
    if result.returncode != 0 or "### lscpu" not in result.stdout:
        die(f"coleta falhou (código {result.returncode}):\n{result.stdout[-2000:]}{result.stderr[-2000:]}")
    sections = parse_sections(result.stdout)
    summary = machine_summary(sections)
    collected_at = sections.get("collected_at", "")
    MACHINES.mkdir(parents=True, exist_ok=True)
    data = {"schema": "modb.perfpack.machine", "schema_version": 1, "environment": args.environment,
            "collected_at": collected_at, "summary": summary, "sections": sections}
    (MACHINES / f"{args.environment}.json").write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n",
                                                        encoding="utf-8")
    write_machine_doc(entry)
    print(f"perfpack: {MACHINES.relative_to(ROOT)}/{args.environment}.md e .json")
    print(f"  {summary['cpu_model']} · {summary['vcpus']} vCPUs · {summary['ram']} · "
          f"steal {summary['steal_pct_5s']}%")


def cmd_all(args: argparse.Namespace) -> None:
    args.package = None
    cmd_deploy(args)
    run_name = cmd_run(args)
    if args.detach:
        print("perfpack: rodando destacado; traga os resultados depois com 'fetch'")
        return
    cmd_fetch(args)
    if run_name:
        print(f"perfpack: pronto — {run_name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--environments-file", type=Path, default=CATALOG)
    parser.add_argument("--history-file", type=Path, default=HISTORY)
    parser.add_argument("--modb-load", help="modb_load local usado para indexar (padrão: build/relwithdebinfo)")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("build", help="monta o pacote do commit atual")
    p.add_argument("--allow-dirty", action="store_true")
    p.add_argument("--no-linux", action="store_true")
    p.add_argument("--no-windows", action="store_true")
    p.add_argument("--linux-binary", help="usa este modb_load Linux em vez de compilar no WSL")
    p.add_argument("--wsl-distro", default="Ubuntu-24.04")
    p.set_defaults(func=cmd_build)

    def env_arg(p: argparse.ArgumentParser) -> None:
        p.add_argument("--environment", "-e", required=True, help="id em loadtests/environments.json")

    def run_args(p: argparse.ArgumentParser) -> None:
        p.add_argument("--suite", default="standard", help="standard, smoke ou caminho de suíte no pacote")
        p.add_argument("--repeat", type=int)
        p.add_argument("--only", help="só casos cujo id contém este texto")
        p.add_argument("--build", action="store_true", help="compila no destino em vez do binário pronto")
        p.add_argument("--detach", action="store_true", help="(Linux) roda em segundo plano; traga com fetch")

    p = sub.add_parser("deploy", help="envia e extrai o pacote no destino")
    env_arg(p)
    p.add_argument("--package")
    p.set_defaults(func=cmd_deploy)

    p = sub.add_parser("run", help="roda uma suíte no destino")
    env_arg(p)
    p.add_argument("--package")
    run_args(p)
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("status", help="lista os resultados no destino")
    env_arg(p)
    p.set_defaults(func=cmd_status)

    for name, func, help_text in (("fetch", cmd_fetch, "traz, confere e indexa os resultados novos"),):
        p = sub.add_parser(name, help=help_text)
        env_arg(p)
        p.add_argument("--no-index", action="store_true")
        p.set_defaults(func=func)

    p = sub.add_parser("all", help="deploy + run + fetch")
    env_arg(p)
    run_args(p)
    p.add_argument("--no-index", action="store_true")
    p.set_defaults(func=cmd_all)

    p = sub.add_parser("machine", help="descreve a máquina (load-history/machines/<ambiente>.md/.json)")
    env_arg(p)
    p.set_defaults(func=cmd_machine)

    p = sub.add_parser("import", help="confere e indexa resultados copiados à mão")
    p.add_argument("dirs", nargs="+")
    p.add_argument("--no-index", action="store_true")
    p.set_defaults(func=cmd_import)

    args = parser.parse_args()
    # Console Windows em cp1252: um caractere fora dela não pode derrubar a execução.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
    args.environments_file = args.environments_file.resolve()
    args.history_file = args.history_file.resolve()
    args.func(args)


if __name__ == "__main__":
    main()
