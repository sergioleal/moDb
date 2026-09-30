"""Confere os ErrorCode do header contra a documentação e os clientes Python e Node.

Uso: error_codes_test.py <raiz do repositório>

O C++ (modb.error_code_values) fixa os números; este teste pega o que o
compilador não vê: um código novo sem entrada no Apêndice A do
PROTOCOLO_CLIENTES.md, uma tabela da documentação com número ou nome errado, e
as constantes dos clientes Python e Node fora do header.
"""
import re
import sys
from pathlib import Path


def header_codes(root: Path) -> dict[str, int]:
    text = (root / "include/modb/error.hpp").read_text(encoding="utf-8")
    start = text.index("enum class ErrorCode {")
    body = text[start:text.index("\n};", start)]
    codes, missing = {}, []
    for line in body.splitlines()[1:]:
        line = line.split("//")[0].strip()
        if not line:
            continue
        m = re.fullmatch(r"([a-z_0-9]+)\s*=\s*(\d+),", line)
        if not m:
            missing.append(line)
            continue
        codes[m.group(1)] = int(m.group(2))
    if missing:
        sys.exit(f"ErrorCode sem valor explícito: {missing}")
    return codes


def doc_tables(root: Path) -> tuple[dict[str, int], dict[str, int]]:
    text = (root / "docs/PROTOCOLO_CLIENTES.md").read_text(encoding="utf-8")
    appendix_at = text.index("## Apêndice A")
    row = re.compile(r"^\| (\d+) \| `([a-z_0-9]+)` \|", re.M)
    section3 = text[text.index("## 3. Chamada de proc"):text.index("## 4. Value")]
    main = {name: int(code) for code, name in row.findall(section3)}
    appendix = {name: int(code) for code, name in row.findall(text[appendix_at:])}
    return main, appendix


def node_codes(root: Path) -> dict[str, int]:
    text = (root / "clients/node/src/errors.ts").read_text(encoding="utf-8")
    body = text[text.index("export const ErrorCode = {"):]
    body = body[:body.index("} as const")]
    return {name: int(code) for name, code in re.findall(r"^\s+([a-z_0-9]+): (\d+),", body, re.M)}


def main() -> int:
    root = Path(sys.argv[1])
    sys.path.insert(0, str(root / "clients/python"))
    import modb_client

    header = header_codes(root)
    failures = []

    values = sorted(header.values())
    if values != list(range(len(values))):
        failures.append(f"números do header não são contínuos a partir de 0: {values}")

    main_table, appendix = doc_tables(root)
    if appendix != header:
        only_header = sorted(set(header.items()) - set(appendix.items()))
        only_doc = sorted(set(appendix.items()) - set(header.items()))
        failures.append(f"Apêndice A diverge do header; só no header: {only_header}; só no doc: {only_doc}")
    for name, code in main_table.items():
        if header.get(name) != code:
            failures.append(f"§3: {name} = {code}, header = {header.get(name)}")
    node = node_codes(root)
    for name, code in node.items():
        if header.get(name) != code:
            failures.append(f"clients/node ErrorCode: {name} = {code}, header = {header.get(name)}")
    if set(node) != set(main_table):
        failures.append(f"o ErrorCode do cliente Node e a tabela do §3 listam códigos diferentes: "
                        f"{sorted(set(node) ^ set(main_table))}")
    for name, code in modb_client.ERROR_CODES.items():
        if header.get(name) != code:
            failures.append(f"modb_client.ERROR_CODES: {name} = {code}, header = {header.get(name)}")
    if set(modb_client.ERROR_CODES) != set(main_table):
        failures.append(f"ERROR_CODES e a tabela do §3 listam códigos diferentes: "
                        f"{sorted(set(modb_client.ERROR_CODES) ^ set(main_table))}")

    for failure in failures:
        print("FAIL:", failure)
    if not failures:
        print(f"{len(header)} códigos: header, documentação e clientes Python e Node conferem")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
