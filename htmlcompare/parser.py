"""Parse source-specific markup into content, annotations and logical tables.

No scripts, URLs, stylesheets or images from input documents are executed/loaded.
"""
import hashlib
import re
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from lxml import etree, html

from .model import Block, Cell, Document, Note

SPACE = re.compile(r"\s+")
REF = re.compile(r"(?:от\s*)?(\d{2}\.\d{2}\.\d{4})\s*[N№]\s*([\d]+(?:[-–][\w]+)?)", re.I)
NOTE_START = re.compile(r"^\(?\s*(?:в ред\.|в редакции|дополнен|дополнение|введен|введён|пункт введен|подпункт введен|пп?\.\s*\S+\s+(?:введен|в ред\.)|абзац введен|примечани[ея]\b|см\. текст)", re.I)
STATUS = re.compile(r"утратил[аио]?\s+силу", re.I)
VOID = {"br", "hr", "img", "input", "meta", "link"}
META_PREFIX = ("Документ предоставлен", "Дата сохранения:", "Недействующая редакция", "По состоянию на", "См. последующие изменения")


def space(text: str) -> str:
    return SPACE.sub(" ", text.replace("\u00ad", "")).strip()


def canonical(text: str, heading: bool = False) -> str:
    value = space(text)
    value = re.sub(r"\bN\s*(?=\d)", "№ ", value)
    value = re.sub(r"№\s*(?=\d)", "№ ", value)
    # Only enumeration prefixes: 27(1)) / 27¹) / 27^1) represent one index.
    value = re.sub(r"^(\d+)\((\d+)\)([.)])", r"\1^\2\3", value)
    value = re.sub(r"^(\d+)\.(\d+)([.)])", r"\1^\2\3", value)
    return value.casefold() if heading else value


def _flatten(node, marked: bool = False) -> list[tuple[str, bool]]:
    tag = str(node.tag).lower()
    if tag in {"script", "style", "noscript", "label", "input"}:
        return []
    marked = marked or any(c.startswith("mark") for c in node.get("class", "").split())
    out = [(node.text or "", marked)]
    for child in node:
        if not isinstance(child.tag, str):
            out.append((child.tail or "", marked))
            continue
        if child.tag == "br":
            out.append((" ", marked))
        elif child.tag == "sup" or "W9" in child.get("class", "").split():
            out.append(("^" + space(child.text_content()), marked))
        else:
            out.extend(_flatten(child, marked))
        out.append((child.tail or "", marked))
    return out


def _parts(node) -> tuple[str, list[Note]]:
    pieces = _flatten(node)
    body, marked = [], []
    for text, is_note in pieces:
        (marked if is_note else body).append(text)
    main = space("".join(body))
    note = space("".join(marked))
    notes = [Note(note)] if note else []
    # In these exports 'абзац;' may sit inside OR outside the mark spans.
    if STATUS.search(note) and re.fullmatch(r"(?:\d+(?:\^\d+)?[.)]?\s*)?(?:абзац[;.]?)?", main, re.I):
        prefix = re.match(r"\d+(?:\^\d+)?[.)]?", main)
        main = ((prefix.group() + " ") if prefix else "абзац ") + "[Утратил силу]"
    return main, notes


def _refs(node) -> list[tuple[str, str]]:
    result = []
    for a in node.xpath('.//a[@href]|.//span[@cmdprm]'):
        if any(any(c.startswith("mark") for c in p.get("class", "").split()) for p in [a, *a.iterancestors()]):
            continue
        label = space(a.text_content())
        href = a.get("href", "")
        cmd = a.get("cmdprm", "")
        if href.startswith("#"):
            target = "внутренняя:" + href[1:]
        elif cmd:
            target = "команда:" + cmd
        else:
            parsed = urlparse(href)
            query = parse_qs(parsed.query)
            # Preserve destination identifiers; equality across providers is not guessed.
            target = parsed.netloc.lower() + parsed.path + "?" + "&".join(f"{k}={','.join(v)}" for k, v in sorted(query.items()))
        if label:
            result.append((label, target))
    return result


