"""Deterministic structural comparison; priorities are review cues, not legal advice."""
import hashlib
import re
from collections import defaultdict
from difflib import SequenceMatcher

from .model import Block, Change, Document, Result
from .parser import REF, STATUS, canonical, space


def key(block: Block) -> str:
    return canonical(block.text, block.kind in {"heading", "signature"})


def enumeration(text: str) -> str:
    match = re.match(r"^(\d+(?:\^\d+)?[.)]|[а-я]\))\s*", canonical(text))
    return match.group(1) if match else ""


def similarity(a: Block, b: Block) -> float:
    if a.kind != b.kind and "table" in {a.kind, b.kind}:
        return 0.0
    x, y = key(a), key(b)
    # Bound work on very long blocks while retaining the end of each paragraph.
    if len(x) > 3000:
        x = x[:1500] + x[-1500:]
    if len(y) > 3000:
        y = y[:1500] + y[-1500:]
    score = SequenceMatcher(None, x, y, autojunk=False).ratio()
    ea, eb = enumeration(a.text), enumeration(b.text)
    if ea and eb and ea != eb:
        score -= .18
    if ea and ea == eb:
        score += .10
    return min(1.0, max(0.0, score))


def _align_gap(a: list[Block], b: list[Block]):
    """Monotonic sequence alignment inside gaps between exact content anchors."""
    if not a:
        return [(None, x) for x in b]
    if not b:
        return [(x, None) for x in a]
    n, m = len(a), len(b)
    # Avoid unbounded memory on completely unrelated very large documents.
    if n * m > 250_000:
        return [(x, None) for x in a] + [(None, x) for x in b]
    costs = [[0.0] * (m + 1) for _ in range(n + 1)]
    steps = [[""] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        costs[i][0], steps[i][0] = float(i), "delete"
    for j in range(1, m + 1):
        costs[0][j], steps[0][j] = float(j), "insert"
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            score = similarity(a[i - 1], b[j - 1])
            pair_cost = (1 - score) * 1.8 if score >= .42 else 2.2
            candidates = [(costs[i - 1][j - 1] + pair_cost, "pair"),
                          (costs[i - 1][j] + 1, "delete"),
                          (costs[i][j - 1] + 1, "insert")]
            costs[i][j], steps[i][j] = min(candidates, key=lambda p: p[0])
    pairs = []
    i, j = n, m
    while i or j:
        step = steps[i][j]
        if step == "pair":
            pairs.append((a[i - 1], b[j - 1])); i -= 1; j -= 1
        elif step == "delete":
            pairs.append((a[i - 1], None)); i -= 1
        else:
            pairs.append((None, b[j - 1])); j -= 1
    return list(reversed(pairs))


def align(a: list[Block], b: list[Block]):
    pairs = []
    for tag, i, j, k, l in SequenceMatcher(None, [key(x) for x in a], [key(x) for x in b], autojunk=False).get_opcodes():
        if tag == "equal":
            pairs.extend(zip(a[i:j], b[k:l]))
        else:
            pairs.extend(_align_gap(a[i:j], b[k:l]))
    return pairs


def _note_signature(text: str):
    refs = sorted(set(REF.findall(text)))
    lowered = text.casefold()
    kind = "repeal" if STATUS.search(text) else "addition" if re.search(r"дополн|введ[её]н", lowered) else "edition" if re.search(r"ред\.|редакци", lowered) else "other"
    dates = sorted(set(re.findall(r"\d{1,2}\s+[а-я]+\s+\d{4}\s*(?:г\.|года)?|\d{2}\.\d{2}\.\d{4}", lowered)))
    return kind, refs, dates


def _priority(before: str, after: str):
    if re.search(r"\d", before + after):
        return "high", "Изменились число, дата или номер — требуется приоритетная проверка."
    if re.search(r"\b(?:не|ни|нет|запрещается|разрешается|обязан|вправе)\b", before + " " + after, re.I):
        return "high", "Изменились отрицание или слова, влияющие на условие нормы."
    return "medium", "Изменился текст документа."


def compare(left: Document, right: Document) -> Result:
    changes = []
    warnings = [*left.warnings, *right.warnings]

    def add(category, priority, reason, a, b, before, after, context=""):
        payload = f"{category}|{a.id if a else ''}|{b.id if b else ''}|{before}|{after}"
        ident = hashlib.sha256(payload.encode()).hexdigest()[:16]
        changes.append(Change(ident, category, priority, reason, a.id if a else None,
                              b.id if b else None, before, after, context or ((a or b).section if a or b else ""),
                              original_priority=priority))

    if left.act_number and right.act_number and (left.act_date, left.act_number) != (right.act_date, right.act_number):
        warnings.append("Реквизиты актов различаются. Возможно, выбраны разные документы.")
    if left.amendments and right.amendments and left.amendments != right.amendments:
        warnings.append("Списки изменяющих актов различаются: проверьте совместимость редакций.")
    elif left.revision != right.revision:
        warnings.append("Даты редакций / состояния различаются. Совпадение списка изменений не подтверждает одинаковую дату действия.")
    if not left.act_number or not right.act_number:
        warnings.append("Не все реквизиты распознаны. Сопоставление выбранных документов нужно подтвердить вручную.")

    def compare_text(a, b, before, after, category=None, context=""):
        heading = a.kind == b.kind and a.kind in {"heading", "signature"}
        x, y = canonical(before, heading), canonical(after, heading)
        if x == y:
            if space(before) != space(after):
                add("format", "low", "Одинаковое содержание при разных регистре или записи обозначений.", a, b, before, after, context)
            return
        tokens_a = re.findall(r"\w+|[^\w\s]", x)
        tokens_b = re.findall(r"\w+|[^\w\s]", y)
        groups = defaultdict(lambda: [[], []])
        for tag, i, j, k, l in SequenceMatcher(None, tokens_a, tokens_b, autojunk=False).get_opcodes():
            if tag == "equal":
                continue
            old, new = " ".join(tokens_a[i:j]), " ".join(tokens_b[k:l])
            if re.search(r"\d", old + new):
                cat = "numbers"
            elif re.search(r"\w", old + new):
                cat = "words"
            else:
                cat = "punctuation"
            groups[cat][0].append(old)
            groups[cat][1].append(new)
        for cat, (old, new) in groups.items():
            priority, reason = _priority(" ".join(old), " ".join(new))
            if cat == "punctuation":
                priority, reason = "medium", "Изменилась пунктуация; она может влиять на чтение нормы."
            add(category or cat, priority, reason, a, b, before, after, context)

    def compare_notes(a, b):
        x, y = " ".join(n.text for n in a.notes), " ".join(n.text for n in b.notes)
        if canonical(x) != canonical(y):
            sx, sy = _note_signature(x), _note_signature(y)
            if sx != sy:
                priority = "high" if (sx[0] == "repeal" or sy[0] == "repeal" or sx[1] != sy[1] or sx[2] != sy[2]) else "medium"
                reason = "Различаются реквизиты, вид отметки, дата действия или наличие отметки."
            else:
                priority, reason = "low", "Реквизиты и распознанный вид отметки совпадают; различается формулировка."
            add("notes", priority, reason, a, b, x, y)
        if x and y and {n.position for n in a.notes} != {n.position for n in b.notes}:
            add("note_position", "low", "Отметки размещены внутри текста и отдельным блоком.", a, b, x, y)

    def compare_table(a, b):
        rows_a, rows_b = defaultdict(list), defaultdict(list)
        for c in a.cells:
            rows_a[c.row].append(c)
        for c in b.cells:
            rows_b[c.row].append(c)
        ra = [Block(str(i), " | ".join(c.text for c in row)) for i, row in rows_a.items()]
        rb = [Block(str(i), " | ".join(c.text for c in row)) for i, row in rows_b.items()]
        for la, lb in align(ra, rb):
            if la is None or lb is None:
                add("tables", "high", "Добавлена или отсутствует строка таблицы.", a, b,
                    la.text if la else "", lb.text if lb else "", f"Строка {int((la or lb).id) + 1}")
                continue
            ca, cb = rows_a[int(la.id)], rows_b[int(lb.id)]
            ma, mb = {c.col: c for c in ca}, {c.col: c for c in cb}
            for col in sorted(ma.keys() | mb.keys()):
                u, v = ma.get(col), mb.get(col)
                context = f"Таблица: строка слева {int(la.id) + 1}, справа {int(lb.id) + 1}, столбец {col + 1}"
                if u is None or v is None:
                    add("tables", "high", "Изменилось число или расположение ячеек.", a, b, u.text if u else "", v.text if v else "", context)
                else:
                    compare_text(a, b, u.text, v.text, "tables", context)
                    if (u.rowspan, u.colspan) != (v.rowspan, v.colspan):
                        add("tables", "medium", "Изменилось объединение ячеек.", a, b,
                            f"{u.rowspan} × {u.colspan}", f"{v.rowspan} × {v.colspan}", context)

    for a, b in align(left.blocks, right.blocks):
        if a is None or b is None:
            block = a or b
            add("structure", "high", "Фрагмент есть только в одном документе; возможен пропуск или перенос.",
                a, b, block.text if a else "", block.text if b else "")
            continue
        if a.kind == b.kind == "table":
            compare_table(a, b)
        else:
            compare_text(a, b, a.text, b.text)
        compare_notes(a, b)
        if a.links != b.links:
            add("links", "low", "Различаются гиперссылки или команды перехода. Адреса разных систем не доказывают различие целевого акта.",
                a, b, "\n".join(f"{t} → {v}" for t, v in a.links), "\n".join(f"{t} → {v}" for t, v in b.links))
    return Result(left, right, changes, list(dict.fromkeys(warnings)))
