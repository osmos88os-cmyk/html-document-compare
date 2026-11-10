from dataclasses import dataclass, field

CATEGORIES = {
    "words": "Слова", "numbers": "Числа и даты", "punctuation": "Пунктуация",
    "notes": "Редакционные отметки", "note_position": "Расположение отметок",
    "links": "Ссылки", "tables": "Таблицы", "structure": "Структура и пропуски",
    "format": "Оформление",
}
PRIORITIES = {"high": "Высокий", "medium": "Средний", "low": "Низкий"}
REVIEWS = {"new": "Не проверено", "confirmed": "Подтверждено", "ignored": "Допустимо"}


@dataclass
class Note:
    text: str
    position: str = "inline"


@dataclass
class Cell:
    row: int
    col: int
    text: str
    rowspan: int = 1
    colspan: int = 1
    header: bool = False


@dataclass
class Block:
    id: str
    text: str
    kind: str = "paragraph"
    notes: list[Note] = field(default_factory=list)
    links: list[tuple[str, str]] = field(default_factory=list)
    cells: list[Cell] = field(default_factory=list)
    section: str = ""
    source_line: int | None = None


@dataclass
class Document:
    name: str
    sha256: str
    source: str
    blocks: list[Block]
    title: str = ""
    act_date: str = ""
    act_number: str = ""
    revision: str = ""
    amendments: list[tuple[str, str]] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


@dataclass
class Change:
    id: str
    category: str
    priority: str
    reason: str
    left_id: str | None
    right_id: str | None
    before: str
    after: str
    context: str = ""
    review: str = "new"
    original_priority: str = ""


@dataclass
class Result:
    left: Document
    right: Document
    changes: list[Change]
    warnings: list[str] = field(default_factory=list)