def _cells(table) -> list[Cell]:
    cells, occupied = [], set()
    for row, tr in enumerate(table.xpath('./tr|./thead/tr|./tbody/tr|./tfoot/tr')):
        col = 0
        for td in tr.xpath('./td|./th'):
            while (row, col) in occupied:
                col += 1
            try:
                rs = max(1, min(1000, int(td.get("rowspan", "1"))))
                cs = max(1, min(1000, int(td.get("colspan", "1"))))
            except ValueError:
                rs, cs = 1, 1
            text = space(" ".join(t for t, _ in _flatten(td)))
            cells.append(Cell(row, col, text, rs, cs, td.tag == "th"))
            if len(occupied) + rs * cs > 100_000:
                raise ValueError("Слишком сложная таблица: больше 100 000 позиций ячеек")
            occupied.update((r, c) for r in range(row, row + rs) for c in range(col, col + cs))
            col += cs
    return cells


def parse_html(raw: bytes, name: str = "Документ.html") -> Document:
    if len(raw) > 50 * 1024 * 1024:
        raise ValueError("Файл больше 50 МБ. Разделите документ перед сверкой.")
    if not raw.strip():
        raise ValueError("HTML-файл пуст")
    charset = re.search(rb'charset\s*=\s*["\']?([\w-]+)', raw[:10000], re.I)
    encoding = charset.group(1).decode("ascii") if charset else None
    if not encoding:
        try:
            raw.decode("utf-8-sig")
            encoding = "utf-8"
        except UnicodeDecodeError:
            encoding = "windows-1251"
    try:
        root = html.fromstring(raw, parser=html.HTMLParser(encoding=encoding, no_network=True))
    except (etree.ParserError, LookupError) as error:
        raise ValueError(f"Не удалось прочитать HTML: {error}") from error
    body = root.find("body")
    if body is None:
        body = root
    fulltext = space(body.text_content())
    source = "К+" if root.get("xmlns") == "http://www.w3.org/1999/xhtml" or "Документ предоставлен КонсультантПлюс" in fulltext else "HTML"
    if source == "HTML" and root.xpath('//*[@class="fragments"]'):
        source = "Комплекс / ТЕХН"
    if "Недействующая редакция" in fulltext[:600]:
        source = "ГПУ"
    title = space(" ".join(root.xpath('//title/text()')))
    refs = REF.findall(fulltext)
    act_date, act_number = refs[0] if refs and source in {"К+", "ГПУ"} else ("", "")
    if not act_date:
        match = re.search(r"(?:УП|Указ.*?)\s+(\d+)\s+от\s+(\d{2}\.\d{2}\.\d{4})", name, re.I)
        if match:
            act_number, act_date = match.groups()
    revision_match = re.search(r"(?:По состоянию на|ред\.\s*от|\sна)\s*(\d{2}\.\d{2}\.\d{4})", fulltext[:1800] + " " + name, re.I)
    revision = revision_match.group(1) if revision_match else ""
    blocks = []
    amendments = []
    warnings = []
    section = ""
    candidates = []

    def collect(node):
        if not isinstance(node.tag, str) or node.tag in {"script", "style", "head", "noscript", "label", "input", "svg"}:
            return
        if node.tag == "table":
            text = space(node.text_content())
            rows = node.xpath('./tr|./tbody/tr|./thead/tr|./tfoot/tr')
            layout = len(rows) <= 1 and ("Список изменяющих документов" in text or bool(re.match(r"^\d{1,2}\s+[а-я]+\s+\d{4}\s+года", text, re.I)))
            if layout:
                if "Список изменяющих документов" in text:
                    amendments.extend(REF.findall(text))
                return
            candidates.append(node)
            return
        if node.tag in {"p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "pre"}:
            candidates.append(node)
            return
        has_blocks = any(isinstance(c.tag, str) and c.tag in {"div", "p", "table", "ul", "ol", "h1", "h2", "h3", "pre", "section", "article"} for c in node)
        if node.tag == "div" and not has_blocks:
            candidates.append(node)
            return
        if not has_blocks and node is body and space(node.text_content()):
            candidates.append(node)
            return
        # Preserve loose text around block elements rather than silently losing it.
        if space(node.text or ""):
            p = html.Element("p")
            p.text = node.text
            candidates.append(p)
        for child in node:
            collect(child)
            if space(child.tail or ""):
                p = html.Element("p")
                p.text = child.tail
                candidates.append(p)

    collect(body)
    started = source != "К+"
    for node in candidates:
        text, notes = _parts(node)
        rawtext = space(node.text_content())
        classes = node.get("class", "").split()
        if source == "К+" and not started:
            if rawtext == "УКАЗ" or rawtext.startswith("В целях"):
                started = True
            else:
                continue
        if not text and not notes:
            continue
        if rawtext.startswith(META_PREFIX) or "ihint" in node.get("class", ""):
            continue
        if source == "ГПУ" and not blocks and rawtext.startswith("Указ Президента Российской Федерации от"):
            continue
        if "Список изменяющих документов" in rawtext:
            amendments.extend(REF.findall(rawtext))
            continue
        if notes and not text and REF.search(rawtext) and not blocks:
            amendments.extend(REF.findall(rawtext))
            continue
        if notes and not text:
            if blocks:
                for n in notes:
                    n.position = "after"
                blocks[-1].notes.extend(notes)
            continue
        if source == "К+" and NOTE_START.match(rawtext):
            if blocks and not rawtext.lower().startswith("(см. текст"):
                blocks[-1].notes.append(Note(rawtext, "after"))
            continue
        # K+ expresses repeal notices as standalone plain text; retain the status.
        if source == "К+" and STATUS.search(text) and re.match(r"^(?:\d+(?:\([\d]+\))?[.)]?\s*|абзац\s+)?утратил", text, re.I):
            prefix = re.match(r"\d+(?:\([\d]+\))?[.)]?", text)
            notes = [Note(text, "after")]
            text = ((prefix.group() + " ") if prefix else "абзац ") + "[Утратил силу]"
        kind = "paragraph"
        if node.tag == "table":
            kind = "table"
        elif "aR" in classes or any(c in {"Y", "S"} for c in classes):
            kind = "signature"
        elif node.tag.startswith("h") or any(c in {"T", "Z", "s2B"} for c in classes) or re.match(r"^[IVXLCDM]+\.\s", text):
            kind = "heading"
        if kind == "heading":
            section = text[:200]
        block = Block(f"b{len(blocks)}", text, kind, notes, _refs(node), section=section, source_line=node.sourceline)
        if kind == "table":
            block.cells = _cells(node)
            if node.xpath('.//table'):
                warnings.append("Вложенная таблица: проверьте сопоставление её ячеек вручную.")
        # Exports split one heading into a different number of visual lines.
        if kind in {"heading", "signature"} and blocks and blocks[-1].kind == kind:
            blocks[-1].text += " " + text
            blocks[-1].notes.extend(notes)
            blocks[-1].section = blocks[-1].text[:200]
            if kind == "heading":
                section = blocks[-1].section
        else:
            blocks.append(block)
    if not blocks:
        raise ValueError("В файле не найден текст документа")
    if len(blocks) > 6000:
        raise ValueError("Больше 6000 блоков: разделите документ на части перед сверкой.")
    if not amendments:
        # Top-level list in Complex sits in markx, following the heading.
        for node in candidates[:20]:
            t = space(node.text_content())
            if t.startswith("(В редакции") or t.startswith("(в ред."):
                amendments = REF.findall(t)
                break
    return Document(name, hashlib.sha256(raw).hexdigest(), source, blocks, title,
                    act_date, act_number, revision, sorted(set(amendments)), list(dict.fromkeys(warnings)))


def parse_file(path: str | Path) -> Document:
    path = Path(path)
    return parse_html(path.read_bytes(), path.name)
