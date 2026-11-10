import html
import json
import re
from collections import defaultdict
from dataclasses import asdict
from datetime import datetime, timezone
from difflib import SequenceMatcher
from pathlib import Path

from .model import CATEGORIES, PRIORITIES, REVIEWS, Result

CSS = """
body { color:#202c3b; background:white; font-family:'Segoe UI',Arial,sans-serif; font-size:12pt; line-height:1.5; margin:20px; }
p { margin:10px 0; } h2 { font-size:15pt; color:#173b57; margin-top:22px; }
.note { color:#697a86; font-size:10pt; margin-top:4px; }
.high { background-color:#ffe0dc; } .medium { background-color:#fff0c4; } .low { background-color:#dfeefb; }
.muted { color:#708393; } .context { color:#52677a; font-size:10pt; }
table { border-collapse:collapse; width:100%; margin:12px 0; } td,th { border:1px solid #bccbd5; padding:7px; vertical-align:top; }
th { background:#eff4f7; } a { color:#126c80; text-decoration:none; } .card { margin-bottom:20px; }
"""
e = html.escape


def highlighted(text: str, other: str, color: str) -> str:
    a = re.findall(r"\w+|[^\w\s]|\s+", text)
    b = re.findall(r"\w+|[^\w\s]|\s+", other)
    parts = []
    for tag, i, j, _, _ in SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        value = e("".join(a[i:j])).replace("\n", "<br>")
        parts.append(value if tag == "equal" else f'<span style="background-color:{color};">{value}</span>')
    return "".join(parts)


def render_document(result: Result, side: str, categories: set[str], selected: str = "", change_ids: set[str] | None = None) -> str:
    doc = getattr(result, side)
    other = result.right if side == "left" else result.left
    other_blocks = {b.id: b for b in other.blocks}
    by_block = defaultdict(list)
    for c in result.changes:
        ident = c.left_id if side == "left" else c.right_id
        if ident and c.category in categories and (change_ids is None or c.id in change_ids):
            by_block[ident].append(c)
    out = [f"<html><head><style>{CSS}</style></head><body>"]
    for block in doc.blocks:
        changes = by_block[block.id]
        priority = min((c.priority for c in changes), key=lambda x: {"high": 0, "medium": 1, "low": 2}[x]) if changes else ""
        color = {"high": "#ffe0dc", "medium": "#fff0c4", "low": "#dfeefb"}.get(priority, "white")
        tag = "h2" if block.kind == "heading" else "p"
        attrs = f' style="background-color:{color};"' if changes else ""
        text = e(block.text)
        text_changes = [c for c in changes if c.category not in {"links", "notes", "note_position"}]
        if text_changes and block.kind != "table":
            paired = next((other_blocks.get(c.right_id if side == "left" else c.left_id) for c in text_changes), None)
            text = highlighted(block.text, paired.text if paired else "", color)
            attrs = ""
        if selected and any(c.id == selected for c in changes):
            out.append('<p style="color:#147a84; font-weight:bold;">▶ Выбранное различие</p>')
        out.append(f'<a name="{block.id}"></a>')
        if block.kind == "table":
            out.append(f'<table{attrs}>')
            rows = defaultdict(list)
            for c in block.cells:
                rows[c.row].append(c)
            for cells in rows.values():
                out.append("<tr>")
                for cell in cells:
                    celltag = "th" if cell.header else "td"
                    cell_changes = []
                    for change in changes:
                        pos = re.search(r"строка слева (\d+), справа (\d+), столбец (\d+)", change.context)
                        row_only = re.fullmatch(r"Строка (\d+)", change.context)
                        if pos and int(pos.group(1 if side == "left" else 2)) == cell.row + 1 and int(pos.group(3)) == cell.col + 1:
                            cell_changes.append(change)
                        elif row_only and int(row_only.group(1)) == cell.row + 1 and (change.before if side == "left" else change.after):
                            cell_changes.append(change)
                    style = f' style="background-color:{color};"' if cell_changes else ""
                    value = e(cell.text)
                    if cell_changes:
                        change = cell_changes[0]
                        value = highlighted(cell.text, change.after if side == "left" else change.before, color)
                    out.append(f'<{celltag} rowspan="{cell.rowspan}" colspan="{cell.colspan}"{style}>{value}</{celltag}>')
                out.append("</tr>")
            out.append("</table>")
        else:
            out.append(f"<{tag}{attrs}>{text}</{tag}>")
        # Notes remain visible; the checkboxes govern highlights and the change list.
        for n in block.notes:
            note_cls = priority if any(c.category in {"notes", "note_position"} for c in changes) else ""
            out.append(f'<p class="note {note_cls}">{e(n.text)}</p>')
        if changes:
            out.append('<p class="context">' + " · ".join(dict.fromkeys(CATEGORIES[c.category] for c in changes)) + "</p>")
    return "".join(out) + "</body></html>"


def save_session(result: Result, path: str | Path):
    data = {"schema": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
            "left": {"name": result.left.name, "sha256": result.left.sha256},
            "right": {"name": result.right.name, "sha256": result.right.sha256},
            "reviews": {c.id: {"review": c.review, "priority": c.priority} for c in result.changes}}
    Path(path).write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")


def load_session(result: Result, path: str | Path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if data.get("schema") != 1:
        raise ValueError("Неподдерживаемая версия файла проверки")
    if any(data.get(side, {}).get("sha256") != getattr(result, side).sha256 for side in ("left", "right")):
        raise ValueError("Файл проверки относится к другой паре документов или документы были изменены")
    reviews = data.get("reviews", {})
    for c in result.changes:
        saved = reviews.get(c.id, {})
        if saved.get("review") in REVIEWS:
            c.review = saved["review"]
        if saved.get("priority") in PRIORITIES:
            c.priority = saved["priority"]


def export_report(result: Result, path: str | Path):
    out = [f'<html lang="ru"><meta charset="utf-8"><title>Отчёт сверки HTML</title><style>{CSS}</style><body>',
           '<h1>Отчёт сверки HTML</h1>', '<p>Приоритет — подсказка проверяющему. Итоговую оценку делает специалист.</p>',
           f'<p><b>Слева:</b> {e(result.left.name)}<br><b>Справа:</b> {e(result.right.name)}</p>',
           f'<p class="context">SHA-256 слева: {result.left.sha256}<br>SHA-256 справа: {result.right.sha256}</p>',
           '<p>Отчёт содержит все категории, включая скрытые в интерфейсе.</p>']
    out.extend(f'<p class="medium">{e(w)}</p>' for w in result.warnings)
    for i, c in enumerate(result.changes, 1):
        out.append(f'<div class="card"><h2>{i}. {e(CATEGORIES[c.category])} · {PRIORITIES[c.priority]}</h2>'
                   f'<p>{e(c.reason)}</p><p class="context">{e(c.context)}</p>'
                   f'<p>Решение: <b>{REVIEWS[c.review]}</b></p>'
                   '<table><tr><th>Слева</th><th>Справа</th></tr><tr>'
                   f'<td>{highlighted(c.before, c.after, "#ffe0dc") or "—"}</td>'
                   f'<td>{highlighted(c.after, c.before, "#dcf2e8") or "—"}</td></tr></table></div>')
    if not result.changes:
        out.append("<p>Различий по применённым правилам не обнаружено.</p>")
    out.append("</body></html>")
    Path(path).write_text("".join(out), encoding="utf-8")
