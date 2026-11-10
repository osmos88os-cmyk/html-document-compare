import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

import unittest
from pathlib import Path
from PySide6.QtWidgets import QApplication

from htmlcompare.app import MainWindow, STYLE
from htmlcompare.compare import compare
from htmlcompare.parser import parse_file


class InterfaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])
        cls.app.setStyleSheet(STYLE)

    def setUp(self):
        self.window = MainWindow()
        base = Path(__file__).resolve().parents[1] / 'examples'
        self.result = compare(parse_file(base / 'left.html'), parse_file(base / 'right.html'))
        self.window.set_result(self.result)

    def tearDown(self):
        self.window.dirty = False
        self.window.close()

    def test_filter_does_not_delete_changes(self):
        count = len(self.result.changes)
        for check in self.window.categories.values():
            check.setChecked(False)
        self.assertEqual(self.window.change_list.topLevelItemCount(), 0)
        self.assertEqual(len(self.result.changes), count)
        self.window.categories['numbers'].setChecked(True)
        self.assertGreater(self.window.change_list.topLevelItemCount(), 0)

    def test_manual_review_and_priority(self):
        c = self.window.selected_change()
        self.window.review_edit.setCurrentIndex(self.window.review_edit.findData('confirmed'))
        self.assertEqual(c.review, 'confirmed')
        self.window.priority_edit.setCurrentIndex(self.window.priority_edit.findData('low'))
        self.assertEqual(c.priority, 'low')
        self.assertTrue(self.window.dirty)

    def test_navigation(self):
        before = self.window.selected_change().id
        self.window.navigate(1)
        self.assertNotEqual(self.window.selected_change().id, before)

    def test_search_and_priority_filter(self):
        self.window.search.setText('650')
        self.assertEqual(self.window.change_list.topLevelItemCount(), 1)
        self.window.priority_filter.setCurrentIndex(self.window.priority_filter.findData('low'))
        self.assertEqual(self.window.change_list.topLevelItemCount(), 0)

    def test_cell_highlight(self):
        # Qt serializes the requested changed-cell background into its rich-text DOM.
        self.assertIn('#ffe0dc', self.window.right_view.toHtml())
        self.assertIn('250', self.window.right_view.toPlainText())


if __name__ == '__main__':
    unittest.main()
