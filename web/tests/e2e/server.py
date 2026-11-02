#!/usr/bin/env python3
"""私有 ANOF/nginx HTTPS 生命周期。控制数据走管道，不使用已有实例。"""
import argparse
from datetime import datetime, timedelta, timezone
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import termios
import threading
import time

PASSWORD = 'fixture-admin-password-only'


def require(ok, label):
    if not ok:
        raise RuntimeError(label)


def run_command(args, **kwargs):
    result = subprocess.run(args, capture_output=True, timeout=10, **kwargs)
    if result.returncode != 0:
        reason = re.search(rb'error [0-9]+ at [0-9]+ depth lookup: [a-z ]+', result.stderr)
        raise RuntimeError('fixture command failed: ' + Path(args[0]).name + (' ' + reason[0].decode() if reason else ''))
    return result.stdout


def initialize(executable, root):
    master, slave = pty.openpty()
    def terminal():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)
    args = [executable, 'init', '--root', str(root), '--admin-login', 'administrator',
            '--admin-display-name', 'Administrator', '--development-http',
            '--content-budget-bytes', '10485760', '--disk-safety-min-bytes', '0', '--disk-safety-percent', '0']
    for scope in ('private', 'public', 'contribution'):
        args += [f'--{scope}-bytes', '1048576', f'--{scope}-nodes', '100']
    proc = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave, preexec_fn=terminal)
    os.close(slave)
    captured = b''
    try:
        for prompt in (b'Administrator password: ', b'Confirm password: '):
            deadline = time.monotonic() + 10
            while prompt not in captured:
                require(time.monotonic() < deadline and proc.poll() is None, 'initialization deadline')
                if select.select([master], [], [], .05)[0]:
                    captured += os.read(master, 4096)
                    require(len(captured) < 16384, 'initialization output limit')
            os.write(master, (PASSWORD + '\n').encode())
        require(proc.wait(timeout=10) == 0, 'initialization failure')
        require(PASSWORD.encode() not in captured, 'terminal secret echo')
    finally:
        if proc.poll() is None:
            proc.kill(); proc.wait(timeout=5)
        os.close(master)


def port():
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        return reservation.getsockname()[1]


class Child:
    def __init__(self, args):
        self.log = bytearray()
        self.overflow = False
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        def drain():
            while chunk := os.read(self.proc.stdout.fileno(), 4096):
                if len(self.log) + len(chunk) <= 1048576:
                    self.log.extend(chunk)
                else:
                    self.overflow = True
        self.thread = threading.Thread(target=drain)
        self.thread.start()
    def stop(self, sig=signal.SIGTERM):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        try:
            self.proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait(timeout=5)
            raise RuntimeError('child shutdown deadline') from None
        finally:
            self.thread.join(timeout=5)
            self.proc.stdout.close()
        require(not self.thread.is_alive() and not self.overflow, 'child log limit or drain failure')


