#!/usr/bin/env python3
"""Gera um site HTML estático e navegável a partir de toda a documentação Markdown.

Os `.md` versionados continuam sendo a fonte; o HTML é descartável e fica fora do
git (por padrão em `build/docs-site/`). Abre direto do disco (`file://`), sem
servidor e sem rede.

Por que um script próprio e não MkDocs/pandoc: o pacote `markdown` já está
instalado nesta máquina e basta; MkDocs exigiria instalar dependências, pandoc
não está disponível, e nenhum dos dois reescreve `arquivo.cpp:353` para uma
página com âncora de linha, que é como os planos citam o código.

Uso:
    python scripts/build_docs_site.py [--out DIR] [--no-strict]

Com links internos quebrados o build falha (código 1) e lista cada um, a menos
que se passe `--no-strict`.
"""

from __future__ import annotations

import argparse
import html
import json
import os
import re
import subprocess
import sys
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path

import markdown

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "build" / "docs-site"
SITE_TITLE = "moDb — documentação"

# Ordem das seções na navegação: (prefixo do caminho, título). O primeiro prefixo
# que casar vence; "" casa arquivos da raiz.
SECTIONS = [
    ("docs/reference/", "Referência"),
    ("docs/decisions/", "Decisões (ADRs)"),
    ("docs/training/", "Treinamento"),
    ("docs/", "Documentação"),
    ("docs-process/training/", "Treinamento Ring0 (servidor)"),
    ("docs-process/", "Processo e planos"),
    ("examples/", "Exemplos"),
    ("", "Visão geral"),
]
SECTION_ORDER = [
    "Visão geral", "Documentação", "Referência", "Decisões (ADRs)", "Treinamento",
    "Treinamento Ring0 (servidor)", "Processo e planos", "Exemplos",
]
EXCLUDED_PREFIXES = ("tests/fixtures/",)

# Páginas HTML já existentes, geradas por outros scripts, que valem um atalho na
# página inicial.
EXTRA_LINKS = [
    ("docs-process/training/html/index.html", "Treinamento Ring0 narrado (HTML com áudio)"),
    ("loadtests/dashboard/index.html", "Dashboard dos testes de carga"),
]

# Arquivos de código maiores que isto são linkados crus, sem página de visualização.
SOURCE_VIEW_MAX_BYTES = 1_000_000

LINE_SUFFIX = re.compile(r"^(?P<path>.+?):(?P<line>\d+)(?:-\d+)?$")
HREF_ATTR = re.compile(r'(<(?:a|img)\b[^>]*?\s(?:href|src)=")([^"]*)(")')
TAG = re.compile(r"<[^>]+>")
FENCE = re.compile(r"^\s*(```|~~~)")
LIST_ITEM = re.compile(r"^(\s*)([-*+]|\d+[.)])\s+")
TASK_ITEM = re.compile(r"<li>(<p>)?\[([ xX])\]\s*")


# ---------------------------------------------------------------------------
# Modelo


@dataclass
class Doc:
    rel: str                      # caminho relativo à raiz do repo, com "/"
    section: str
    title: str = ""
    body: str = ""                # HTML do conteúdo, antes da reescrita de links
    toc_tokens: list = field(default_factory=list)
    ids: set = field(default_factory=set)
    text: str = ""                # texto puro, para a busca

    @property
    def out_rel(self) -> str:
        return self.rel[:-3] + ".html"


# ---------------------------------------------------------------------------
# Coleta


def list_markdown() -> list[str]:
    try:
        # Versionados e novos ainda não commitados; ignorados pelo .gitignore ficam de fora.
        out = subprocess.run(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "*.md"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        ).stdout
        files = [line.strip() for line in out.splitlines() if line.strip()]
    except (OSError, subprocess.CalledProcessError):
        skip = {"build", ".git", "load-results", "benchmark-results"}
        files = [
            p.relative_to(ROOT).as_posix()
            for p in ROOT.rglob("*.md")
            if not (set(p.relative_to(ROOT).parts) & skip)
            and not p.relative_to(ROOT).parts[0].startswith("cmake-build")
        ]
    return sorted(f for f in files if not f.startswith(EXCLUDED_PREFIXES))


def section_of(rel: str) -> str:
    for prefix, title in SECTIONS:
        if prefix == "" and "/" not in rel:
            return title
        if prefix and rel.startswith(prefix):
            return title
    return "Visão geral" if "/" not in rel else "Outros"


