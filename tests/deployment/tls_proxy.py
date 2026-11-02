#!/usr/bin/env python3
"""有界真实 nginx/TLS + ANOF 进程验收；不安装服务，不修改系统信任库。"""
import argparse
import fcntl
import http.client
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
import time
import uuid

PASSWORD = "isolated TLS fixture password"


def require(ok, label):
    if not ok:
        raise RuntimeError(label)  # 不打印响应、密码或Cookie。


def initialize(executable, root):
    master, slave = pty.openpty()

    def terminal():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)

    args = [executable, "init", "--root", str(root), "--admin-login", "administrator",
            "--admin-display-name", "Administrator", "--development-http",
            "--content-budget-bytes", "1048576", "--disk-safety-min-bytes", "0",
            "--disk-safety-percent", "0"]
    for name in ("private", "public", "contribution"):
        args += [f"--{name}-bytes", "1048576", f"--{name}-nodes", "100"]
    proc = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave, preexec_fn=terminal)
    os.close(slave)
    captured = b""
    try:
        for prompt in (b"Administrator password: ", b"Confirm password: "):
            deadline = time.monotonic() + 10
            while prompt not in captured:
                require(time.monotonic() < deadline, "initialization terminal deadline")
                if select.select([master], [], [], 0.05)[0]:
                    captured += os.read(master, 4096)
                    require(len(captured) < 16384, "initialization output limit")
            os.write(master, (PASSWORD + "\n").encode())
        require(proc.wait(timeout=10) == 0, "initialization failed")
        require(PASSWORD.encode() not in captured, "terminal secret echoed")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        os.close(master)


def ready(proc):
    data = b""
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        require(proc.poll() is None, "ANOF stopped before ready")
        if not select.select([proc.stdout], [], [], 0.05)[0]:
            continue
        data += os.read(proc.stdout.fileno(), 4096)
        require(len(data) < 32768, "startup log bound")
        for line in data.splitlines():
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("event") == "ANOF_READY":
                return int(record["port"]), data
    raise RuntimeError("ANOF readiness deadline")


