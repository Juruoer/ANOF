# 编译和安装

下面以 Ubuntu 24.04 x86_64 为例。编译后得到一个 `anof` 程序，服务、命令行客户端和本地维护都使用它。浏览器界面需要另外构建。

## 准备依赖

先按根目录 [README](README.md#获取源码) 下载源码和 muduox 子模块，再安装编译依赖：

```sh
sudo apt install build-essential cmake pkg-config git libcurl4-openssl-dev \
  libsodium-dev libtomlplusplus-dev libcli11-dev nlohmann-json3-dev
```

各依赖的版本要求如下。CMake 会在配置时检查它们，缺少依赖或版本过低时会给出错误。

| 依赖          | 要求                                                         |
|---------------|--------------------------------------------------------------|
| C++ 编译器    | 支持 C++17，已使用 GCC 13.3.0 构建                           |
| CMake         | 3.28.3 或更高                                                |
| SQLite        | 3.54.1 或更高的 3.x 共享库，头文件与库版本一致，启用线程支持 |
| libsodium     | 1.0.18 或更高                                                |
| nlohmann-json | 3.11.3 或更高                                                |
| CLI11         | 2.4.1 或更高                                                 |
| toml++        | 3.4.0 或更高                                                 |
| libcurl       | 8.5.0 或更高，支持 HTTP、HTTPS 和大文件                      |

Ubuntu 自带的 SQLite 可能不满足要求。可以按本文末尾的 [SQLite 安装示例](#sqlite) 单独编译，再将安装路径交给构建脚本。

只编译 `anof` 不需要 Node.js、Boost 或浏览器。Web 构建使用 Node.js **24.22.1** 和 npm **11.19.1**；运行 C++ 测试才需要
Boost.Test 和 Python，见下方[测试](#测试)。

## 编译 ANOF

在项目根目录执行：

```sh
./scripts/build.sh --preset release
```

如果 SQLite 装在其他位置，用 `--sqlite-prefix` 指定包含 `include` 和 `lib` 的安装目录：

```sh
./scripts/build.sh --preset release --sqlite-prefix /path/to/sqlite-prefix
```

成功后，程序位于 `cmake-build-release/apps/anof/anof`。可以先查看帮助：

```sh
./cmake-build-release/apps/anof/anof --help
```

脚本默认使用两个编译任务。常用选项如下，完整参数可用 `./scripts/build.sh --help` 查看。

| 选项               | 用法                                        |
|--------------------|---------------------------------------------|
| `--preset debug`   | 编译 Debug 版本，输出到 `cmake-build-debug` |
| `--jobs 4`         | 使用四个编译任务                            |
| `--build-dir DIR`  | 改用指定的构建目录                          |
| `--target anof`    | 只构建 `anof` 及其依赖                      |
| `--configure-only` | 检查依赖并生成构建文件，暂不编译            |
| `--web-dist DIR`   | 将已构建的 Web 文件加入安装内容             |

额外的 CMake 参数放在 `--` 后，例如 `-- -DANOF_BUNDLE_SQLITE=OFF`。脚本只负责配置和编译，依赖需要事先装好。

## 构建 Web

在已安装上述 Node.js/npm 的环境中执行：

```sh
cd web
./scripts/npm.sh ci
./scripts/npm.sh run tools:check
./scripts/npm.sh run build
cd ..
./scripts/build.sh --preset release --web-dist "$PWD/web/dist"
```

最后一条命令让 CMake 安装 `web/dist`。如果你只使用命令行，可以跳过这一节；若之前配置过 Web，使用 `-- -DANOF_WEB_DIST=`清除旧路径。

Web 脚本会先查找 PATH 中版本匹配的 Linux Node，也能使用已有的 `web/.tools` 安装。需要指定其他位置时，设置 `ANOF_NODE` 和
`ANOF_NPM`，示例见 [Node.js](#nodejs)。在 WSL 中也应使用 Linux 工具，不要调用 Windows 的 Node/npm。

## 安装

先安装到一个新目录，检查生成的文件：

```sh
cmake --install cmake-build-release --prefix "$PWD/package-root" --component Runtime
./package-root/bin/anof --help
ldd package-root/bin/anof
```

`package-root` 中包含程序、SQLite 库、部署示例和许可证；如果配置了 Web，也会包含静态页面。安装系统服务时，可改为 `/usr/local`：

```sh
sudo cmake --install cmake-build-release --prefix /usr/local --component Runtime
```

安装后还需要创建实例、设置 HTTPS 和启动服务，请继续阅读 [部署说明](deploy/README.md)。

默认会把所选的 SQLite 库一并安装到 `lib/anof`，程序通过相对路径加载它。因此移动安装目录时，需要保留整个目录结构。其他动态库仍由系统提供。

如果希望使用外部 SQLite，可在构建时加上 `-- -DANOF_BUNDLE_SQLITE=OFF`。此时程序会记住所选库的目录，该目录在运行时必须仍然可访问。systemd
示例禁止服务访问用户家目录，使用外部库时请将它放在服务可读取的位置。

## 测试

C++ 测试要求 Boost.Test >= 1.74、Python >= 3.12；已有测试使用 Boost 1.92.0 和 Python 3.12.3。安装测试依赖后，可以执行：

```sh
./scripts/build.sh --preset debug-tests --test
./scripts/build.sh --preset release-tests --test
```

普通 `debug` 和 `release` preset 不启用测试。测试类型及单独运行的方法见 [测试说明](tests/README.md)；Web 和 HTTPS
部署测试分别见 [Web 文档](web/README.md) 与 [部署测试](tests/deployment/README.md)。

## 直接使用 CMake

脚本使用的是项目自带的 CMake Presets，也可以在终端或 IDE 中直接使用：

```sh
cmake --preset release -DSQLite3_ROOT=/path/to/sqlite-prefix
cmake --build --preset release
```

依赖位于系统搜索路径时，省略 `SQLite3_ROOT` 即可。四个 preset 使用 Unix Makefiles：`debug` 与 `debug-tests` 共用 Debug
构建目录，`release` 与 `release-tests` 共用 Release 目录，不要同时在同一目录构建。

个人设置可以放在不提交的 `CMakeUserPresets.json` 中：

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "local-debug",
      "inherits": "debug-tests",
      "cacheVariables": {
        "SQLite3_ROOT": "/path/to/sqlite-prefix"
      }
    }
  ]
}
```

随后用 `cmake --preset local-debug` 配置，再用 `cmake --build cmake-build-debug --parallel 2` 编译。构建脚本只接受项目自带的四个
preset，自定义 preset 请直接交给 CMake。

更换 SQLite 安装位置后，如果 CMake 仍找到旧库，重新使用脚本的 `--sqlite-prefix`；它会清除 SQLite 的旧查找结果。直接使用
CMake 时，添加 `-USQLite3_INCLUDE_DIR -USQLite3_LIBRARY` 后重新指定 `SQLite3_ROOT`
。配置会检查实际库的版本和能力，因此只有头文件更新而库仍旧时也会报错。此检查需要运行本机程序，构建入口不支持交叉编译。

运行 `anof` 时若看到 `stage=dependencies`，用 `ldd /path/to/anof` 检查实际加载的库，再核对构建时选用的路径。

## 单独安装 SQLite 和 Node.js

以下命令需要 curl、tar、xz；SQLite 的下载校验还使用 Python 3。请在空工作目录中操作，安装目录由你自行选择。

### SQLite

下面以 SQLite 3.54.1 为例。先把 `sqlite_prefix` 改成你准备使用的绝对路径；建议单独安装，以免影响系统自带的 SQLite。

```sh
(
set -eu
mkdir sqlite-source-build
cd sqlite-source-build
curl --fail --location --output sqlite-autoconf-3540100.tar.gz \
  https://sqlite.org/2026/sqlite-autoconf-3540100.tar.gz
python3 - <<'PY'
import hashlib
from pathlib import Path
assert hashlib.sha3_256(Path('sqlite-autoconf-3540100.tar.gz').read_bytes()).hexdigest() == 'b6ced585c750916b1052667239a12c25a7ff34e7f06e1e6e01ecb0427657d9c4'
PY
tar -xzf sqlite-autoconf-3540100.tar.gz
cd sqlite-autoconf-3540100
sqlite_prefix=/absolute/path/to/new-sqlite-prefix
./configure --prefix="$sqlite_prefix" --libdir="$sqlite_prefix/lib"
make -j2
make install
)
```

### Node.js

如果还没有所需版本的 Node.js，可以下载官方 Linux x64 包。下面的校验值与 [工具锁](web/toolchain-lock.json) 一致。在一个空目录中执行：

```sh
(
set -eu
curl --fail --location --output node-v24.22.1-linux-x64.tar.xz \
  https://nodejs.org/dist/v24.22.1/node-v24.22.1-linux-x64.tar.xz
printf '%s\n' '6c172a9ab6718f7b801fb5a7bd486af2ee5e32758086a04b84fcce2aa77095e6  node-v24.22.1-linux-x64.tar.xz' | sha256sum -c -
tar -xf node-v24.22.1-linux-x64.tar.xz
)
if [ "$?" -ne 0 ]; then exit 1; fi
export ANOF_NODE="$PWD/node-v24.22.1-linux-x64/bin/node"
export ANOF_NPM="$PWD/node-v24.22.1-linux-x64/lib/node_modules/npm/bin/npm-cli.js"
```

保持这两个环境变量，回到项目的 `web` 目录后即可运行 `./scripts/npm.sh ci`。
