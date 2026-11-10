"""Russian desktop UI. Input content is only rendered as escaped, local rich text."""
import argparse
import sys
from pathlib import Path

from PySide6.QtCore import Qt, QThread, Signal, QSettings, QTimer, QUrl
from PySide6.QtGui import QColor, QFont, QKeySequence, QShortcut, QTextDocument
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QFileDialog, QFrame, QGridLayout, QHBoxLayout,
    QLabel, QLineEdit, QMainWindow, QMessageBox, QPushButton, QSplitter,
    QTextBrowser, QTreeWidget, QTreeWidgetItem, QVBoxLayout, QWidget,
)

from . import __version__
from .compare import compare
from .model import CATEGORIES, PRIORITIES, REVIEWS, Result
from .parser import parse_file
from .render import CSS, e, export_report, load_session, render_document, save_session

STYLE = """
QMainWindow, QWidget { background:#f3f6f8; color:#24394b; font-family:'Segoe UI'; font-size:13px; }
QFrame#header, QFrame#header QLabel { background:#162e42; color:white; }
QLabel#title { font-size:23px; font-weight:700; } QLabel#subtitle { color:#a6c0d3; font-size:12px; }
QFrame#card { background:white; border:1px solid #dce5eb; border-radius:9px; }
QFrame#card QLabel { background:transparent; } QLabel#file { font-size:14px; font-weight:600; }
QLabel#muted { color:#728696; } QLabel#summary { font-size:15px; font-weight:600; padding:6px; }
QPushButton { background:white; border:1px solid #ccd9e2; border-radius:6px; padding:8px 12px; }
QPushButton:hover { border-color:#168b96; background:#e8f5f5; }
QPushButton:disabled { color:#9baab6; background:#eef2f4; }
QPushButton#primary { background:#147d88; border:1px solid #147d88; color:white; font-weight:700; padding:10px 22px; }
QPushButton#primary:hover { background:#116873; }
QTextBrowser { background:white; border:1px solid #dce5eb; border-radius:6px; padding:6px; selection-background-color:#bbdde7; }
QLineEdit, QComboBox { background:white; border:1px solid #ccd9e2; border-radius:5px; padding:6px; }
QTreeWidget { background:white; border:1px solid #dce5eb; border-radius:6px; alternate-background-color:#f7fafb; }
QTreeWidget::item { padding:8px 3px; } QTreeWidget::item:selected { color:#173d51; background:#dceff2; }
QHeaderView::section { background:#eaf0f4; border:0; padding:8px; color:#4d6678; }
QCheckBox { spacing:6px; padding:3px; } QSplitter::handle { background:#e1e8ed; }
QStatusBar { background:#eaf0f4; color:#61798b; }
"""


class LocalBrowser(QTextBrowser):
    """Do not load file: / http: resources embedded in input or report text."""
    def loadResource(self, resource_type, url):
        return None


class Worker(QThread):
    done = Signal(object)
    failed = Signal(str)

    def __init__(self, left, right):
        super().__init__()
        self.left, self.right = left, right

    def run(self):
        try:
            self.done.emit(compare(parse_file(self.left), parse_file(self.right)))
        except Exception as error:
            self.failed.emit(str(error))


