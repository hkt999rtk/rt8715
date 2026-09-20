#!/usr/bin/env python3
"""Check actual fault-injection output, including negative recovery evidence."""
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text()
failed = {}
retried = set()
next_hw = set()
case = {}
comparisons = set()
for line in text.splitlines():
    if line.startswith('CASE '):
        case = dict(re.findall(r'(\w+)=(\w+)', line))
    if '[CHACHASWREC][RX]' in line:
        fields = dict(re.findall(r'(\w+)=(-?\w+)', line))
        expected = '0' if case.get('bad') == '1' else '1'
        assert fields['fresh_tag_ok'] == expected, line
        assert fields['selected'] == ('reject' if expected == '0' else 'fresh'), line
        assert fields['path'] in ('direct', 'staged'), line
        if case.get('fault') == '6' and expected == '1':
            damage = case['damage']
            if damage in ('1', '3'):
                assert fields['reuse_tag_ok'] == '0', line
            elif damage in ('2', '4'):
                assert fields['reuse_tag_ok'] == '1' and fields['plain_match'] == '0', line
            elif damage == '5':
                assert fields['reuse_ran'] == '0', line
            elif damage == '0':
                assert fields['reuse_tag_ok'] == fields['plain_match'] == '1', line
            comparisons.add((fields['path'], damage))
    match = re.search(r'\[CHACHAREC\]\[(TX|RX)\] (.*)', line)
    if not match:
        continue
    direction, body = match.groups()
    fields = dict(re.findall(r'(\w+)=(-?\w+)', body))
    key = (direction, fields['id'])
    event = fields['event']
    if event == 'hw_fail':
        assert fields['dma_quiesced'] == fields['input_preserved'] == '1', line
        assert case.get('fault') not in ('0', '5'), line
        assert key not in failed, line
        failed[key] = fields
    elif event == 'sw_retry_done':
        assert key in failed and key not in retried, line
        assert fields['sw_retry'] == '1', line
        expected = '0' if case.get('bad') == '1' else '1'
        assert fields['recovered'] == expected, line
        assert fields['tag_ok' if direction == 'RX' else 'ciphertext_tag_ready'] == expected, line
        retried.add(key)
    elif event == 'next_hw_success':
        previous = (direction, fields['after_fail_id'])
        assert previous in failed and int(key[1]) > int(previous[1]), line
        assert case.get('fault') == '0' and case.get('bad') == '0', line
        assert fields['hw_complete'] == '1', line
        next_hw.add(direction)
assert failed.keys() == retried, (failed.keys() - retried)
assert {key[0] for key in failed} == next_hw == {'TX', 'RX'}
assert 'recovered=0 tag_ok=0' in text
assert 'engine_ready=0' in text
assert comparisons == {(p, str(d)) for p in ('direct', 'staged') for d in range(6)}
assert 'TX/RX recovery, partial DMA, bad tag, allocation, streaming, direct TX: PASS' in text
print(f'TX/RX recovery log evidence: PASS ({len(failed)} matched failures/retries)')