def stop(proc, sig=signal.SIGTERM, check=True):
    if proc.poll() is None:
        proc.send_signal(sig)
    try:
        out, _ = proc.communicate(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate(timeout=5)
        raise RuntimeError("process shutdown deadline") from None
    if check:
        require(proc.returncode == 0, "process shutdown failed")
    return out or b""


def run(args):
    os.umask(0o077)
    with tempfile.TemporaryDirectory(prefix="anof-tls-") as directory:
        base = Path(directory)
        root = base / "instance"
        root.mkdir(mode=0o700)
        initialize(args.anof, root)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        origin = f"https://localhost:{port}"
        config = root / ".anof/config.toml"
        text = config.read_text()
        for name, value in {"port": "0", "development_http": "false", "web_enabled": "true",
                            "public_origin": json.dumps(origin), "trusted_proxies": '["127.0.0.1"]'}.items():
            text, count = re.subn(rf"^{name}\s*=.*$", f"{name} = {value}", text, flags=re.MULTILINE)
            require(count == 1, "fixture configuration key missing")
        config.write_text(text)
        cert, key = base / "cert.pem", base / "key.pem"
        with (base / "openssl.log").open("wb") as log:
            subprocess.run([args.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                            "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                            "-keyout", str(key), "-out", str(cert)], stdout=log, stderr=log, check=True, timeout=10)
        context = ssl.create_default_context(cafile=str(cert))
        server = proxy = None
        with (base / "server.err").open("wb") as server_err, (base / "proxy.err").open("wb") as proxy_err:
            try:
                server = subprocess.Popen([args.anof, "serve", "--root", str(root)], stdout=subprocess.PIPE,
                                          stderr=server_err)
                backend, startup = ready(server)
                passed_headers = ["Authorization", "Cookie", "Origin", "X-ANOF-Request", "Content-Type",
                                  "Content-Length",
                                  "If-Match", "If-None-Match", "Range", "If-Range", "Upload-Offset"]
                forwards = "\n".join(
                    f"proxy_set_header {h} $http_{h.lower().replace('-', '_')};" for h in passed_headers)
                nginx = f'''
 daemon off;
 master_process off;
 pid {base}/nginx.pid;
 error_log {base}/nginx-error.log warn;
 events {{ worker_connections 32; }}
 http {{
   access_log off;
   client_body_temp_path {base}/body;
   proxy_temp_path {base}/proxy;
   fastcgi_temp_path {base}/fastcgi;
   uwsgi_temp_path {base}/uwsgi;
   scgi_temp_path {base}/scgi;
   server {{
     listen 127.0.0.1:{port} ssl;
     server_name localhost;
     ssl_certificate {cert};
     ssl_certificate_key {key};
     if ($host != localhost) {{ return 421; }}
     location / {{
       proxy_pass http://127.0.0.1:{backend};
       proxy_http_version 1.1;
       proxy_pass_request_headers off;
       proxy_request_buffering off;
       proxy_buffering off;
       proxy_connect_timeout 3s;
       proxy_read_timeout 10s;
       proxy_send_timeout 10s;
       proxy_set_header Host localhost:{port};
       proxy_set_header Connection close;
       proxy_set_header X-Forwarded-For $remote_addr;
       proxy_set_header X-Forwarded-Proto https;
       {forwards}
     }}
   }}
 }}
'''
                conf = base / "nginx.conf"
                conf.write_text(nginx)
                checked = subprocess.run(
                    [args.nginx, "-t", "-e", str(base / "nginx-error.log"), "-p", str(base) + "/", "-c", str(conf)],
                    capture_output=True, timeout=5)
                require(checked.returncode == 0,
                        "private nginx config: " + checked.stderr.decode(errors="replace")[:2048])
                proxy = subprocess.Popen(
                    [args.nginx, "-e", str(base / "nginx-error.log"), "-p", str(base) + "/", "-c", str(conf)],
                    stdout=subprocess.PIPE, stderr=proxy_err)
                deadline = time.monotonic() + 5
                while True:
                    require(proxy.poll() is None, "nginx startup failed")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        require(time.monotonic() < deadline, "nginx readiness deadline")
                        time.sleep(0.02)

                def call(method, path, expected, data=None, headers=None, direct=False):
                    h = dict(headers or {})
                    if isinstance(data, dict):
                        data = json.dumps(data).encode()
                        h["Content-Type"] = "application/json"
                    conn = http.client.HTTPConnection("127.0.0.1", backend,
                                                      timeout=5) if direct else http.client.HTTPSConnection("localhost",
                                                                                                            port,
                                                                                                            context=context,
                                                                                                            timeout=5)
                    try:
                        conn.request(method, path, body=data, headers=h)
                        response = conn.getresponse()
                        content = response.read(524289)
                        require(len(content) <= 524288, "response limit")
                        require(response.status == expected,
                                f"{method} expected {expected}, received {response.status}")
                        fields = {k.lower(): v for k, v in response.getheaders()}
                        value = json.loads(content) if content and fields.get("content-type", "").startswith(
                            "application/json") else content
                        return fields, value
                    finally:
                        conn.close()

                same = {"Origin": origin, "X-ANOF-Request": "1"}
                credentials = {"login": "administrator", "password": PASSWORD, "credential_mode": "cookie"}
                fields, value = call("POST", "/api/v1/auth/sessions", 201, credentials, same)
                require(set(value) == {"user", "expires_at", "credential_mode"}, "cookie login exposed extra fields")
                session = fields["set-cookie"]
                require(
                    all(x in session for x in ("Secure", "HttpOnly", "SameSite=Strict", "Path=/", "Max-Age=604800")),
                    "cookie attributes")
                require("Domain=" not in session, "host cookie domain")
                cookie = session.split(";", 1)[0]
                read = {"Cookie": cookie}
                write = dict(read, **same)
                _, me = call("GET", "/api/v1/users/me", 200, headers=read)
                require(str(uuid.UUID(me["instance_id"])) == me["instance_id"], "instance identity")
                credentials["credential_mode"] = "bearer"
                fields, bearer = call("POST", "/api/v1/auth/sessions", 201, credentials)
                require("set-cookie" not in fields, "bearer set cookie")
                auth = {"Authorization": "Bearer " + bearer["access_token"]}
                call("GET", "/api/v1/users/me", 200, headers=dict(auth, Cookie="theme=dark"))
                call("GET", "/api/v1/users/me", 401,
                     headers={"Cookie": "__Host-anof_session=" + bearer["access_token"]})
                call("GET", "/api/v1/users/me", 401, headers={"Authorization": "Bearer " + cookie.split("=", 1)[1]})
                call("GET", "/api/v1/users/me", 400, headers=dict(auth, **read))
                bad = dict(write, Origin="https://evil.test")
                fields, _ = call("DELETE", "/api/v1/auth/session", 403, headers=bad)
                require("set-cookie" not in fields, "unsafe origin cleared cookie")
                call("DELETE", "/api/v1/auth/session", 403, headers=dict(read, Origin=origin))
                call("GET", "/api/v1/users/me", 400, headers=read, direct=True)
                call("GET", "/api/v1/users/me", 403, headers=dict(read, Host="evil.test",
                                                                  **{"X-Forwarded-For": "127.0.0.1",
                                                                     "X-Forwarded-Proto": "https"}), direct=True)
                call("GET", "/api/v1/users/me", 421, headers=dict(read, Host="evil.test"))
                # 白名单转发清洗客户端伪造来源，不让它们成为后台身份。
                spoof = dict(read, Forwarded="for=evil",
                             **{"X-Real-IP": "evil", "X-Forwarded-For": "evil", "X-Forwarded-Proto": "http",
                                "X-Forwarded-Host": "evil"})
                call("GET", "/api/v1/users/me", 200, headers=spoof)
                _, spaces = call("GET", "/api/v1/spaces", 200, headers=read)
                space = next(s for s in spaces["items"] if s["kind"] == "private")
                payload = b"TLS cookie content"
                _, upload = call("POST", "/api/v1/uploads", 201,
                                 {"client_upload_id": str(uuid.uuid4()), "space_id": space["id"],
                                  "parent_id": space["root_node_id"], "name": "content.txt",
                                  "length": str(len(payload))}, write)
                path = "/api/v1/uploads/" + upload["id"]
                call("PATCH", path, 204, payload,
                     dict(write, **{"Content-Type": "application/offset+octet-stream", "Upload-Offset": "0"}))
                call("POST", path + "/complete", 202, {}, write)
                deadline = time.monotonic() + 5
                while True:
                    _, upload = call("GET", path, 200, headers=read)
                    if upload["state"] == "COMPLETED":
                        break
                    require(time.monotonic() < deadline, "upload publication deadline")
                    time.sleep(0.01)
                content_path = "/api/v1/nodes/" + upload["result_node_id"] + "/content"
                fields, downloaded = call("GET", content_path, 200, headers=read)
                require(downloaded == payload, "download bytes differ")
                require(fields["content-disposition"].startswith("attachment"), "download is not attachment")
                call("HEAD", content_path, 200, headers=read)
                _, partial = call("GET", content_path, 206, headers=dict(read, Range="bytes=0-2"))
                require(partial == payload[:3], "range bytes differ")
                fields, _ = call("DELETE", "/api/v1/auth/session", 204, headers=write)
                require("Max-Age=0" in fields["set-cookie"], "logout did not clear")
                call("GET", "/api/v1/users/me", 401, headers=read)
                call("GET", "/api/v1/users/me", 200, headers=auth)
                stop(proxy, signal.SIGQUIT)
                proxy = None
                logs = startup + stop(server)
                server = None
                logs += (base / "server.err").read_bytes() + (base / "nginx-error.log").read_bytes()
                for secret in (PASSWORD, cookie, bearer["access_token"]):
                    require(secret.encode() not in logs, "secret found in process logs")
                # 正常停止后锁已释放，真实本地校验无需恢复或重建。
                result = subprocess.run([args.anof, "--json", "config", "validate", "--root", str(root)],
                                        capture_output=True, timeout=5)
                require(result.returncode == 0, "instance lease not released")
            finally:
                failing = sys.exc_info()[0] is not None
                cleanup_failed = False
                for proc in (proxy, server):
                    if proc is not None:
                        try:
                            stop(proc, check=False)
                        except RuntimeError:
                            cleanup_failed = True
                if cleanup_failed and not failing:
                    raise RuntimeError("child process cleanup deadline")
    print("TLS proxy, mode isolation, same-origin, upload/download and shutdown acceptance passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("anof", "nginx", "openssl"):
        parser.add_argument("--" + name, required=True)
    run(parser.parse_args())