class FilePane(QFrame):
    chosen = Signal()

    def __init__(self, title, settings):
        super().__init__()
        self.setObjectName("card")
        self.path = ""
        self.can_choose = lambda: True
        self.settings = settings
        layout = QVBoxLayout(self)
        row = QHBoxLayout()
        label = QLabel(title)
        label.setObjectName("muted")
        row.addWidget(label)
        row.addStretch()
        self.button = QPushButton("Выбрать HTML…")
        self.button.clicked.connect(self.choose)
        row.addWidget(self.button)
        layout.addLayout(row)
        self.file = QLabel("Документ не выбран")
        self.file.setWordWrap(True)
        self.file.setObjectName("file")
        layout.addWidget(self.file)
        self.meta = QLabel("Файлы остаются на этом компьютере")
        self.meta.setWordWrap(True)
        self.meta.setObjectName("muted")
        layout.addWidget(self.meta)

    def choose(self):
        if not self.can_choose():
            return
        path, _ = QFileDialog.getOpenFileName(self, "Выбрать документ", self.settings.value("directory", ""), "HTML (*.html *.htm);;Все файлы (*)")
        if path:
            self.set_path(path)
            self.settings.setValue("directory", str(Path(path).parent))
            self.chosen.emit()

    def set_path(self, path):
        self.path = str(path)
        self.file.setText(Path(path).name)
        self.file.setToolTip(str(path))
        self.meta.setText("Готов к сверке")

    def set_document(self, doc):
        info = [doc.source]
        if doc.act_number:
            info.append(f"№ {doc.act_number} от {doc.act_date}")
        if doc.revision:
            info.append(f"Редакция / состояние: {doc.revision}")
        info.append(f"Блоков: {len(doc.blocks)}")
        self.meta.setText(" · ".join(info))


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"Сверка HTML · {__version__}")
        self.resize(1480, 960)
        self.setMinimumSize(1060, 720)
        self.settings = QSettings("HTMLCompare", "HTMLCompare")
        self.result: Result | None = None
        self.worker = None
        self.dirty = False
        self.syncing = False
        self.updating = False
        self.visible_changes = []
        root = QWidget()
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)
        layout.setContentsMargins(16, 12, 16, 8)
        layout.setSpacing(10)

        header = QFrame()
        header.setObjectName("header")
        head = QHBoxLayout(header)
        head.setContentsMargins(18, 14, 18, 14)
        stack = QVBoxLayout()
        title = QLabel("Сверка HTML")
        title.setObjectName("title")
        subtitle = QLabel("Документы · редакционные отметки · таблицы")
        subtitle.setObjectName("subtitle")
        stack.addWidget(title)
        stack.addWidget(subtitle)
        head.addLayout(stack)
        head.addStretch()
        head.addWidget(QLabel("●  Локальная обработка"))
        layout.addWidget(header)

        files = QHBoxLayout()
        self.left_file = FilePane("ЛЕВЫЙ ДОКУМЕНТ · исходный / ТЕХН", self.settings)
        self.right_file = FilePane("ПРАВЫЙ ДОКУМЕНТ · для сравнения", self.settings)
        for pane in (self.left_file, self.right_file):
            files.addWidget(pane, 1)
            pane.chosen.connect(self.files_changed)
            pane.can_choose = self.confirm_discard
        layout.addLayout(files)

        toolbar = QHBoxLayout()
        self.compare_button = QPushButton("Сравнить документы")
        self.compare_button.setObjectName("primary")
        self.compare_button.setEnabled(False)
        self.compare_button.clicked.connect(self.start_compare)
        toolbar.addWidget(self.compare_button)
        self.summary = QLabel("Выберите два HTML-файла")
        self.summary.setObjectName("summary")
        toolbar.addWidget(self.summary, 1)
        self.sync = QCheckBox("Синхронная прокрутка")
        self.sync.setChecked(True)
        self.sync.setToolTip("При ручной прокрутке — по относительной позиции; по выбору различия — к паре блоков.")
        toolbar.addWidget(self.sync)
        self.report_button = QPushButton("HTML-отчёт…")
        self.report_button.clicked.connect(self.export)
        self.report_button.setEnabled(False)
        toolbar.addWidget(self.report_button)
        layout.addLayout(toolbar)

        filter_layout = QGridLayout()
        self.categories = {}
        for index, (category, name) in enumerate(CATEGORIES.items()):
            check = QCheckBox(name)
            check.setChecked(category not in {"format", "links", "note_position"})
            check.toggled.connect(self.refilter)
            self.categories[category] = check
            filter_layout.addWidget(check, index // 5, index % 5)
        layout.addLayout(filter_layout)
        self.warning = QLabel()
        self.warning.setWordWrap(True)
        self.warning.setStyleSheet("background:#fff0c4; color:#735921; border-radius:5px; padding:8px;")
        self.warning.hide()
        layout.addWidget(self.warning)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        sidebar = QWidget()
        side = QVBoxLayout(sidebar)
        side.setContentsMargins(0, 0, 4, 0)
        row = QHBoxLayout()
        self.priority_filter = QComboBox()
        self.priority_filter.addItem("Любой приоритет", "")
        for k, v in PRIORITIES.items():
            self.priority_filter.addItem(v, k)
        self.priority_filter.currentIndexChanged.connect(self.refilter)
        row.addWidget(self.priority_filter)
        self.review_filter = QComboBox()
        self.review_filter.addItem("Все решения", "")
        for k, v in REVIEWS.items():
            self.review_filter.addItem(v, k)
        self.review_filter.currentIndexChanged.connect(self.refilter)
        row.addWidget(self.review_filter)
        side.addLayout(row)
        self.search = QLineEdit()
        self.search.setPlaceholderText("Поиск по различиям…")
        self.search.textChanged.connect(self.refilter)
        side.addWidget(self.search)
        self.change_list = QTreeWidget()
        self.change_list.setHeaderLabels(["Различие", "Приоритет"])
        self.change_list.setRootIsDecorated(False)
        self.change_list.setAlternatingRowColors(True)
        self.change_list.setColumnWidth(0, 225)
        self.change_list.currentItemChanged.connect(self.select_change)
        side.addWidget(self.change_list, 3)
        nav = QHBoxLayout()
        for name, delta in [("← Предыдущее", -1), ("Следующее →", 1)]:
            button = QPushButton(name)
            button.clicked.connect(lambda checked=False, d=delta: self.navigate(d))
            nav.addWidget(button)
        side.addLayout(nav)
        self.detail = LocalBrowser()
        self.detail.setMaximumHeight(180)
        self.detail.setHtml("<p>Выберите различие, чтобы увидеть причину приоритета.</p>")
        side.addWidget(self.detail, 1)
        review_row = QHBoxLayout()
        self.review_edit = QComboBox()
        for k, v in REVIEWS.items():
            self.review_edit.addItem(v, k)
        self.review_edit.currentIndexChanged.connect(self.update_review)
        review_row.addWidget(self.review_edit)
        self.priority_edit = QComboBox()
        for k, v in PRIORITIES.items():
            self.priority_edit.addItem(v, k)
        self.priority_edit.currentIndexChanged.connect(self.update_review)
        review_row.addWidget(self.priority_edit)
        side.addLayout(review_row)
        sessions = QHBoxLayout()
        self.save_button = QPushButton("Сохранить проверку…")
        self.save_button.clicked.connect(self.save)
        self.load_button = QPushButton("Открыть проверку…")
        self.load_button.clicked.connect(self.load)
        for b in (self.save_button, self.load_button):
            b.setEnabled(False)
            sessions.addWidget(b)
        side.addLayout(sessions)
        splitter.addWidget(sidebar)
        documents = QSplitter(Qt.Orientation.Horizontal)
        self.left_view, self.right_view = LocalBrowser(), LocalBrowser()
        for view in (self.left_view, self.right_view):
            view.setOpenLinks(False)
            view.setOpenExternalLinks(False)
            view.setHtml(f'<html><style>{CSS}</style><body><h2>Документ появится здесь</h2><p class="muted">Выберите файлы и запустите сверку.</p></body></html>')
            documents.addWidget(view)
        documents.setSizes([500, 500])
        splitter.addWidget(documents)
        splitter.setSizes([350, 1100])
        layout.addWidget(splitter, 1)
        self.left_view.verticalScrollBar().valueChanged.connect(lambda v: self.scroll_sync(self.left_view, self.right_view, v))
        self.right_view.verticalScrollBar().valueChanged.connect(lambda v: self.scroll_sync(self.right_view, self.left_view, v))
        self.statusBar().showMessage("Готово · Приоритет требует оценки проверяющего · F7 / Shift+F7 — переход по различиям")
        QShortcut(QKeySequence("F7"), self, activated=lambda: self.navigate(1))
        QShortcut(QKeySequence("Shift+F7"), self, activated=lambda: self.navigate(-1))
        self.review_edit.setEnabled(False)
        self.priority_edit.setEnabled(False)

    def confirm_discard(self):
        if not self.dirty:
            return True
        answer = QMessageBox.question(self, "Несохранённая проверка", "Решения проверяющего ещё не сохранены. Продолжить без сохранения?",
                                      QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No, QMessageBox.StandardButton.No)
        return answer == QMessageBox.StandardButton.Yes

    def files_changed(self):
        self.result = None
        self.dirty = False
        self.change_list.clear()
        self.detail.clear()
        self.report_button.setEnabled(False)
        self.save_button.setEnabled(False)
        self.load_button.setEnabled(False)
        self.compare_button.setEnabled(bool(self.left_file.path and self.right_file.path))
        self.summary.setText("Файлы выбраны · запустите сверку")
        self.warning.hide()
        self.left_view.clear()
        self.right_view.clear()
        self.review_edit.setEnabled(False)
        self.priority_edit.setEnabled(False)

    def start_compare(self):
        if not self.confirm_discard():
            return
        self.compare_button.setEnabled(False)
        self.left_file.button.setEnabled(False)
        self.right_file.button.setEnabled(False)
        self.summary.setText("Разбор структуры и сверка…")
        self.worker = Worker(self.left_file.path, self.right_file.path)
        self.worker.done.connect(self.set_result)
        self.worker.failed.connect(self.compare_failed)
        self.worker.finished.connect(self.worker_finished)
        self.worker.start()

    def worker_finished(self):
        self.compare_button.setEnabled(True)
        self.left_file.button.setEnabled(True)
        self.right_file.button.setEnabled(True)

    def compare_failed(self, message):
        self.summary.setText("Не удалось выполнить сверку")
        QMessageBox.critical(self, "Ошибка сверки", message)

    def set_result(self, result):
        self.result = result
        self.dirty = False
        self.left_file.set_document(result.left)
        self.right_file.set_document(result.right)
        self.warning.setText("\n".join(result.warnings))
        self.warning.setVisible(bool(result.warnings))
        for b in (self.report_button, self.save_button, self.load_button):
            b.setEnabled(True)
        self.refilter()

    def enabled_categories(self):
        return {k for k, check in self.categories.items() if check.isChecked()}

    def selected_change(self):
        item = self.change_list.currentItem()
        return item.data(0, Qt.ItemDataRole.UserRole) if item else None

    def refilter(self, *args):
        if not self.result:
            return
        selected = self.selected_change()
        selected_id = selected.id if selected else ""
        cats = self.enabled_categories()
        search = self.search.text().casefold().strip()
        self.visible_changes = [c for c in self.result.changes if c.category in cats
            and (not self.priority_filter.currentData() or c.priority == self.priority_filter.currentData())
            and (not self.review_filter.currentData() or c.review == self.review_filter.currentData())
            and (not search or search in (c.before + c.after + c.context + c.reason).casefold())]
        self.change_list.blockSignals(True)
        self.change_list.clear()
        current = None
        for i, c in enumerate(self.visible_changes, 1):
            prefix = "✓ " if c.review != "new" else ""
            item = QTreeWidgetItem([f"{prefix}{i}. {CATEGORIES[c.category]}", PRIORITIES[c.priority]])
            item.setData(0, Qt.ItemDataRole.UserRole, c)
            item.setToolTip(0, c.context + "\n" + (c.before or c.after)[:500])
            item.setForeground(1, QColor({"high": "#b84235", "medium": "#937016", "low": "#477a9e"}[c.priority]))
            self.change_list.addTopLevelItem(item)
            if c.id == selected_id:
                current = item
        self.change_list.blockSignals(False)
        high = sum(c.priority == "high" for c in self.visible_changes)
        self.summary.setText(f"Показано {len(self.visible_changes)} из {len(self.result.changes)} · высокий: {high}")
        self.render_views()
        if current:
            self.change_list.setCurrentItem(current)
        elif self.visible_changes:
            self.change_list.setCurrentItem(self.change_list.topLevelItem(0))
        else:
            self.detail.setHtml("<p>Различий по выбранным фильтрам нет.</p>")
            self.review_edit.setEnabled(False)
            self.priority_edit.setEnabled(False)

    def render_views(self):
        if not self.result:
            return
        self.syncing = True
        try:
            for side, view in [("left", self.left_view), ("right", self.right_view)]:
                selected = self.selected_change()
                view.setHtml(render_document(self.result, side, self.enabled_categories(), selected=selected.id if selected else "", change_ids={c.id for c in self.visible_changes}))
        finally:
            self.syncing = False

    def select_change(self, item=None, previous=None):
        c = self.selected_change()
        if not c:
            return
        self.updating = True
        self.review_edit.setEnabled(True)
        self.priority_edit.setEnabled(True)
        self.review_edit.setCurrentIndex(self.review_edit.findData(c.review))
        self.priority_edit.setCurrentIndex(self.priority_edit.findData(c.priority))
        self.updating = False
        self.detail.setHtml(f'<html><style>{CSS}</style><body><b>{e(CATEGORIES[c.category])}</b><p>{e(c.reason)}</p>'
                            f'<p class="context">{e(c.context)}</p></body></html>')
        self.render_views()
        self.syncing = True
        try:
            if c.left_id:
                self.left_view.scrollToAnchor(c.left_id)
            if c.right_id:
                self.right_view.scrollToAnchor(c.right_id)
        finally:
            self.syncing = False

    def navigate(self, delta):
        count = self.change_list.topLevelItemCount()
        if count:
            current = self.change_list.indexOfTopLevelItem(self.change_list.currentItem())
            self.change_list.setCurrentItem(self.change_list.topLevelItem((current + delta) % count))

    def update_review(self):
        if self.updating:
            return
        c = self.selected_change()
        if c:
            c.review, c.priority = self.review_edit.currentData(), self.priority_edit.currentData()
            self.dirty = True
            self.refilter()

    def scroll_sync(self, source, target, value):
        if self.syncing or not self.sync.isChecked():
            return
        self.syncing = True
        bar, other = source.verticalScrollBar(), target.verticalScrollBar()
        other.setValue(round(value / max(1, bar.maximum()) * other.maximum()))
        self.syncing = False

    def export(self):
        if not self.result:
            return
        path, _ = QFileDialog.getSaveFileName(self, "Сохранить полный отчёт", "Отчёт сверки.html", "HTML (*.html)")
        if path:
            try:
                export_report(self.result, path)
                self.statusBar().showMessage(f"Полный отчёт сохранён: {path}")
            except OSError as error:
                QMessageBox.critical(self, "Ошибка сохранения", str(error))

    def save(self):
        if not self.result:
            return
        path, _ = QFileDialog.getSaveFileName(self, "Сохранить решения проверяющего", "Проверка.json", "JSON (*.json)")
        if path:
            try:
                save_session(self.result, path)
                self.dirty = False
                self.statusBar().showMessage(f"Решения сохранены: {path}")
            except OSError as error:
                QMessageBox.critical(self, "Ошибка сохранения", str(error))

    def load(self):
        if not self.result:
            return
        path, _ = QFileDialog.getOpenFileName(self, "Открыть решения для этой пары файлов", "", "JSON (*.json)")
        if path:
            try:
                load_session(self.result, path)
                self.dirty = False
                self.refilter()
            except (OSError, ValueError, TypeError) as error:
                QMessageBox.critical(self, "Ошибка открытия проверки", str(error))

    def closeEvent(self, event):
        if self.worker and self.worker.isRunning():
            QMessageBox.information(self, "Сверка выполняется", "Дождитесь завершения сверки перед закрытием.")
            event.ignore()
        elif not self.confirm_discard():
            event.ignore()
        else:
            event.accept()


def main():
    args_parser = argparse.ArgumentParser(description="Сверка HTML")
    args_parser.add_argument("--left")
    args_parser.add_argument("--right")
    args_parser.add_argument("--smoke-test", action="store_true", help="Проверить запуск интерфейса и выйти")
    args_parser.add_argument("--screenshot", help="Сохранить снимок интерфейса при smoke-test")
    args = args_parser.parse_args()
    app = QApplication(sys.argv[:1])
    app.setApplicationName("Сверка HTML")
    app.setApplicationVersion(__version__)
    app.setStyle("Fusion")
    app.setStyleSheet(STYLE)
    window = MainWindow()
    if args.left:
        window.left_file.set_path(args.left)
    if args.right:
        window.right_file.set_path(args.right)
    window.compare_button.setEnabled(bool(args.left and args.right))
    window.show()
    if args.smoke_test:
        if args.left and args.right:
            window.set_result(compare(parse_file(args.left), parse_file(args.right)))
        def finish():
            if args.screenshot:
                if not window.grab().save(args.screenshot):
                    raise RuntimeError("Не удалось сохранить снимок")
            app.quit()
        QTimer.singleShot(250, finish)
    elif args.left and args.right:
        QTimer.singleShot(0, window.start_compare)
    return app.exec()
