"""Cliente de referência do moDb em Python (só biblioteca padrão).

Chama stored procedures de um servidor moDb (modb_add_server) pelo protocolo
nativo -- TCP -- ou, na mesma máquina, pelo anel de memória compartilhada
(ADR-026), sem syscall por chamada. A especificação byte a byte está em
docs/PROTOCOLO_CLIENTES.md; este arquivo é a implementação de exemplo para
quem for escrever um cliente em outra linguagem.

    from modb_client import Client, Id
    with Client("127.0.0.1", 7474) as c:
        c.attach_shared_memory()            # opcional, mesma máquina
        # atrás de um modb-proxy com --tokens: Client(..., token="...")
        nota = c.call("notas.criar", {"texto": "comprar café"})
        print(c.call("notas.ler", {"id": nota["id"]}))
        # minor 3 (ADR-029): em nome de quem (token com a role delegate) e
        # uma chave que torna a escrita repetível
        c.call("notas.criar", {"texto": "x"}, acting_as="user:24", idempotency_key="uuid-...")

Valores: None, bool, int, float, str, list, dict (chaves str) e Id (id de
objeto; uma subclasse de int).
"""

from __future__ import annotations

import ctypes
import mmap
import os
import select
import socket
import struct
import sys
import time

__all__ = ["Client", "Id", "ModbError", "encode_value", "decode_value"]

PROTOCOL_MAJOR = 1
PROTOCOL_MINOR = 3
MAX_FRAME_BYTES = 16 * 1024 * 1024

T_OP_CALL, T_OP_RESULT = 9, 10
T_HELLO, T_HELLO_OK = 1, 2
T_SHM_ATTACH, T_SHM_ATTACH_OK = 15, 16
T_AUTHENTICATE, T_AUTHENTICATE_OK = 17, 18

class Id(int):
    """Id de objeto do moDb (tag 5 no Value); em JSON vira número."""

    def __repr__(self) -> str:
        return f"Id({int(self)})"


# Códigos de erro (modb::ErrorCode) que um cliente de procs encontra; os números
# são estáveis (docs/COMPATIBILIDADE.md) e o teste modb.error_codes os confere
# contra o include/modb/error.hpp.
ERROR_CODES = {
    "invalid_argument": 1,
    "value_too_large": 21,
    "record_not_found": 30,
    "snapshot_conflict": 44,
    "protocol_error": 45,
    "frame_too_large": 46,
    "connection_closed": 47,
    "operation_not_found": 48,
    "incompatible_protocol_version": 50,
    "conflict": 70,
    "internal_error": 71,
    "operation_timeout": 72,
    "unauthenticated": 73,
    "permission_denied": 74,
}


class ModbError(Exception):
    def __init__(self, code: int, message: str, detail=None):
        super().__init__(f"{message} (code {code})")
        self.code = code
        self.message = message
        # Minor 3: o Value que a proc mandou com o erro (por convenção
        # {"reason": ..., "field": ...}); None se não mandou.
        self.detail = detail


# --- Value v1 ---------------------------------------------------------------------
#
#   versão u8 (=1) | valor
#   valor = tag u8 | conteúdo:  0 null | 1 bool u8 | 2 i64 | 3 f64 | 4 u32+utf8 |
#           5 id u64 | 6 u32 + valores | 7 u32 + (u32+chave utf8, valor)...

def _enc(v, out: bytearray) -> None:
    if v is None:
        out.append(0)
    elif v is True or v is False:
        out += bytes((1, 1 if v else 0))
    elif isinstance(v, Id):
        out.append(5)
        out += struct.pack("<Q", int(v))
    elif isinstance(v, int):
        out.append(2)
        out += struct.pack("<q", v)
    elif isinstance(v, float):
        out.append(3)
        out += struct.pack("<d", v)
    elif isinstance(v, str):
        b = v.encode("utf-8")
        out.append(4)
        out += struct.pack("<I", len(b)) + b
    elif isinstance(v, (list, tuple)):
        out.append(6)
        out += struct.pack("<I", len(v))
        for item in v:
            _enc(item, out)
    elif isinstance(v, dict):
        out.append(7)
        out += struct.pack("<I", len(v))
        for k, item in v.items():
            kb = str(k).encode("utf-8")
            out += struct.pack("<I", len(kb)) + kb
            _enc(item, out)
    else:
        raise TypeError(f"cannot encode {type(v).__name__} as a moDb value")


