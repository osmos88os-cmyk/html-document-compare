"""Optional local integration check. Input files are never copied into the repo."""
import argparse
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from htmlcompare.compare import compare
from htmlcompare.parser import parse_file

parser = argparse.ArgumentParser()
parser.add_argument('--tech', type=Path, required=True)
parser.add_argument('--gpu', type=Path, required=True)
parser.add_argument('--kplus', type=Path, required=True)
args = parser.parse_args()
results = []
for right_path in sorted(args.gpu.glob('*.html')):
    left_path = args.tech / right_path.name
    left = parse_file(left_path)
    comparisons = [('ГПУ', right_path)]
    matches = list(args.kplus.glob(f'*N {left.act_number} *.html'))
    if len(matches) == 1:
        comparisons.append(('К+', matches[0]))
    for provider, right_path in comparisons:
        result = compare(left, parse_file(right_path))
        counts = Counter(c.category for c in result.changes)
        content = sum(counts[c] for c in ('words', 'numbers', 'structure', 'tables', 'punctuation'))
        results.append({'act': left.act_number, 'provider': provider, 'left_blocks': len(left.blocks),
                        'right_blocks': len(result.right.blocks), 'categories': dict(counts),
                        'content_changes': content, 'warnings': result.warnings})
print(json.dumps(results, ensure_ascii=False, indent=2))
if any(x['content_changes'] for x in results):
    raise SystemExit('Unexpected content differences in the supplied baseline pairs')
