# HTTPS 与安装测试

这里有两组可选测试：一组检查 ANOF 与 nginx 的 HTTPS 配合，另一组先安装程序，再使用实际部署模板检查登录、传输和恢复。它们都只使用自己的临时目录和进程，不启用主机服务。

## HTTPS 代理

[tls_proxy.py](tls_proxy.py) 使用真实 ANOF 进程、nginx 和 Python 标准库 HTTPS 客户端。初始化在私有 PTY 内输入测试密码；所有实例、临时证书、代理配置、PID、日志和各类 nginx 临时目录都位于独立的 `/tmp/anof-tls-*` 目录。测试不安装系统服务、不写系统信任库，也不使用已有实例。

要运行这一组，先准备现成的 nginx 和 openssl 可执行文件。CMake 只有在显式设置 `ANOF_TEST_NGINX` 时注册 `anof_tls_proxy_integration_test`；没有该参数的测试总数不包含 TLS 验收。nginx 必须是已有可执行文件，CMake 不下载依赖；同时需要 Linux Python 3 和 openssl。沿用当前构建配置及数据库依赖，再设置：

```sh
cmake --preset debug-tests -DANOF_TEST_NGINX=/absolute/path/to/nginx
cmake --build cmake-build-debug --target anof --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_tls_proxy_integration_test$' -j 1
```

夹具分配回环临时端口，生成带 localhost SAN 的临时自签名证书，仅在测试客户端信任该证书。代理关闭默认请求头透传，白名单传递业务头，覆盖 Host/转发来源/HTTPS 标记。后台校验固定同源配置和可信代理，不以代理单层清洗替代应用检查。

验证范围：Cookie 设置属性、响应不暴露 token、真实模式隔离、无关 Cookie 的 Bearer 请求、混合凭据拒绝、错误 Origin/缺 CSRF 不清 Cookie、直连及错误 Host 拒绝、伪造代理头清洗、小文件上传、GET/HEAD/Range 下载、注销及正常退出后实例锁释放，并扫描进程日志中的测试秘密。普通请求有 5 秒期限、响应 512KiB 上限、进程启停期限和 CTest 60 秒上限；失败也清理本次创建的子进程及目录。

这一组使用 Python HTTPS 客户端，没有运行浏览器。Cookie 的浏览器行为需要另外运行 Web 的 Chromium/Firefox 测试。代理配置仅为测试夹具，不是完整生产部署模板；CSP、静态资源、HSTS、systemd 及主机日志轮转不由此夹具建立。

## 安装与部署示例

[install_smoke.py](install_smoke.py) 在显式启用 `ANOF_ENABLE_INSTALL=ON`、`ANOF_BUNDLE_SQLITE=ON`、提供绝对路径 `ANOF_WEB_DIST` 及 `ANOF_TEST_NGINX` 后注册为 `anof_install_deployment_test`。沿用上述配置与 SQLite 参数，预先构建 Web dist，再执行：

```sh
cmake --preset debug-tests -DANOF_ENABLE_INSTALL=ON -DANOF_BUNDLE_SQLITE=ON -DANOF_TEST_NGINX=/absolute/path/to/nginx -DANOF_WEB_DIST="$PWD/web/dist"
cmake --build cmake-build-debug --target anof --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_(install_deployment|tls_proxy_integration)_test$' -j 1
```

测试将 Runtime 组件安装到私有临时前缀并整体移动，通过 ELF、ldd 和实际加载检查 SQLite 来自安装产物。使用已安装的 [nginx 示例](../../deploy/nginx/anof.conf)，仅替换域名、端口和路径，验证静态 MIME/缓存/CSP/HSTS、API 404、实例文件不可访问、代理头清洗、Cookie 登录、小文件上传、GET/HEAD/Range/304、日志无测试秘密。随后停止服务，执行 Full FSCK、备份、新目录恢复与拒绝覆盖，重新启动恢复实例，确认旧 Cookie 失效及下载内容一致；同时检查监听冲突和停止后的锁释放。

使用 `/tmp/anof-install-*` 小实例和临时证书，不写系统信任库、不安装或启动主机 systemd 服务。测试强制串行、CTest 上限 120 秒；子命令 30 秒、HTTP5 秒、响应 512KiB。安装测试不替代真实浏览器测试、目标主机服务隔离/开机启动或物理掉电验收。可用 `ctest --test-dir cmake-build-debug -N` 确认两组测试确实注册，未注册的可选测试不会出现在结果中。
