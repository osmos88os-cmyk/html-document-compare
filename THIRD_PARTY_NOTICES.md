# Сторонние компоненты

Приложение использует динамически загружаемые сторонние библиотеки. Их лицензии не заменяются условиями на собственный код проекта.

- **PySide6 Essentials и Shiboken6 6.8.3** — LGPLv3 / GPLv3 / коммерческие условия Qt. В этой сборке используются QtCore, QtGui и QtWidgets из поставки Essentials. [Условия Qt for Python](https://doc.qt.io/qtforpython-6/licenses.html), [исходный код PySide](https://code.qt.io/cgit/pyside/pyside-setup.git/?h=6.8.3), [исходный код Qt](https://download.qt.io/archive/qt/6.8/6.8.3/).
- **lxml 5.4.0** — BSD. [Лицензии и исходники](https://github.com/lxml/lxml/tree/lxml-5.4.0).
- **libxml2 / libxslt** — лицензии поставляются с lxml. [libxml2](https://gitlab.gnome.org/GNOME/libxml2), [libxslt](https://gitlab.gnome.org/GNOME/libxslt).
- **CPython** — PSF License. [Лицензия](https://docs.python.org/3/license.html).
- **PyInstaller** — GPL с исключением для создаваемых пакетов приложений. [Условия](https://pyinstaller.org/en/stable/license.html).

Поставка в режиме `onedir` сохраняет Qt в отдельных динамических библиотеках внутри `_internal`. Исходники приложения доступны владельцу в репозитории. Для полного комплекта текстов лицензий сборка содержит каталог `licenses`.