def encode_value(v) -> bytes:
    out = bytearray((1,))
    _enc(v, out)
    return bytes(out)


def _dec(buf, pos: int, depth: int):
    if depth > 64:
        raise ValueError("value nested too deeply")
    tag = buf[pos]
    pos += 1
    if tag == 0:
        return None, pos
    if tag == 1:
        return buf[pos] != 0, pos + 1
    if tag == 2:
        return struct.unpack_from("<q", buf, pos)[0], pos + 8
    if tag == 3:
        return struct.unpack_from("<d", buf, pos)[0], pos + 8
    if tag == 4:
        (n,) = struct.unpack_from("<I", buf, pos)
        pos += 4
        return bytes(buf[pos:pos + n]).decode("utf-8"), pos + n
    if tag == 5:
        return Id(struct.unpack_from("<Q", buf, pos)[0]), pos + 8
    if tag == 6:
        (n,) = struct.unpack_from("<I", buf, pos)
        pos += 4
        items = []
        for _ in range(n):
            item, pos = _dec(buf, pos, depth + 1)
            items.append(item)
        return items, pos
    if tag == 7:
        (n,) = struct.unpack_from("<I", buf, pos)
        pos += 4
        m = {}
        for _ in range(n):
            (kn,) = struct.unpack_from("<I", buf, pos)
            pos += 4
            k = bytes(buf[pos:pos + kn]).decode("utf-8")
            pos += kn
            m[k], pos = _dec(buf, pos, depth + 1)
        return m, pos
    raise ValueError(f"unknown value tag {tag}")


def decode_value(buf) -> object:
    if len(buf) == 0:
        return None
    if buf[0] != 1:
        raise ValueError(f"unknown value encoding version {buf[0]}")
    v, pos = _dec(buf, 1, 0)
    if pos != len(buf):
        raise ValueError("trailing bytes after value")
    return v


# --- frames ----------------------------------------------------------------------
#
#   | length u32 (cobre type + payload) | type u8 | payload |

