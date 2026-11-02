#!/usr/bin/env python3
"""安装到私有前缀，演练实际部署示例及停机备份/新目录恢复；不启用主机服务。"""
import argparse
import ctypes
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import signal
import socket
import ssl
import subprocess
import tempfile
import time
import uuid
from tls_proxy import initialize, ready, stop, require, PASSWORD


def command(args, expected=0):
    result = subprocess.run([str(x) for x in args], capture_output=True, timeout=30)
    require(result.returncode == expected, f'command {args[0]} {args[1]} exited {result.returncode}, expected {expected}')
    require(len(result.stdout) + len(result.stderr) < 1024 * 1024, 'command output bound')
    return result


def unused_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def run(args):
    os.umask(0o077)
    with tempfile.TemporaryDirectory(prefix='anof-install-') as directory:
        base = Path(directory)
        stage = base / 'runtime'
        command([args.cmake, '--install', args.build, '--prefix', stage, '--component', 'Runtime'])
        # 移动整个安装前缀，确认不依赖安装时路径或构建机SQLite绝对位置。
        prefix = base / 'relocated'
        stage.rename(prefix)
        executable = prefix / args.bindir / 'anof'
        data = prefix / args.datadir / 'anof'
        deploy = data / 'deploy'
        require((data / 'CHANGELOG.md').is_file(), 'changelog installation absent')
        require((data / 'SECURITY.md').is_file(), 'security policy installation absent')
        for arguments in [['--version'], ['serve', '--version']]:
            result = command([executable, *arguments])
            require(result.stdout == f'ANOF {args.anof_version}\n'.encode() and result.stderr == b'',
                    'installed version output mismatch')
        require((data / 'web/index.html').is_file(), 'Web installation absent')
        require((data / 'web/THIRD_PARTY_NOTICES.txt').read_bytes() == Path(args.web, 'THIRD_PARTY_NOTICES.txt').read_bytes(), 'NOTICE differs')
        needed = command(['readelf', '-d', executable]).stdout.decode()
        require(not re.search(r'NEEDED.*\[/[^]]*sqlite', needed), 'absolute SQLite DT_NEEDED')
        linked = command(['ldd', executable]).stdout.decode()
        require('not found' not in linked, 'missing dynamic dependency')
        sqlite_line = next(line for line in linked.splitlines() if 'sqlite' in line)
        require(str(prefix) in sqlite_line, 'SQLite did not resolve installed private library')
        library = next((prefix / args.libdir / 'anof').glob('libsqlite3.so*'))
        sqlite = ctypes.CDLL(str(library)); sqlite.sqlite3_libversion.restype = ctypes.c_char_p
        require(sqlite.sqlite3_libversion().decode() == args.sqlite_version, 'SQLite runtime version mismatch')
        root = base / 'instance'; root.mkdir(mode=0o700)
        initialize(str(executable), root)
        config = root / '.anof/config.toml'
        tls_port = unused_port()
        origin = f'https://localhost:{tls_port}'
        config_text = (deploy / 'config.toml').read_text().replace('https://cloud.example.test', origin)
        config_text = re.sub(r'^port = .*$', 'port = 0', config_text, flags=re.M)
        config_text = re.sub(r'^disk_safety_min_bytes = .*$', 'disk_safety_min_bytes = 0', config_text, flags=re.M)
        config.write_text(config_text)
        command([executable, '--json', 'config', 'validate', '--root', root])
        # 占用端口拒绝后实例锁必须可重新取得。
        with socket.socket() as blocker:
            blocker.bind(('127.0.0.1', 0)); blocker.listen()
            config.write_text(config_text.replace('port = 0', f'port = {blocker.getsockname()[1]}'))
            command([executable, 'serve', '--root', root], 4)
        config.write_text(config_text)
        command([executable, '--json', 'config', 'validate', '--root', root])
        cert, key = base / 'cert.pem', base / 'key.pem'
        command([args.openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=localhost',
                 '-addext', 'subjectAltName=DNS:localhost', '-keyout', key, '-out', cert])
        context = ssl.create_default_context(cafile=str(cert))
        server = proxy = None
        secrets = [PASSWORD]
        logs = b''
        try:
            server = subprocess.Popen([str(executable), 'serve', '--root', str(root)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            backend, startup = ready(server); logs += startup
            command([executable, '--json', 'config', 'validate', '--root', root], 3)
            def proxy_start(backend):
                # 修改部署示例的环境值；代理/安全策略本身不另写一套。
                template = (deploy / 'nginx/anof.conf').read_text()
                for old, new in [('listen 443 ssl;', f'listen 127.0.0.1:{tls_port} ssl;'),
                                 ('cloud.example.test', f'localhost:{tls_port}'),
                                 ('/etc/anof/tls/fullchain.pem', str(cert)), ('/etc/anof/tls/privkey.pem', str(key)),
                                 ('/usr/local/share/anof/web', str(data / 'web')),
                                 ('/var/log/anof/nginx/access.log', str(base / 'access.log')),
                                 ('127.0.0.1:8080', f'127.0.0.1:{backend}')]:
                    template = template.replace(old, new)
                template = template.replace(f'server_name localhost:{tls_port};', 'server_name localhost;')
                paths = '\n'.join(f'{n}_temp_path {base}/{n};' for n in ['client_body', 'proxy', 'fastcgi', 'uwsgi', 'scgi'])
                text = f'daemon off; master_process off; pid {base}/nginx.pid; error_log {base}/startup.log crit; events {{ worker_connections 32; }} http {{ {paths}\n{template}\n}}'
                conf = base / 'nginx.conf'; conf.write_text(text)
                command([args.nginx, '-t', '-e', str(base/'startup.log'), '-p', str(base)+'/', '-c', conf])
                proc = subprocess.Popen([args.nginx, '-e', str(base/'startup.log'), '-p', str(base)+'/', '-c', str(conf)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic()+5
                    while True:
                        require(proc.poll() is None, 'proxy stopped')
                        try:
                            with socket.create_connection(('127.0.0.1',tls_port),timeout=.2): break
                        except OSError:
                            require(time.monotonic()<deadline, 'proxy startup deadline'); time.sleep(.02)
                    return proc
                except BaseException:
                    stop(proc, check=False)
                    raise
            proxy = proxy_start(backend)
            def call(method,path,status,body=None,headers=None):
                fields = dict(headers or {})
                if isinstance(body,dict): body=json.dumps(body).encode(); fields['Content-Type']='application/json'
                conn=http.client.HTTPSConnection('localhost',tls_port,context=context,timeout=5)
                try:
                    conn.request(method,path,body=body,headers=fields); response=conn.getresponse(); content=response.read(524289)
                    require(len(content)<=524288,'HTTP response bound')
                    require(response.status==status,f'{method} unexpected status {response.status} expected {status}')
                    h={k.lower():v for k,v in response.getheaders()}
                    require(h.get('strict-transport-security')=='max-age=31536000','HSTS absent')
                    return h,content
                finally: conn.close()
            h,html=call('GET','/',200)
            require('text/html' in h.get('content-type','') and h['cache-control']=='no-cache','HTML headers')
            require("'wasm-unsafe-eval'" in h['content-security-policy'] and "'unsafe-eval'" not in h['content-security-policy'],'Worker CSP')
            asset=re.search(rb'src="([^"]+\.js)"',html).group(1).decode()
            h,_=call('GET',asset,200); require('immutable' in h['cache-control'] and 'javascript' in h['content-type'],'asset headers')
            worker=next((data/'web/assets').glob('sha256-*.js')); call('GET','/assets/'+worker.name,200)
            call('GET','/THIRD_PARTY_NOTICES.txt',200)
            for path in ['/objects/x','/.anof/config.toml','/AGENTS.md','/unknown']:
                call('GET',path,404)
            call('GET','/api/unknown',404); call('GET','/health/ready',200)
            call('GET','/',421,headers={'Host':'other.invalid'})
            same={'Origin':origin,'X-ANOF-Request':'1'}
            def login():
                h,b=call('POST','/api/v1/auth/sessions',201,{'login':'administrator','password':PASSWORD,'credential_mode':'cookie'},same)
                require('access_token' not in json.loads(b),'Cookie response token')
                cookie=h['set-cookie'].split(';',1)[0]; secrets.append(cookie); return cookie
            cookie=login(); read={'Cookie':cookie}; write={**read,**same}
            _,b=call('GET','/api/v1/users/me',200,headers=read); instance=json.loads(b)['instance_id']
            call('GET','/api/v1/users/me',200,headers={**read,'Forwarded':'for=bad','X-Real-IP':'bad','X-Forwarded-For':'bad','X-Forwarded-Proto':'http'})
            call('DELETE','/api/v1/auth/session',403,headers={**write,'Origin':'https://evil.invalid'})
            _,b=call('GET','/api/v1/spaces',200,headers=read); space=next(s for s in json.loads(b)['items'] if s['kind']=='private')
            payload=b'installed ANOF backup and restore'
            _,b=call('POST','/api/v1/uploads',201,{'client_upload_id':str(uuid.uuid4()),'space_id':space['id'],'parent_id':space['root_node_id'],'name':'installed.txt','length':str(len(payload)),'sha256':hashlib.sha256(payload).hexdigest()},write)
            upload='/api/v1/uploads/'+json.loads(b)['id']
            h,_=call('PATCH',upload,204,payload,{**write,'Upload-Offset':'0','Content-Type':'application/offset+octet-stream'})
            require(h['upload-offset']==str(len(payload)),'offset lost')
            call('POST',upload+'/complete',202,{},write)
            deadline=time.monotonic()+5
            while True:
                _,b=call('GET',upload,200,headers=read); u=json.loads(b)
                if u['state']=='COMPLETED': break
                require(time.monotonic()<deadline,'publication deadline');time.sleep(.02)
            content='/api/v1/nodes/'+u['result_node_id']+'/content'
            h,b=call('GET',content,200,headers=read);require(b==payload and h['cache-control']=='no-store','content corrupted/cacheable')
            require(h['content-disposition'].startswith('attachment'),'attachment missing')
            call('HEAD',content,200,headers=read);_,b=call('GET',content,206,headers={**read,'Range':'bytes=1-3','If-Range':h['etag']});require(b==payload[1:4],'range changed')
            call('GET',content,304,headers={**read,'If-None-Match':h['etag']})
            call('GET','/api/unknown?secret_query_marker',404)
            logs+=stop(proxy,signal.SIGQUIT);proxy=None
            logs+=stop(server);server=None
            scratch=base/'scratch';scratch.mkdir(mode=0o700)
            command([executable,'--json','fsck','--root',root,'--full','--work-dir',scratch])
            backup=base/'backup'; restored=base/'restored'
            command([executable,'--json','backup','--root',root,'--output',backup,'--work-dir',scratch])
            command([executable,'--json','restore','--root',restored,'--backup',backup,'--config',config,'--work-dir',scratch])
            command([executable,'--json','restore','--root',restored,'--backup',backup,'--config',config,'--work-dir',scratch],3)
            command([executable,'--json','fsck','--root',restored,'--full','--work-dir',scratch])
            server=subprocess.Popen([str(executable),'serve','--root',str(restored)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
            backend,startup=ready(server);logs+=startup;proxy=proxy_start(backend)
            call('GET','/api/v1/users/me',401,headers=read)
            read={'Cookie':login()};_,b=call('GET','/api/v1/users/me',200,headers=read);require(json.loads(b)['instance_id']==instance,'restore changed instance identity')
            _,b=call('GET',content,200,headers=read);require(b==payload,'restored download differs')
            logs+=stop(proxy,signal.SIGQUIT);proxy=None;logs+=stop(server);server=None
            logs+=(base/'access.log').read_bytes()+(base/'startup.log').read_bytes()
            for secret in secrets+['secret_query_marker']:
                require(secret.encode() not in logs,'sensitive data in logs')
            command([executable,'--json','config','validate','--root',restored])
        finally:
            for proc in [proxy,server]:
                if proc is not None: stop(proc,check=False)
    print('Installed relocation, deployment template, TLS/content, lock and offline restore acceptance passed')


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    for name in ['cmake','build','web','nginx','openssl','bindir','libdir','datadir','sqlite-version','anof-version']:
        parser.add_argument('--'+name,required=True)
    run(parser.parse_args())