def natural_key(rel: str):
    parts = []
    for part in rel.lower().split("/"):
        # README primeiro dentro de cada diretório
        weight = 0 if part == "readme.md" else 1
        tokens = [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", part)]
        parts.append((weight, tokens))
    return parts


# ---------------------------------------------------------------------------
# Markdown


def github_slug(value: str, separator: str = "-") -> str:
    """Âncoras no estilo do GitHub, que é como os documentos já as escrevem."""
    # O toc já entrega texto puro; não remover "<...>", que aqui é C++ (`Handle<T>`).
    value = html.unescape(value).strip().lower()
    value = "".join(ch for ch in value if ch.isalnum() or ch in " -_" or unicodedata.category(ch) == "Mn")
    return value.replace(" ", separator)


def normalize_github_markdown(source: str) -> str:
    """Aproxima o Markdown do GitHub das regras do Python-Markdown.

    Python-Markdown exige linha em branco antes de lista e de tabela, e 4 espaços
    para aninhar listas; o GitHub aceita 2 e dispensa a linha em branco. Os
    documentos foram escritos para o GitHub.
    """
    out: list[str] = []
    in_fence = False
    in_list = False
    prev = ""
    for line in source.splitlines():
        if FENCE.match(line):
            in_fence = not in_fence
            out.append(line)
            prev = line
            continue
        if in_fence:
            out.append(line)
            prev = line
            continue

        stripped = line.strip()
        indent = len(line) - len(line.lstrip(" "))
        is_item = bool(LIST_ITEM.match(line))
        is_table = stripped.startswith("|")

        if is_item and indent == 0 and prev.strip() and not LIST_ITEM.match(prev) and not in_list:
            out.append("")
        if is_table and prev.strip() and not prev.strip().startswith("|"):
            out.append("")

        if is_item:
            in_list = True
        elif stripped and indent == 0:
            in_list = False

        if in_list and indent > 0 and stripped:
            line = " " * (indent * 2) + line.lstrip(" ")
        out.append(line)
        prev = line
    return "\n".join(out) + "\n"


def make_markdown() -> markdown.Markdown:
    return markdown.Markdown(
        extensions=["tables", "fenced_code", "toc", "sane_lists", "md_in_html"],
        extension_configs={"toc": {"slugify": github_slug, "toc_depth": "2-4"}},
    )


def render(doc: Doc, md: markdown.Markdown) -> None:
    raw = (ROOT / doc.rel).read_bytes()
    try:
        source = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        source = raw.decode("cp1252", errors="replace")
        print(f"aviso: {doc.rel} não é UTF-8; lido como cp1252", file=sys.stderr)

    md.reset()
    body = md.convert(normalize_github_markdown(source))
    body = TASK_ITEM.sub(
        lambda m: '<li class="task">' + (m.group(1) or "")
        + ('<input type="checkbox" disabled checked> ' if m.group(2) in "xX"
           else '<input type="checkbox" disabled> '),
        body,
    )
    body = re.sub(r"~~(.+?)~~", r"<del>\1</del>", body)

    doc.body = body
    doc.toc_tokens = md.toc_tokens
    doc.ids = set(re.findall(r'\sid="([^"]+)"', body))
    match = re.search(r"<h1[^>]*>(.*?)</h1>", body, re.S)
    doc.title = html.unescape(TAG.sub("", match.group(1))).strip() if match else Path(doc.rel).stem
    doc.text = re.sub(r"\s+", " ", html.unescape(TAG.sub(" ", body))).strip()


# ---------------------------------------------------------------------------
# Links


@dataclass
class LinkContext:
    docs: dict[str, Doc]
    out_dir: Path
    broken: list[tuple[str, str, str]] = field(default_factory=list)
    sources: set[str] = field(default_factory=set)
    assets: set[str] = field(default_factory=set)
    dir_indexes: set[str] = field(default_factory=set)


def rel_url(from_out_rel: str, to_out_rel: str) -> str:
    base = os.path.dirname(from_out_rel) or "."
    return os.path.relpath(to_out_rel, base).replace(os.sep, "/")


def resolve_link(doc: Doc, href: str, ctx: LinkContext, is_img: bool) -> str:
    if not href or href.startswith(("http://", "https://", "mailto:", "data:", "javascript:")):
        return href
    from urllib.parse import unquote

    path_part, _, fragment = href.partition("#")
    fragment = unquote(fragment)
    if not path_part:
        if fragment and fragment not in doc.ids:
            ctx.broken.append((doc.rel, href, "âncora inexistente"))
        return href

    path_part = unquote(path_part)
    line = None
    if not (ROOT / os.path.dirname(doc.rel) / path_part).exists():
        m = LINE_SUFFIX.match(path_part)
        if m:
            path_part, line = m.group("path"), m.group("line")

    target = os.path.normpath(os.path.join(os.path.dirname(doc.rel), path_part)).replace(os.sep, "/")
    target_abs = ROOT / target
    if target.startswith("..") or not target_abs.exists():
        ctx.broken.append((doc.rel, href, "arquivo inexistente"))
        return href

    if target_abs.is_dir():
        directory = target.rstrip("/")
        readme = f"{directory}/README.md"
        if readme in ctx.docs:
            target = readme
        elif any(rel.startswith(directory + "/") for rel in ctx.docs):
            # Sem README: aponta para um índice gerado do diretório.
            ctx.dir_indexes.add(directory)
            return rel_url(doc.out_rel, f"{directory}/index.html")
        else:
            ctx.broken.append((doc.rel, href, "diretório sem documentos"))
            return href

    if target.endswith(".md"):
        if target not in ctx.docs:
            ctx.broken.append((doc.rel, href, "documento fora do site"))
            return href
        dest = ctx.docs[target]
        if fragment and fragment not in dest.ids:
            ctx.broken.append((doc.rel, href, "âncora inexistente"))
        return rel_url(doc.out_rel, dest.out_rel) + (f"#{fragment}" if fragment else "")

    if is_img or target_abs.suffix.lower() in {".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp"}:
        ctx.assets.add(target)
        return rel_url(doc.out_rel, target)

    if target_abs.suffix.lower() in {".html", ".htm", ".mp3", ".pdf"} \
            or target_abs.stat().st_size > SOURCE_VIEW_MAX_BYTES:
        # Link para o arquivo real no repositório.
        real = os.path.relpath(target_abs, (ctx.out_dir / doc.out_rel).parent)
        return real.replace(os.sep, "/")

    ctx.sources.add(target)
    anchor = f"#L{line}" if line else (f"#{fragment}" if fragment else "")
    return rel_url(doc.out_rel, f"_src/{target}.html") + anchor


def rewrite_links(doc: Doc, ctx: LinkContext) -> str:
    def repl(m: re.Match[str]) -> str:
        is_img = m.group(1).lstrip().startswith("<img")
        return m.group(1) + html.escape(resolve_link(doc, html.unescape(m.group(2)), ctx, is_img)) + m.group(3)

    return HREF_ATTR.sub(repl, doc.body)


# ---------------------------------------------------------------------------
# Páginas


def page_shell(*, title: str, out_rel: str, nav: str, main: str, toc: str = "") -> str:
    root = rel_url(out_rel, "index.html").rsplit("index.html", 1)[0]
    return f"""<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)} · moDb</title>
<link rel="stylesheet" href="{root}assets/site.css">
<script>try{{var t=localStorage.getItem('modb-docs-theme');if(t)document.documentElement.dataset.theme=t;}}catch(e){{}}</script>
</head>
<body data-root="{root}">
<header class="topbar">
  <button class="menu-btn" type="button" aria-label="Abrir menu" aria-expanded="false">☰</button>
  <a class="brand" href="{root}index.html">{html.escape(SITE_TITLE)}</a>
  <form class="search" action="{root}search.html" method="get" role="search">
    <input type="search" name="q" placeholder="Buscar na documentação…" autocomplete="off" aria-label="Buscar">
    <div class="search-results" hidden></div>
  </form>
  <button class="theme-btn" type="button" aria-label="Alternar tema claro/escuro">◐</button>
</header>
<div class="layout">
  <nav class="sidebar" aria-label="Documentos">{nav}</nav>
  <main class="content">{main}</main>
  {f'<aside class="toc" aria-label="Nesta página"><div class="toc-title">Nesta página</div>{toc}</aside>' if toc else ''}
</div>
<script src="{root}search-index.js"></script>
<script src="{root}assets/site.js"></script>
</body>
</html>
"""


def build_nav(ordered: dict[str, list[Doc]], current_out: str) -> str:
    parts = []
    for section, docs in ordered.items():
        is_open = any(d.out_rel == current_out for d in docs)
        items = []
        for d in docs:
            cls = ' class="current" aria-current="page"' if d.out_rel == current_out else ""
            items.append(
                f'<li><a{cls} href="{html.escape(rel_url(current_out, d.out_rel))}" '
                f'title="{html.escape(d.rel)}">{html.escape(d.title)}</a></li>'
            )
        parts.append(
            f'<details{" open" if is_open else ""}><summary>{html.escape(section)} '
            f'<span class="count">{len(docs)}</span></summary><ul>{"".join(items)}</ul></details>'
        )
    return "".join(parts)


def toc_html(tokens: list) -> str:
    # O h1 é o título da página; o sumário começa nos filhos dele.
    flat = []
    for tok in tokens:
        flat.extend(tok["children"] if tok["level"] == 1 else [tok])
    if not flat:
        return ""
    items = "".join(
        f'<li><a href="#{html.escape(tok["id"])}">{tok["name"]}</a>{toc_html(tok["children"])}</li>'
        for tok in flat
    )
    return f"<ul>{items}</ul>"


def breadcrumb(doc: Doc) -> str:
    crumbs = [f'<a href="{rel_url(doc.out_rel, "index.html")}">Início</a>', html.escape(doc.section)]
    return '<div class="breadcrumb">' + " › ".join(crumbs) + "</div>"


def prev_next(doc: Doc, siblings: list[Doc]) -> str:
    i = siblings.index(doc)
    links = []
    if i > 0:
        p = siblings[i - 1]
        links.append(f'<a class="prev" href="{rel_url(doc.out_rel, p.out_rel)}">← {html.escape(p.title)}</a>')
    else:
        links.append("<span></span>")
    if i + 1 < len(siblings):
        n = siblings[i + 1]
        links.append(f'<a class="next" href="{rel_url(doc.out_rel, n.out_rel)}">{html.escape(n.title)} →</a>')
    return '<nav class="pager">' + "".join(links) + "</nav>"


def write_doc_page(doc: Doc, body: str, ordered: dict[str, list[Doc]], out_dir: Path) -> None:
    siblings = ordered[doc.section]
    toc = toc_html(doc.toc_tokens)
    heading_count = body.count("<h2") + body.count("<h3")
    main = (
        breadcrumb(doc)
        + f'<article class="doc">{body}</article>'
        + prev_next(doc, siblings)
        + f'<footer class="doc-source">Fonte: <code>{html.escape(doc.rel)}</code></footer>'
    )
    page = page_shell(
        title=doc.title, out_rel=doc.out_rel, nav=build_nav(ordered, doc.out_rel), main=main,
        toc=toc if heading_count >= 3 else "",
    )
    dest = out_dir / doc.out_rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(page, encoding="utf-8")


def write_index(ordered: dict[str, list[Doc]], out_dir: Path) -> None:
    cards = []
    for section, docs in ordered.items():
        items = "".join(
            f'<li><a href="{html.escape(d.out_rel)}">{html.escape(d.title)}</a></li>' for d in docs[:8]
        )
        more = f'<li class="more">… e mais {len(docs) - 8}</li>' if len(docs) > 8 else ""
        cards.append(
            f'<section class="card"><h2>{html.escape(section)} <span class="count">{len(docs)}</span></h2>'
            f"<ul>{items}{more}</ul></section>"
        )
    extras = []
    for rel, label in EXTRA_LINKS:
        if (ROOT / rel).exists():
            href = os.path.relpath(ROOT / rel, out_dir).replace(os.sep, "/")
            extras.append(f'<li><a href="{html.escape(href)}">{html.escape(label)}</a></li>')
    total = sum(len(d) for d in ordered.values())
    main = (
        f"<h1>{html.escape(SITE_TITLE)}</h1>"
        f'<p class="lead">{total} documentos em {len(ordered)} seções, gerados a partir dos '
        f"arquivos <code>.md</code> do repositório. Use a busca no topo ou o menu lateral.</p>"
        + (f'<h2>Outras páginas</h2><ul class="extras">{"".join(extras)}</ul>' if extras else "")
        + f'<div class="cards">{"".join(cards)}</div>'
    )
    (out_dir / "index.html").write_text(
        page_shell(title="Início", out_rel="index.html", nav=build_nav(ordered, "index.html"), main=main),
        encoding="utf-8",
    )


def write_search_page(ordered: dict[str, list[Doc]], out_dir: Path) -> None:
    main = '<h1>Busca</h1><p class="search-summary"></p><ol class="search-page-results"></ol>'
    (out_dir / "search.html").write_text(
        page_shell(title="Busca", out_rel="search.html", nav=build_nav(ordered, "search.html"), main=main),
        encoding="utf-8",
    )


def write_search_index(docs: list[Doc], out_dir: Path) -> None:
    entries = [
        {"t": d.title, "u": d.out_rel, "s": d.section, "p": d.rel, "x": d.text[:60000]} for d in docs
    ]
    (out_dir / "search-index.js").write_text(
        "window.MODB_SEARCH_INDEX=" + json.dumps(entries, ensure_ascii=False) + ";\n", encoding="utf-8"
    )


def write_source_page(rel: str, ordered: dict[str, list[Doc]], out_dir: Path) -> None:
    raw = (ROOT / rel).read_bytes()
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = raw.decode("cp1252", errors="replace")
    rows = "".join(
        f'<tr id="L{i}"><td class="ln"><a href="#L{i}">{i}</a></td><td class="code">{html.escape(line) or " "}</td></tr>'
        for i, line in enumerate(text.splitlines(), 1)
    )
    out_rel = f"_src/{rel}.html"
    real = os.path.relpath(ROOT / rel, (out_dir / out_rel).parent).replace(os.sep, "/")
    main = (
        f'<div class="breadcrumb"><a href="{rel_url(out_rel, "index.html")}">Início</a> › código-fonte</div>'
        f'<h1 class="src-title"><code>{html.escape(rel)}</code></h1>'
        f'<p class="src-meta"><a href="{html.escape(real)}">abrir o arquivo original</a> · '
        f"cópia do momento do build; a numeração de linhas vale para esta versão</p>"
        f'<div class="src-wrap"><table class="source"><tbody>{rows}</tbody></table></div>'
    )
    dest = out_dir / out_rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(
        page_shell(title=Path(rel).name, out_rel=out_rel, nav=build_nav(ordered, out_rel), main=main),
        encoding="utf-8",
    )


def write_dir_index(directory: str, docs: dict[str, Doc], ordered: dict[str, list[Doc]], out_dir: Path) -> None:
    out_rel = f"{directory}/index.html"
    members = sorted((d for d in docs.values() if d.rel.startswith(directory + "/")), key=lambda d: natural_key(d.rel))
    items = "".join(
        f'<li><a href="{html.escape(rel_url(out_rel, d.out_rel))}">{html.escape(d.title)}</a> '
        f'<span class="r-meta">{html.escape(d.rel[len(directory) + 1:])}</span></li>'
        for d in members
    )
    main = (
        f'<div class="breadcrumb"><a href="{rel_url(out_rel, "index.html")}">Início</a> › índice</div>'
        f'<article class="doc"><h1><code>{html.escape(directory)}/</code></h1>'
        f"<p>Índice gerado: o diretório não tem <code>README.md</code>.</p><ul>{items}</ul></article>"
    )
    dest = out_dir / out_rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(
        page_shell(title=f"{directory}/", out_rel=out_rel, nav=build_nav(ordered, out_rel), main=main),
        encoding="utf-8",
    )


def copy_assets(out_dir: Path, assets: set[str]) -> None:
    here = Path(__file__).resolve().parent / "docs_site_assets"
    (out_dir / "assets").mkdir(parents=True, exist_ok=True)
    for name in ("site.css", "site.js"):
        (out_dir / "assets" / name).write_bytes((here / name).read_bytes())
    for rel in assets:
        dest = out_dir / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes((ROOT / rel).read_bytes())


# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="diretório de saída")
    parser.add_argument("--no-strict", action="store_true", help="não falhar com links quebrados")
    args = parser.parse_args()
    out_dir = args.out.resolve()
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8")

    md = make_markdown()
    docs = {rel: Doc(rel=rel, section=section_of(rel)) for rel in list_markdown()}
    for doc in docs.values():
        render(doc, md)

    ordered: dict[str, list[Doc]] = {}
    for section in SECTION_ORDER + sorted({d.section for d in docs.values()} - set(SECTION_ORDER)):
        members = sorted((d for d in docs.values() if d.section == section), key=lambda d: natural_key(d.rel))
        if members:
            ordered[section] = members

    ctx = LinkContext(docs=docs, out_dir=out_dir)
    bodies = {rel: rewrite_links(doc, ctx) for rel, doc in docs.items()}

    out_dir.mkdir(parents=True, exist_ok=True)
    copy_assets(out_dir, ctx.assets)
    for rel, doc in docs.items():
        write_doc_page(doc, bodies[rel], ordered, out_dir)
    for rel in sorted(ctx.sources):
        write_source_page(rel, ordered, out_dir)
    for directory in sorted(ctx.dir_indexes):
        write_dir_index(directory, docs, ordered, out_dir)
    write_index(ordered, out_dir)
    write_search_page(ordered, out_dir)
    write_search_index(list(docs.values()), out_dir)

    print(f"{len(docs)} documentos, {len(ctx.sources)} arquivos de código, {len(ordered)} seções")
    print(f"saída: {out_dir / 'index.html'}")
    if ctx.broken:
        print(f"\n{len(ctx.broken)} link(s) interno(s) quebrado(s):", file=sys.stderr)
        for src, href, why in sorted(ctx.broken):
            print(f"  {src}: {href}  ({why})", file=sys.stderr)
        if not args.no_strict:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
