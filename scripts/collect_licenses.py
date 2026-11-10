"""Copy dependency license texts into a distributable directory."""
import importlib.metadata
import shutil
import sys
from pathlib import Path

destination = Path(sys.argv[1] if len(sys.argv) > 1 else "dist/HTMLCompare/licenses")
destination.mkdir(parents=True, exist_ok=True)
bundled = Path(__file__).resolve().parents[1] / 'licenses'
if bundled.exists():
    shutil.copytree(bundled, destination / 'bundled', dirs_exist_ok=True)
for package in ("PySide6-Essentials", "shiboken6", "lxml", "pyinstaller"):
    dist = importlib.metadata.distribution(package)
    for file in dist.files or []:
        if any(part.lower().startswith(("license", "copying", "copyright")) for part in file.parts):
            source = Path(dist.locate_file(file))
            if source.is_file():
                target = destination / package / str(file).replace("..", "_")
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
python_license = Path(sys.base_prefix) / 'LICENSE.txt'
if python_license.exists():
    shutil.copy2(python_license, destination / 'CPython-LICENSE.txt')
