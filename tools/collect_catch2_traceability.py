#!/usr/bin/env python3
"""
Collect CAP master-test-plan (TC-*) traceability evidence from the Catch2
unit-test binary.

Enumerates tests via `--list-tests --reporter xml` (no test bodies run, so
this can't introduce or hit test flakiness) and pairs the resulting TC-ID ->
test-case mapping with the pass/fail verdict of the most recent `make check`
run, read from automake's .trs result file. Catch2's gate is one binary
verdict for the whole suite, not per-test, so every nodeid under a given
report gets that same outcome.

Output format matches CAP tools/test/helpers/traceability.py's
write_traceability_json so per-run artifacts from every tier merge cleanly.
"""
import argparse
import datetime
import json
import pathlib
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict

TC_ID_RE = re.compile(r'^TC-[A-Za-z0-9]+-[A-Za-z0-9]+$')


def list_tagged_tests(binary):
    """
    Run the Catch2 binary's --list-tests in XML form and return
    {tc_id: [nodeid, ...]}, without executing any test body.
    """
    # sourcery skip: dangerous-subprocess-use-audit
    result = subprocess.run(
        [str(binary.resolve()), '--list-tests', '--reporter', 'xml'],
        cwd=binary.parent, capture_output=True, text=True, check=True)
    root = ET.fromstring(result.stdout)
    tagged = defaultdict(list)
    for case in root.findall('TestCase'):
        name = case.findtext('Name', default='')
        source = case.find('SourceInfo')
        file_name = source.findtext('File', default='') if source is not None else ''
        line = source.findtext('Line', default='') if source is not None else ''
        nodeid = f'{file_name}:{line}::{name}'
        tags_text = case.findtext('Tags', default='') or ''
        for tag in re.findall(r'\[([^\]]+)\]', tags_text):
            if TC_ID_RE.match(tag):
                tagged[tag].append(nodeid)
    return dict(sorted(tagged.items()))


def read_trs_outcome(trs_path):
    """
    Read automake's .trs result file and return 'passed', 'failed', or
    'not-run' if the file is missing (suite hasn't been run yet).
    """
    if not trs_path.is_file():
        return 'not-run'
    for line in trs_path.read_text(encoding='utf-8').splitlines():
        if line.startswith(':global-test-result:'):
            return 'passed' if line.split(':')[-1].strip() == 'PASS' else 'failed'
    return 'not-run'


def build_report(binary, trs_path):
    """
    Combine the tag mapping with the suite-wide outcome into the shared
    {test_id: [{"nodeid":..., "outcome":...}]} report shape.
    """
    outcome = read_trs_outcome(trs_path)
    return {
        tc_id: [{'nodeid': nodeid, 'outcome': outcome} for nodeid in nodeids]
        for tc_id, nodeids in list_tagged_tests(binary).items()
    }


def write_traceability_json(path, report):
    """
    Write the traceability report artifact in the shared interchange format.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        'generated': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'test_ids': report,
    }
    path.write_text(json.dumps(payload, indent=2) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--binary', type=pathlib.Path, default=pathlib.Path('tests/all_test'),
        help='path to the built Catch2 test binary (default: tests/all_test)')
    parser.add_argument(
        '--trs', type=pathlib.Path, default=None,
        help='path to the automake .trs result file (default: <binary>.trs)')
    parser.add_argument(
        '--output', type=pathlib.Path, default=pathlib.Path('tests/catch2-traceability.json'),
        help='where to write the traceability JSON artifact')
    args = parser.parse_args()

    if not args.binary.is_file():
        print(f'error: no test binary at {args.binary} (build it first)', file=sys.stderr)
        return 1

    trs_path = args.trs if args.trs is not None else args.binary.with_suffix('.trs')
    report = build_report(args.binary, trs_path)
    write_traceability_json(args.output, report)
    print(f'traceability report: {args.output} ({len(report)} TC IDs)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
