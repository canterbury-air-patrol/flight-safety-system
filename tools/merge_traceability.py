#!/usr/bin/env python3
"""
Merge per-suite traceability artifacts (Catch2 tests/catch2-traceability.json,
pytest e2e/traceability.json) into one CI-run artifact.

Each input has the shape {"generated": iso8601, "test_ids": {tc_id: [{"nodeid",
"outcome"}, ...]}}. A TC ID covered by more than one suite keeps every
suite's entries rather than deduping — per todo/27, the master-plan audit
distinguishes tiers by which report an ID appears in, so evidence from two
tiers should both surface, not collapse into one.
"""
import argparse
import datetime
import json
import pathlib
import sys
from collections import defaultdict


def merge_reports(paths):
    """
    Read each traceability JSON in `paths` and union their test_ids maps.
    """
    merged = defaultdict(list)
    for path in paths:
        payload = json.loads(path.read_text(encoding='utf-8'))
        for tc_id, entries in payload.get('test_ids', {}).items():
            merged[tc_id].extend(entries)
    return dict(sorted(merged.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        'inputs', nargs='+', type=pathlib.Path,
        help='traceability JSON artifacts to merge')
    parser.add_argument(
        '--output', type=pathlib.Path, default=pathlib.Path('traceability-report.json'),
        help='where to write the merged artifact')
    args = parser.parse_args()

    missing = [path for path in args.inputs if not path.is_file()]
    if missing:
        for path in missing:
            print(f'error: no such traceability artifact: {path}', file=sys.stderr)
        return 1

    merged = merge_reports(args.inputs)
    payload = {
        'generated': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'sources': [str(path) for path in args.inputs],
        'test_ids': merged,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + '\n', encoding='utf-8')
    print(f'merged traceability report: {args.output} ({len(merged)} TC IDs)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
