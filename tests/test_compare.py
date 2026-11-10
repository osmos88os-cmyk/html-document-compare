import tempfile
import unittest
from pathlib import Path

from htmlcompare.compare import compare
from htmlcompare.parser import parse_html
from htmlcompare.render import export_report, load_session, render_document, save_session


def doc(body, kp=False, name="УП 1 от 01.01.2020.html"):
    namespace = 'xmlns="http://www.w3.org/1999/xhtml"' if kp else ''
    if kp:
        body = '<div class="s2B">УКАЗ</div>' + body
    return parse_html(f'<html {namespace}><head><meta charset="utf-8"></head><body>{body}</body></html>'.encode(), name)


class ComparisonTests(unittest.TestCase):
    def test_identical(self):
        d = doc('<p>1. Выплатить 650 рублей.</p>')
        self.assertEqual(compare(d, d).changes, [])

    def test_numbers_are_high_priority(self):
        r = compare(doc('<p>Выплатить 650 рублей.</p>'), doc('<p>Выплатить 850 рублей.</p>'))
        self.assertTrue(any(c.category == 'numbers' and c.priority == 'high' for c in r.changes))

    def test_negation_is_high_priority(self):
        r = compare(doc('<p>Передача разрешается.</p>'), doc('<p>Передача не разрешается.</p>'))
        self.assertTrue(any(c.category == 'words' and c.priority == 'high' for c in r.changes))

    def test_preserve_ed_content(self):
        a = doc('<p>Выплатить <span class="ed">650 рублей</span>.</p>')
        b = doc('<p>Выплатить <span class="ed">850 рублей</span>.</p>')
        self.assertIn('650', a.blocks[0].text)
        self.assertTrue(any(c.category == 'numbers' for c in compare(a, b).changes))

    def test_fragmented_note_and_relocation(self):
        a = doc('<p>6. Выплатить.<span class="mark">(В редакции </span><span class="mark">Указа от 01.01.2020 № 2)</span></p>')
        b = doc('<div>6. Выплатить.</div><div>(в ред. Указа от 01.01.2020 N 2)</div>', kp=True)
        r = compare(a, b)
        self.assertEqual(len(a.blocks[0].notes), 1)
        self.assertTrue(any(c.category == 'note_position' for c in r.changes))
        self.assertFalse(any(c.category in {'words', 'numbers'} for c in r.changes))

    def test_repeal_markup_boundary(self):
        a = doc('<p>абзац; <span class="mark">(Утратил силу - Указ от 05.02.2024 № 94)</span></p>')
        b = doc('<p><span class="mark">абзац; (Утратил силу - Указ от 05.02.2024 № 94)</span></p>')
        self.assertEqual(a.blocks[0].text, b.blocks[0].text)
        self.assertFalse(any(c.category in {'structure', 'words'} for c in compare(a, b).changes))

    def test_repeal_status_is_never_lost(self):
        a = doc('<p>22) <span class="mark">(Утратил силу - Указ от 01.01.2020 № 2)</span></p>')
        b = doc('<p>22) Выплачивает пособие.</p>')
        self.assertIn('Утратил силу', a.blocks[0].text)
        self.assertTrue(any(c.category in {'words', 'structure'} for c in compare(a, b).changes))

    def test_different_repeal_dates_high(self):
        a = doc('<p>22) <span class="mark">(Утратил силу с 01.01.2020 - Указ от 01.01.2019 № 2)</span></p>')
        b = doc('<p>22) <span class="mark">(Утратил силу с 01.02.2020 - Указ от 01.01.2019 № 2)</span></p>')
        self.assertTrue(any(c.category == 'notes' and c.priority == 'high' for c in compare(a, b).changes))

    def test_note_reference_change_high(self):
        a = doc('<p>Текст.<span class="mark">(В редакции Указа от 01.01.2020 № 2)</span></p>')
        b = doc('<p>Текст.<span class="mark">(В редакции Указа от 01.01.2020 № 3)</span></p>')
        self.assertTrue(any(c.category == 'notes' and c.priority == 'high' for c in compare(a, b).changes))

    def test_superscript_enumeration(self):
        a = doc('<p>27<span class="W9">1</span>) Текст.</p>')
        b = doc('<p>27(1)) Текст.</p>')
        self.assertIn('27^1', a.blocks[0].text)
        self.assertFalse(any(c.category in {'numbers', 'words', 'structure'} for c in compare(a, b).changes))

    def test_different_superscript_is_detected(self):
        a = doc('<p>27<sup>1</sup>) Текст.</p>')
        b = doc('<p>27<sup>2</sup>) Текст.</p>')
        self.assertTrue(any(c.category == 'numbers' for c in compare(a, b).changes))

    def test_paragraph_insert_not_cascade(self):
        a = doc('<p>1. Первый пункт.</p><p>2. Второй пункт.</p>')
        b = doc('<p>1. Первый пункт.</p><p>1.1. Добавленный пункт.</p><p>2. Второй пункт.</p>')
        changes = compare(a, b).changes
        self.assertEqual(len(changes), 1)
        self.assertEqual(changes[0].category, 'structure')

    def test_repeated_numbers_across_sections(self):
        a = doc('<h2>I. Первый раздел</h2><p>1. Альфа</p><h2>II. Второй раздел</h2><p>1. Бета 10</p>')
        b = doc('<h2>I. Первый раздел</h2><p>1. Альфа</p><h2>II. Второй раздел</h2><p>1. Бета 20</p>')
        c = compare(a, b).changes[0]
        self.assertIn('II.', c.context)

    def test_layout_table_not_data(self):
        a = doc('<table><tr><td>21 декабря 2016 года</td><td>N 699</td></tr></table><p>Текст.</p>')
        self.assertEqual(len(a.blocks), 1)
        self.assertEqual(a.blocks[0].kind, 'paragraph')

    def test_one_row_data_table_preserved(self):
        a = doc('<table><tr><td>Тариф</td><td>650</td></tr></table>')
        self.assertEqual(a.blocks[0].kind, 'table')

    def test_inserted_table_row(self):
        a = doc('<table><tr><th>Название</th><th>Сумма</th></tr><tr><td>Альфа</td><td>10</td></tr><tr><td>Бета</td><td>20</td></tr></table>')
        b = doc('<table><tr><th>Название</th><th>Сумма</th></tr><tr><td>Альфа</td><td>10</td></tr><tr><td>Новая</td><td>30</td></tr><tr><td>Бета</td><td>20</td></tr></table>')
        c = compare(a, b).changes
        self.assertEqual(len(c), 1)
        self.assertEqual(c[0].category, 'tables')
        self.assertIn('Новая', c[0].after)

    def test_table_cell_change(self):
        a = doc('<table><tr><td>Альфа</td><td>650</td></tr></table>')
        b = doc('<table><tr><td>Альфа</td><td>850</td></tr></table>')
        c = compare(a, b).changes[0]
        self.assertEqual(c.category, 'tables')
        self.assertEqual(c.priority, 'high')
        self.assertIn('столбец 2', c.context)

    def test_rowspan_positions(self):
        a = doc('<table><tr><td rowspan="2">А</td><td>10</td></tr><tr><td>20</td></tr></table>')
        self.assertEqual([(c.row, c.col) for c in a.blocks[0].cells], [(0, 0), (0, 1), (1, 1)])

    def test_changed_merge(self):
        a = doc('<table><tr><td colspan="2">Текст</td></tr></table>')
        b = doc('<table><tr><td>Текст</td></tr></table>')
        self.assertTrue(any('объединение' in c.reason for c in compare(a, b).changes))

    def test_links_separate_from_words(self):
        a = doc('<p><a href="https://example.org/a">Документ</a></p>')
        b = doc('<p><a href="https://example.org/b">Документ</a></p>')
        self.assertEqual([c.category for c in compare(a, b).changes], ['links'])

    def test_script_is_ignored_and_escaped(self):
        a = doc('<p>Текст &lt;img src=x&gt;</p><script>alert(1)</script>')
        self.assertNotIn('alert', a.blocks[0].text)
        rendered = render_document(compare(a, a), 'left', set())
        self.assertNotIn('<img', rendered)

    def test_cp1251(self):
        a = parse_html('<html><meta charset="windows-1251"><body><p>Привет</p></body></html>'.encode('cp1251'))
        self.assertEqual(a.blocks[0].text, 'Привет')

    def test_empty_file_rejected(self):
        with self.assertRaises(ValueError):
            parse_html(b'')

    def test_session_hash_and_report(self):
        r = compare(doc('<p>Выплатить 650 рублей.</p>'), doc('<p>Выплатить 850 рублей.</p>'))
        with tempfile.TemporaryDirectory() as d:
            session = Path(d) / 'review.json'
            report = Path(d) / 'report.html'
            r.changes[0].review = 'confirmed'
            save_session(r, session)
            r.changes[0].review = 'new'
            load_session(r, session)
            self.assertEqual(r.changes[0].review, 'confirmed')
            export_report(r, report)
            self.assertIn('Подтверждено', report.read_text(encoding='utf-8'))
            r.right.sha256 = 'changed'
            with self.assertRaises(ValueError):
                load_session(r, session)


if __name__ == '__main__':
    unittest.main()