def serve(args):
    os.umask(0o077)
    # 后端只使用自身构建时链接配置，不继承浏览器私有动态库设置。
    os.environ.pop('LD_LIBRARY_PATH', None)
    os.environ.pop('ALSA_CONFIG_PATH', None)
    with tempfile.TemporaryDirectory(prefix='anof-web-e2e-') as directory:
        base = Path(directory)
        root = base / 'instance'; root.mkdir()
        initialize(args.anof, root)
        public_port, other_port = port(), port()
        origin = f'https://localhost:{public_port}'
        config = root / '.anof/config.toml'
        value = config.read_text()
        for name, replacement in {'port': '0', 'development_http': 'false', 'web_enabled': 'true',
                'public_origin': json.dumps(origin), 'trusted_proxies': '["127.0.0.1"]'}.items():
            value, count = re.subn(rf'^{name}\s*=.*$', f'{name} = {replacement}', value, flags=re.MULTILINE)
            require(count == 1, 'fixture configuration key missing')
        config.write_text(value)
        ca, ca_key, cert, key = [base / name for name in ('ca.pem', 'ca.key', 'server.pem', 'server.key')]
        run_command([args.openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
            '-subj', '/CN=ANOF isolated browser test CA', '-addext', 'basicConstraints=critical,CA:TRUE',
            '-addext', 'keyUsage=critical,keyCertSign,cRLSign', '-keyout', str(ca_key), '-out', str(ca)])
        # WSL 的墙钟可能小幅回拨；证书有效期显式覆盖前一天，不跳过任何 TLS 校验。
        now = datetime.now(timezone.utc)
        start = (now - timedelta(days=1)).strftime('%Y%m%d%H%M%SZ')
        end = (now + timedelta(days=1)).strftime('%Y%m%d%H%M%SZ')
        (base / 'index').write_text('')
        (base / 'serial').write_text('1000\n')
        authority = base / 'ca.conf'
        authority.write_text(f'''[ca]
default_ca = local
[local]
database = {base}/index
serial = {base}/serial
new_certs_dir = {base}
certificate = {ca}
private_key = {ca_key}
default_md = sha256
policy = names
unique_subject = no
[names]
commonName = supplied
[authority]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
[server]
subjectAltName = DNS:localhost,IP:127.0.0.1
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
''')
        ca_request = base / 'ca.csr'
        run_command([args.openssl, 'req', '-new', '-key', str(ca_key),
            '-subj', '/CN=ANOF isolated browser test CA', '-out', str(ca_request)])
        dated_ca = base / 'dated-ca.pem'
        signing = [args.openssl, 'ca', '-batch', '-notext', '-config', str(authority),
            '-startdate', start, '-enddate', end]
        run_command(signing + ['-selfsign', '-extensions', 'authority', '-in', str(ca_request), '-out', str(dated_ca)])
        dated_ca.replace(ca)
        request = base / 'server.csr'
        run_command([args.openssl, 'req', '-new', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
            '-keyout', str(key), '-out', str(request)])
        run_command(signing + ['-extensions', 'server', '-in', str(request), '-out', str(cert)])
        run_command([args.openssl, 'verify', '-CAfile', str(ca), str(cert)])
        backend = proxy = None
        secrets = [PASSWORD]
        try:
            backend = Child([args.anof, 'serve', '--root', str(root)])
            deadline = time.monotonic() + 10
            backend_port = None
            # peek 后继续由专属读线程有界排空，不让 stdout 管道阻塞业务。
            while backend_port is None:
                require(time.monotonic() < deadline and backend.proc.poll() is None, 'backend readiness deadline')
                for line in bytes(backend.log).splitlines():
                    try:
                        record = json.loads(line)
                        if record.get('event') == 'ANOF_READY': backend_port = int(record['port'])
                    except (ValueError, TypeError):
                        pass
                time.sleep(.02)
            headers = ['Authorization', 'Cookie', 'Origin', 'X-ANOF-Request', 'Content-Type', 'Content-Length',
                       'If-Match', 'If-None-Match', 'Range', 'If-Range', 'Upload-Offset']
            forwarded = '\n'.join(f'proxy_set_header {h} $http_{h.lower().replace("-", "_")};' for h in headers)
            upstream = f'''proxy_pass http://127.0.0.1:{backend_port};
                proxy_http_version 1.1; proxy_pass_request_headers off;
                proxy_request_buffering off; proxy_buffering off; proxy_cache off; proxy_next_upstream off;
                proxy_connect_timeout 3s; proxy_read_timeout 45s; proxy_send_timeout 45s;
                proxy_set_header Host localhost:{public_port}; proxy_set_header Connection close;
                proxy_set_header X-Forwarded-For $remote_addr; proxy_set_header X-Forwarded-Proto https;
                {forwarded}'''
            csp = "default-src 'none'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self'; img-src 'self'; connect-src 'self'; worker-src 'self'; font-src 'self'; base-uri 'none'; object-src 'none'; frame-ancestors 'none'; form-action 'self'"
            conf = base / 'nginx.conf'
            conf.write_text(f'''daemon off; master_process off;
                pid {base}/nginx.pid; error_log {base}/nginx-error.log warn;
                events {{ worker_connections 64; }}
                http {{ access_log off;
                    client_body_temp_path {base}/body; proxy_temp_path {base}/proxy;
                    fastcgi_temp_path {base}/fastcgi; uwsgi_temp_path {base}/uwsgi; scgi_temp_path {base}/scgi;
                    types {{ text/html html; text/css css; application/javascript js; }}
                    server {{ listen 127.0.0.1:{public_port} ssl; server_name localhost;
                        ssl_certificate {cert}; ssl_certificate_key {key};
                        if ($host != localhost) {{ return 421; }}
                        location /api/ {{ client_max_body_size 8m; {upstream} }}
                        location /health/ {{ {upstream} }}
                        location = /api {{ return 404; }}
                        location = /health {{ return 404; }}
                        location / {{ root "{args.dist}"; index index.html; try_files $uri $uri/ =404;
                            add_header Content-Security-Policy "{csp}" always;
                            add_header Referrer-Policy no-referrer always;
                            add_header X-Content-Type-Options nosniff always;
                            add_header Cache-Control no-store always;
                        }}
                    }}
                    server {{ listen 127.0.0.1:{other_port} ssl; server_name localhost;
                        ssl_certificate {cert}; ssl_certificate_key {key};
                        location / {{ default_type text/html; return 200 '<!doctype html><title>Isolated other origin</title>'; }}
                    }}
                }}''')
            run_command([args.nginx, '-t', '-p', str(base) + '/', '-c', str(conf), '-e', str(base / 'nginx-error.log')])
            proxy = Child([args.nginx, '-p', str(base) + '/', '-c', str(conf), '-e', str(base / 'nginx-error.log')])
            deadline = time.monotonic() + 5
            tls = ssl.create_default_context(cafile=str(ca))
            while True:
                require(proxy.proc.poll() is None, 'proxy startup failure')
                try:
                    with socket.create_connection(('127.0.0.1', public_port), timeout=.5) as connection:
                        with tls.wrap_socket(connection, server_hostname='localhost'): pass
                    break
                except ssl.SSLCertVerificationError as error:
                    raise RuntimeError('TLS certificate verification: ' + error.verify_message) from None
                except OSError:
                    require(time.monotonic() < deadline, 'proxy readiness deadline'); time.sleep(.02)
            print(json.dumps({'base': str(base), 'origin': origin, 'otherOrigin': f'https://127.0.0.1:{other_port}', 'ca': str(ca)}), flush=True)
            require(select.select([sys.stdin], [], [], 180)[0], 'fixture lifetime deadline')
            control = sys.stdin.buffer.readline(65537)
            require(len(control) <= 65536, 'fixture control limit')
            message = json.loads(control)
            require(message.get('stop') is True, 'invalid fixture control')
            secrets.extend(message.get('secrets', []))
        finally:
            failures = []
            for child, sig in ((proxy, signal.SIGQUIT), (backend, signal.SIGTERM)):
                if child is not None:
                    try: child.stop(sig)
                    except Exception as error: failures.append(error)
            if failures: raise failures[0]
        logs = bytes(backend.log) + bytes(proxy.log)
        error_log = base / 'nginx-error.log'
        require(error_log.stat().st_size < 1048576, 'proxy log limit')
        logs += error_log.read_bytes()
        require(all(isinstance(secret, str) and secret.encode() not in logs for secret in secrets if secret), 'secret found in fixture logs')
        require(backend.proc.returncode == 0 and proxy.proc.returncode == 0, 'process exit failure')
        run_command([args.anof, '--json', 'config', 'validate', '--root', str(root)])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('anof', 'nginx', 'openssl', 'dist'): parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    def interrupted(_signal, _frame):
        raise RuntimeError('fixture interrupted')
    signal.signal(signal.SIGTERM, interrupted)
    try:
        serve(args)
    except Exception as error:
        print('Browser fixture failed: ' + (str(error) if isinstance(error, RuntimeError) else type(error).__name__), file=sys.stderr)
        sys.exit(1)
