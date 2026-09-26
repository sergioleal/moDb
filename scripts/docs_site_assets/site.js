// Comportamento do site de documentação gerado por scripts/build_docs_site.py.
// Funciona em file:// — o índice de busca é carregado como <script>, não por fetch.
(function () {
  "use strict";

  var root = document.body.getAttribute("data-root") || "";
  var index = window.MODB_SEARCH_INDEX || [];

  // ---- tema ----
  var themeBtn = document.querySelector(".theme-btn");
  if (themeBtn) {
    themeBtn.addEventListener("click", function () {
      var el = document.documentElement;
      var current = el.dataset.theme ||
        (window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light");
      var next = current === "dark" ? "light" : "dark";
      el.dataset.theme = next;
      try { localStorage.setItem("modb-docs-theme", next); } catch (e) { /* sem storage */ }
    });
  }

  // ---- menu em telas estreitas ----
  var menuBtn = document.querySelector(".menu-btn");
  if (menuBtn) {
    menuBtn.addEventListener("click", function () {
      var open = document.body.classList.toggle("menu-open");
      menuBtn.setAttribute("aria-expanded", open ? "true" : "false");
    });
    document.addEventListener("click", function (ev) {
      if (!document.body.classList.contains("menu-open")) return;
      if (ev.target.closest(".sidebar") || ev.target.closest(".menu-btn")) return;
      document.body.classList.remove("menu-open");
      menuBtn.setAttribute("aria-expanded", "false");
    });
  }

  // Item atual visível no menu lateral.
  // Rola só o menu: scrollIntoView rolaria também a página quando o menu está
  // fora da tela (telas estreitas), deslocando o conteúdo.
  var sidebar = document.querySelector(".sidebar");
  var current = sidebar && sidebar.querySelector("a.current");
  if (current) {
    var top = current.getBoundingClientRect().top - sidebar.getBoundingClientRect().top;
    sidebar.scrollTop += top - sidebar.clientHeight / 2;
  }

  // ---- âncoras de título ----
  document.querySelectorAll(".doc h2[id], .doc h3[id], .doc h4[id]").forEach(function (h) {
    var a = document.createElement("a");
    a.className = "anchor";
    a.href = "#" + h.id;
    a.textContent = "#";
    a.setAttribute("aria-label", "Link para esta seção");
    h.insertBefore(a, h.firstChild);
  });

  // ---- destaque de código (leve, sem dependência) ----
  var KW = {
    cpp: "alignas auto bool break case catch char class const constexpr consteval continue co_await co_return decltype default delete do double else enum explicit export extern false float for friend if inline int long mutable namespace new noexcept nullptr operator private protected public return short signed sizeof static static_assert struct switch template this throw true try typedef typename union unsigned using virtual void volatile while include define pragma ifdef ifndef endif std uint8_t uint16_t uint32_t uint64_t int64_t size_t",
    sh: "if then else elif fi for in do done while case esac function return export local echo cd set",
    ps: "if else elseif foreach for while function param return try catch finally throw switch true false null",
    py: "def class return if elif else for in while import from as with try except finally raise None True False and or not lambda yield pass",
    json: "true false null",
  };
  var ALIAS = { "c++": "cpp", c: "cpp", cc: "cpp", h: "cpp", hpp: "cpp", bash: "sh", shell: "sh", powershell: "ps", ps1: "ps", pwsh: "ps", python: "py", yaml: "sh", yml: "sh", cmake: "sh", jsonl: "json" };

  function esc(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }

  function highlight(code, lang) {
    var kw = KW[lang];
    if (!kw) return null;
    var words = new RegExp("^(?:" + kw.split(" ").join("|") + ")$");
    var lineCom = lang === "cpp" || lang === "json" ? "//" : "#";
    var re = lang === "cpp"
      ? /(\/\*[\s\S]*?\*\/|\/\/[^\n]*)|("(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')|(\b\d[\d'.xXa-fA-FuUlL]*\b)|([A-Za-z_]\w*)/g
      : new RegExp("(" + (lineCom === "#" ? "#[^\\n]*" : "\\/\\/[^\\n]*") + ")|(\"(?:\\\\.|[^\"\\\\\\n])*\"|'[^'\\n]*')|(\\b\\d[\\d.]*\\b)|([A-Za-z_][\\w-]*)", "g");
    var out = "", last = 0, m;
    while ((m = re.exec(code)) !== null) {
      out += esc(code.slice(last, m.index));
      if (m[1]) out += '<span class="tok-com">' + esc(m[1]) + "</span>";
      else if (m[2]) out += '<span class="tok-str">' + esc(m[2]) + "</span>";
      else if (m[3]) out += '<span class="tok-num">' + esc(m[3]) + "</span>";
      else if (words.test(m[4])) out += '<span class="tok-kw">' + esc(m[4]) + "</span>";
      else out += esc(m[4]);
      last = re.lastIndex;
    }
    return out + esc(code.slice(last));
  }

  document.querySelectorAll(".doc pre code[class*='language-']").forEach(function (el) {
    var cls = /language-([\w+-]+)/.exec(el.className);
    if (!cls) return;
    var lang = cls[1].toLowerCase();
    lang = ALIAS[lang] || lang;
    var html = highlight(el.textContent, lang);
    if (html !== null) el.innerHTML = html;
  });

  // ---- sumário: destaca a seção visível ----
  var tocLinks = Array.prototype.slice.call(document.querySelectorAll(".toc a"));
  if (tocLinks.length && "IntersectionObserver" in window) {
    var byId = {};
    tocLinks.forEach(function (a) { byId[decodeURIComponent(a.hash.slice(1))] = a; });
    var obs = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (!e.isIntersecting) return;
        tocLinks.forEach(function (a) { a.classList.remove("active"); });
        var a = byId[e.target.id];
        if (a) a.classList.add("active");
      });
    }, { rootMargin: "0px 0px -75% 0px" });
    Object.keys(byId).forEach(function (id) {
      var h = document.getElementById(id);
      if (h) obs.observe(h);
    });
  }

  // ---- busca ----
  function fold(s) {
    return s.toLowerCase().normalize("NFD").replace(/[̀-ͯ]/g, "");
  }
  var folded = null;
  function prepared() {
    if (!folded) folded = index.map(function (d) { return { t: fold(d.t), x: fold(d.x), p: fold(d.p) }; });
    return folded;
  }

  function search(query, limit) {
    var terms = fold(query).split(/\s+/).filter(function (t) { return t.length > 1; });
    if (!terms.length) return [];
    var docs = prepared(), hits = [];
    for (var i = 0; i < docs.length; i++) {
      var d = docs[i], score = 0, ok = true;
      for (var j = 0; j < terms.length; j++) {
        var t = terms[j];
        var inTitle = d.t.indexOf(t) !== -1, inPath = d.p.indexOf(t) !== -1;
        var count = 0, pos = d.x.indexOf(t);
        while (pos !== -1 && count < 50) { count++; pos = d.x.indexOf(t, pos + t.length); }
        if (!inTitle && !inPath && !count) { ok = false; break; }
        score += (inTitle ? 40 : 0) + (inPath ? 10 : 0) + Math.min(count, 50);
      }
      if (ok) hits.push({ i: i, score: score });
    }
    hits.sort(function (a, b) { return b.score - a.score; });
    return hits.slice(0, limit).map(function (h) { return { doc: index[h.i], terms: terms, i: h.i }; });
  }

  function snippet(hit) {
    var text = index[hit.i].x, low = prepared()[hit.i].x;
    var pos = -1;
    for (var j = 0; j < hit.terms.length && pos === -1; j++) pos = low.indexOf(hit.terms[j]);
    if (pos === -1) return esc(text.slice(0, 160));
    var start = Math.max(0, pos - 70), end = Math.min(text.length, pos + 130);
    var piece = esc((start ? "…" : "") + text.slice(start, end) + (end < text.length ? "…" : ""));
    hit.terms.forEach(function (t) {
      var src = t.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
      // Casa ignorando acentos: cada letra aceita suas variantes acentuadas.
      var pattern = src.split("").map(function (ch) {
        var alts = { a: "aáàâãä", e: "eéèêë", i: "iíìîï", o: "oóòôõö", u: "uúùûü", c: "cç", n: "nñ" }[ch];
        return alts ? "[" + alts + alts.toUpperCase() + "]" : ch;
      }).join("");
      piece = piece.replace(new RegExp("(" + pattern + ")", "gi"), "<mark>$1</mark>");
    });
    return piece;
  }

  function resultHtml(hit, withSnippet) {
    var d = hit.doc;
    return '<a href="' + root + d.u + '"><div class="r-title">' + esc(d.t) + '</div>' +
      '<div class="r-meta">' + esc(d.s) + " · " + esc(d.p) + "</div>" +
      (withSnippet ? '<div class="r-snip">' + snippet(hit) + "</div>" : "") + "</a>";
  }

  var form = document.querySelector(".search");
  var input = form && form.querySelector("input");
  var box = form && form.querySelector(".search-results");
  if (input && box) {
    var active = -1;
    input.addEventListener("input", function () {
      var q = input.value.trim();
      active = -1;
      if (q.length < 2) { box.hidden = true; box.innerHTML = ""; return; }
      var hits = search(q, 10);
      box.innerHTML = hits.length
        ? hits.map(function (h) { return resultHtml(h, true); }).join("")
        : '<div class="empty">Nada encontrado.</div>';
      box.hidden = false;
    });
    input.addEventListener("keydown", function (ev) {
      var links = box.querySelectorAll("a");
      if (ev.key === "ArrowDown" || ev.key === "ArrowUp") {
        if (!links.length) return;
        ev.preventDefault();
        active = (active + (ev.key === "ArrowDown" ? 1 : -1) + links.length) % links.length;
        links.forEach(function (a, k) { a.classList.toggle("active", k === active); });
        links[active].scrollIntoView({ block: "nearest" });
      } else if (ev.key === "Enter" && active >= 0 && links[active]) {
        ev.preventDefault();
        window.location.href = links[active].href;
      } else if (ev.key === "Escape") {
        box.hidden = true;
      }
    });
    document.addEventListener("click", function (ev) {
      if (!ev.target.closest(".search")) box.hidden = true;
    });
    document.addEventListener("keydown", function (ev) {
      if (ev.key === "/" && document.activeElement !== input &&
          !/input|textarea/i.test((document.activeElement || {}).tagName || "")) {
        ev.preventDefault();
        input.focus();
      }
    });
  }

  // Página de busca completa (search.html?q=...).
  var pageList = document.querySelector(".search-page-results");
  if (pageList) {
    var q = new URLSearchParams(window.location.search).get("q") || "";
    if (input) input.value = q;
    var hits = search(q, 100);
    var summary = document.querySelector(".search-summary");
    if (summary) {
      summary.textContent = q
        ? hits.length + " resultado(s) para “" + q + "”" + (hits.length === 100 ? " (mostrando os 100 primeiros)" : "")
        : "Digite um termo na caixa de busca.";
    }
    pageList.innerHTML = hits.map(function (h) { return "<li>" + resultHtml(h, true) + "</li>"; }).join("");
  }
})();
