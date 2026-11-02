#!/usr/bin/env python3
"""版本查询不访问实例；不支持的运行库在命令执行前拒绝。"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

anof, old_sqlite, bad_curl, version = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='anof-dependencies-') as temporary:
    root = Path(temporary) / 'instance'
    root.mkdir(mode=0o700)
    for args in [ ['--version'], ['serve', '--version'],
                  ['serve', '--root', str(root / 'missing'), '--version'] ]:
        result = subprocess.run([anof, *args], cwd=temporary, capture_output=True, timeout=10)
        assert result.returncode == 0, (args, result.returncode, result.stderr)
        assert result.stdout == f'ANOF {version}\n'.encode(), result.stdout
        assert result.stderr == b'', result.stderr
        assert list(root.iterdir()) == [], 'version query changed instance'
        assert list(Path(temporary).iterdir()) == [root], 'version query created local state'
    read_end, write_end = os.pipe()
    os.close(read_end)
    try:
        result = subprocess.run([anof, '--version'], stdout=write_end,
                                stderr=subprocess.PIPE, timeout=10)
        assert result.returncode == 5, result.returncode
        assert result.stderr == b'', result.stderr
    finally:
        os.close(write_end)
    for library, code in [(old_sqlite, 'UNSUPPORTED_SQLITE_RUNTIME'), (bad_curl, 'UNSUPPORTED_CURL_RUNTIME')]:
        environment = dict(os.environ, LD_PRELOAD=library)
        result = subprocess.run([anof, 'init', '--root', str(root)], env=environment,
                                capture_output=True, timeout=10)
        assert result.returncode == 4, result.returncode
        assert result.stdout == b''
        assert result.stderr.decode() == f'ANOF_ERROR stage=dependencies code={code}\n'
        assert list(root.iterdir()) == [], 'dependency rejection changed instance'
print('Version queries and runtime library rejection preserved the empty instance')