def _string(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<I", len(b)) + b


def _frame(mtype: int, payload: bytes) -> bytes:
    return struct.pack("<IB", len(payload) + 1, mtype) + payload


def _op_call(call_id: int, proc: str, args: bytes, acting_as: str | None = None,
             acting_attributes: dict | None = None, idempotency_key: str | None = None) -> bytes:
    payload = struct.pack("<I", call_id) + _string(proc) + struct.pack("<I", len(args)) + args
    # Extensão do minor 3 (ADR-029), só quando há o que mandar:
    #   flags u8 | [1: acting_as string | n u8 | (chave, valor)*] | [2: idempotency_key string]
    flags = (1 if acting_as else 0) | (2 if idempotency_key else 0)
    if flags:
        payload += bytes((flags,))
        if acting_as:
            attributes = list((acting_attributes or {}).items())
            payload += _string(acting_as) + bytes((len(attributes),))
            for key, value in attributes:
                payload += _string(key) + _string(value)
        if idempotency_key:
            payload += _string(idempotency_key)
    return _frame(T_OP_CALL, payload)


def _parse_op_result(body, pos: int):
    call_id, ok = struct.unpack_from("<IB", body, pos)
    pos += 5
    if ok:
        (n,) = struct.unpack_from("<I", body, pos)
        pos += 4
        return call_id, True, bytes(body[pos:pos + n])
    code, n = struct.unpack_from("<HI", body, pos)
    pos += 6
    message = bytes(body[pos:pos + n]).decode("utf-8")
    pos += n
    detail = None
    # Minor 3: o detail do erro, se veio.
    if len(body) - pos >= 4:
        (d,) = struct.unpack_from("<I", body, pos)
        detail = decode_value(bytes(body[pos + 4:pos + 4 + d]))
    return call_id, False, (code, message, detail)


class _Backoff:
    """Gira ~100 us, cede a CPU até 5 ms, depois dorme de 50 us até 1 ms."""

    def __init__(self):
        self.start = None
        self.sleep = 50e-6

    def wait(self):
        now = time.perf_counter()
        if self.start is None:
            self.start = now
        waited = now - self.start
        if waited < 100e-6:
            return
        if waited < 5e-3:
            time.sleep(0)
            return
        time.sleep(self.sleep)
        self.sleep = min(self.sleep * 2, 1e-3)


# --- anel de memória compartilhada (ADR-026) ----------------------------------------

_HEADER = 512
_REQ_TAIL, _REQ_HEAD, _RESP_TAIL, _RESP_HEAD = 64, 128, 192, 256
_CLIENT_STATE, _SERVER_STATE = 320, 384
_PADDING = 0xFFFFFFFF


class _Ring:
    """Posições u64 alinhadas lidas e gravadas com um acesso de 8 bytes
    (ctypes): em x86-64 isso já tem a ordem acquire/release que o anel pede."""

    def __init__(self, mm, data_off: int, size: int, tail_off: int, head_off: int):
        self.mm, self.data, self.size = mm, data_off, size
        self.tail = ctypes.c_uint64.from_buffer(mm, tail_off)
        self.head = ctypes.c_uint64.from_buffer(mm, head_off)
        self.pending = 0

    def try_write(self, frame: bytes) -> bool:
        need = (len(frame) + 7) & ~7
        if need > self.size:
            raise ModbError(0, f"message of {len(frame)} bytes does not fit the ring of {self.size}")
        head, tail = self.head.value, self.tail.value
        off = tail % self.size
        to_end = self.size - off
        total = to_end + need if need > to_end else need
        if tail + total - head > self.size:
            return False
        if need > to_end:
            struct.pack_into("<I", self.mm, self.data + off, _PADDING)
            tail += to_end
            off = 0
        self.mm[self.data + off:self.data + off + len(frame)] = frame
        self.tail.value = tail + need  # publica depois dos bytes
        return True

    def peek(self):
        head, tail = self.head.value, self.tail.value
        while head != tail:
            off = head % self.size
            (length,) = struct.unpack_from("<I", self.mm, self.data + off)
            if length == _PADDING:
                head += self.size - off
                self.head.value = head
                continue
            n = 4 + length
            if length == 0 or length > MAX_FRAME_BYTES or n > self.size - off or head + ((n + 7) & ~7) > tail:
                raise ModbError(0, "corrupt shared-memory ring")
            self.pending = (n + 7) & ~7
            return memoryview(self.mm)[self.data + off:self.data + off + n]
        return None

    def pop(self):
        self.head.value = self.head.value + self.pending
        self.pending = 0

    def release(self):
        del self.tail, self.head


class _Shm:
    def __init__(self, kind: int, name: str, ring_bytes: int):
        size = _HEADER + 2 * ring_bytes
        if kind == 1:  # mapeamento nomeado do Windows
            self.fd = None
            self.mm = mmap.mmap(-1, size, tagname=name)
        else:  # arquivo (em /dev/shm no Linux)
            self.fd = os.open(name, os.O_RDWR)
            self.mm = mmap.mmap(self.fd, size)
        if self.mm[0:8] != b"MDBSHM01" or struct.unpack_from("<I", self.mm, 8)[0] != ring_bytes:
            raise ModbError(0, "shared-memory region has a bad header")
        self.requests = _Ring(self.mm, _HEADER, ring_bytes, _REQ_TAIL, _REQ_HEAD)
        self.responses = _Ring(self.mm, _HEADER + ring_bytes, ring_bytes, _RESP_TAIL, _RESP_HEAD)
        self.client_state = ctypes.c_uint32.from_buffer(self.mm, _CLIENT_STATE)
        self.server_state = ctypes.c_uint32.from_buffer(self.mm, _SERVER_STATE)
        self.client_state.value = 1  # anexado: o servidor já pode apagar o nome

    def close(self):
        self.client_state.value = 2  # saindo
        self.requests.release()
        self.responses.release()
        del self.client_state, self.server_state
        self.mm.close()
        if self.fd is not None:
            os.close(self.fd)


# --- cliente -----------------------------------------------------------------------

class Client:
    def __init__(self, host: str = "127.0.0.1", port: int = 7474, database: str = "", timeout: float = 30.0,
                 token: str | None = None):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.next_id = 1
        self.shm = None
        hello = struct.pack("<H", PROTOCOL_MAJOR) + _string(database) + bytes((1, 0)) + struct.pack("<H", PROTOCOL_MINOR)
        self.sock.sendall(_frame(T_HELLO, hello))
        mtype, body = self._recv_frame()
        if mtype != T_HELLO_OK:
            raise ModbError(0, f"expected HelloOk, got message type {mtype}")
        (self.server_major, _baseline, _codec, self.max_frame, _streams, _ratio,
         self.idle_timeout_ms) = struct.unpack_from("<HQBIHHI", body, 0)
        self.server_minor = struct.unpack_from("<H", body, 23)[0] if len(body) >= 25 else 0
        # Minor 2: mecanismos de autenticação que o proxy exige (ADR-028).
        self.auth_mechanisms = []
        if self.server_minor >= 2 and len(body) > 25:
            count, off = body[25], 26
            for _ in range(count):
                (n,) = struct.unpack_from("<I", body, off)
                self.auth_mechanisms.append(bytes(body[off + 4:off + 4 + n]).decode("utf-8"))
                off += 4 + n
        self.principal = ""
        if token is not None:
            self.authenticate(token)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        if self.shm is not None:
            self.shm.close()
            self.shm = None
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def _recv_exact(self, n: int) -> bytes:
        chunks, got = [], 0
        while got < n:
            c = self.sock.recv(n - got)
            if not c:
                raise ModbError(0, "server closed the connection")
            chunks.append(c)
            got += len(c)
        return b"".join(chunks)

    def _recv_frame(self):
        (length,) = struct.unpack("<I", self._recv_exact(4))
        if length == 0 or length > MAX_FRAME_BYTES:
            raise ModbError(0, f"bad frame length {length}")
        data = self._recv_exact(length)
        return data[0], memoryview(data)[1:]

    def authenticate(self, token: str, mechanism: str = "token") -> str:
        """Identifica o cliente a um modb-proxy que exige autenticação; devolve o principal."""
        if self.server_minor < 2:
            raise ModbError(0, "server does not support authentication (protocol minor < 2)")
        rid = self._take_id()
        payload = token.encode("utf-8")
        self.sock.sendall(_frame(T_AUTHENTICATE, struct.pack("<I", rid) + _string(mechanism) +
                                 struct.pack("<I", len(payload)) + payload))
        mtype, body = self._recv_frame()
        if mtype != T_AUTHENTICATE_OK:
            raise ModbError(0, f"expected AuthenticateOk, got message type {mtype}")
        _rid, ok, code, n = struct.unpack_from("<IBHI", body, 0)
        message = bytes(body[11:11 + n]).decode("utf-8")
        if not ok:
            raise ModbError(code, message)
        (m,) = struct.unpack_from("<I", body, 11 + n)
        self.principal = bytes(body[15 + n:15 + n + m]).decode("utf-8")
        return self.principal

    def attach_shared_memory(self, ring_bytes: int = 0) -> None:
        """Passa as chamadas ao anel de memória compartilhada (mesma máquina)."""
        if self.shm is not None:
            return
        if self.server_minor < 1:
            raise ModbError(0, "server does not support shared-memory rings")
        rid = self._take_id()
        self.sock.sendall(_frame(T_SHM_ATTACH, struct.pack("<II", rid, ring_bytes)))
        mtype, body = self._recv_frame()
        if mtype != T_SHM_ATTACH_OK:
            raise ModbError(0, f"expected ShmAttachOk, got message type {mtype}")
        got_id, ok = struct.unpack_from("<IB", body, 0)
        if not ok:
            code, n = struct.unpack_from("<HI", body, 5)
            raise ModbError(code, bytes(body[11:11 + n]).decode("utf-8"))
        kind, n = struct.unpack_from("<BI", body, 5)
        name = bytes(body[10:10 + n]).decode("utf-8")
        (size,) = struct.unpack_from("<I", body, 10 + n)
        self.shm = _Shm(kind, name, size)

    def _take_id(self) -> int:
        i = self.next_id
        self.next_id = (self.next_id + 1) & 0xFFFFFFFF or 1
        return i

    def call(self, proc: str, args: dict | None = None, *, acting_as: str | None = None,
             acting_attributes: dict | None = None, idempotency_key: str | None = None):
        """Chama uma proc. `acting_as` (em nome de quem; só atrás de um proxy e com a
        role delegate) e `idempotency_key` (a mesma chave devolve o resultado já
        confirmado, sem executar de novo) pedem minor 3."""
        if (acting_as or idempotency_key) and self.server_minor < 3:
            raise ModbError(1, "server does not support protocol minor 3 (delegation, idempotency keys)")
        call_id = self._take_id()
        frame = _op_call(call_id, proc, encode_value({} if args is None else args), acting_as, acting_attributes,
                         idempotency_key)
        if self.shm is not None:
            body = self._call_shm(frame)
        else:
            self.sock.sendall(frame)
            mtype, body = self._recv_frame()
            if mtype != T_OP_RESULT:
                raise ModbError(0, f"expected OpResult, got message type {mtype}")
        got_id, ok, rest = _parse_op_result(body, 0)
        if got_id != call_id:
            raise ModbError(0, "OpResult call_id mismatch")
        if not ok:
            raise ModbError(*rest)
        return decode_value(rest)

    def _server_gone(self) -> bool:
        """Linha de vida: o TCP fica aberto enquanto o anel existe; EOF ou erro
        nele = o servidor sumiu (morreu sem conseguir marcar o anel)."""
        if self.shm.server_state.value == 2:
            return True
        try:
            readable, _, _ = select.select([self.sock], [], [], 0)
            return bool(readable) and self.sock.recv(1, socket.MSG_PEEK) == b""
        except OSError:
            return True

    def _call_shm(self, frame: bytes):
        shm = self.shm
        b = _Backoff()
        while not shm.requests.try_write(frame):
            if self._server_gone():
                raise ModbError(0, "server closed the shared-memory ring")
            b.wait()
        b = _Backoff()
        checks = 0
        while True:
            view = shm.responses.peek()
            if view is not None:
                mtype = view[4]
                body = bytes(view[5:])
                view.release()
                shm.responses.pop()
                if mtype != T_OP_RESULT:
                    raise ModbError(0, f"unexpected message type {mtype} on the ring")
                return body
            checks += 1
            # A checagem do socket custa uma syscall: só de vez em quando.
            if (checks & 1023) == 0 and self._server_gone():
                raise ModbError(0, "server closed the shared-memory ring")
            b.wait()


if __name__ == "__main__":
    # modb_client.py HOST PORT PROC [JSON]
    import json
    host, port, proc = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    args = json.loads(sys.argv[4]) if len(sys.argv) > 4 else {}
    with Client(host, port) as c:
        print(json.dumps(c.call(proc, args), ensure_ascii=False))
