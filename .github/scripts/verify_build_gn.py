#!/usr/bin/env python3
"""Verify the BUILD.gn file in net/kiwi/ has correct structure."""
import os
import re
import sys

src = os.environ.get('CHROMIUM_SRC', '')
build_gn = os.path.join(src, 'net/kiwi/BUILD.gn')
if not os.path.exists(build_gn):
    print(f'FAIL: BUILD.gn not found at {build_gn}')
    sys.exit(1)

content = open(build_gn).read()

checks = {
    'source_set("kiwi_net_extensions")': 'source_set("kiwi_net_extensions"' in content,
    'has sources list': 'sources' in content,
    'has deps list': 'deps' in content,
    'has public headers': 'public' in content,
    'has configs': 'configs' in content,
}

all_pass = True
for name, passed in checks.items():
    status = 'PASS' if passed else 'FAIL'
    if not passed:
        all_pass = False
    print(f'  [{status}] {name}')

sources = re.findall(r'"([^"]+\.cc)"', content)
headers = re.findall(r'"([^"]+\.h)"', content)
print(f'  [INFO] .cc references: {len(sources)}')
print(f'  [INFO] .h references: {len(headers)}')

if all_pass:
    print('\nBUILD.gn verification PASSED')
    sys.exit(0)
else:
    print('\nBUILD.gn verification FAILED')
    sys.exit(1)
