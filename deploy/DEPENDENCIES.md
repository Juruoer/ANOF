# 依赖与许可证

ANOF 安装后，许可证保存在安装目录下的 `share/anof/licenses`。其中 `LICENSE` 是 ANOF 的 BSD-3-Clause 许可证，`muduox-LICENSE` 包含 Muduo 和 muduox 的版权与许可声明。

## 运行时使用的组件

| 组件 | 用途与安装方式 | 许可材料 |
| --- | --- | --- |
| muduox | HTTP 服务，随 ANOF 源码编译并链接 | 安装后的 `muduox-LICENSE` |
| SQLite | 保存元数据，默认随程序安装到 `lib/anof` | [SQLite 声明](licenses/SQLite.txt) |
| nlohmann-json、CLI11 | JSON 处理和命令行解析，头文件代码编入程序 | [JSON](licenses/nlohmann-json3-dev.copyright)、[CLI11](licenses/libcli11-dev.copyright) |
| toml++、libsodium、libcurl | 配置解析、密码与摘要、HTTP 客户端，由系统提供动态库 | [toml++](licenses/libtomlplusplus-dev.copyright)、[libsodium](licenses/libsodium-dev.copyright)、[curl](licenses/libcurl4-openssl-dev.copyright) |
| hash-wasm | Web 上传时计算 SHA-256，编入 Web 资源 | Web 目录的 `THIRD_PARTY_NOTICES.txt` |

如果构建时设置了 `ANOF_BUNDLE_SQLITE=OFF`，SQLite 也使用指定的外部安装，不会复制到 `lib/anof`。

可以查看程序实际加载的库：

```sh
ldd /usr/local/bin/anof
```

其中还会列出 C++ 运行库、系统 C 库，以及 libcurl 使用的 TLS、压缩等库。迁移程序时需要一起核对这些依赖；出现 `not found` 表示目标机器缺少相应库。

## 构建和测试工具

Node.js、npm、Vite 和 TypeScript 用于构建 Web；Boost.Test、Python、Playwright 和浏览器用于测试。运行安装好的 ANOF 和静态 Web 页面不需要安装这些工具。

源码中的构建说明列出了所需版本，Web 的 `package-lock.json` 和 `toolchain-lock.json` 记录了包和工具版本。

## 再分发

随程序保留 `share/anof/licenses`，随 Web 文件保留 `THIRD_PARTY_NOTICES.txt`。如果另外打包了系统动态库，也需要附上那些库要求的许可和分发材料。

这里收录的 Ubuntu 包版权文件可能包含包内其他文件的条款。附带的 Apache、GPL、LGPL 等文本用于这些条款的引用，并不表示 ANOF 自身采用了这些许可证。更换依赖版本后，请核对相应版本的版权文件。

## SQLite 源码记录

构建示例使用官方 `sqlite-autoconf-3540100`（3.54.1）：

```text
Source ID:
2026-10-16 14:45:20 555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76

源码归档 SHA3-256:
b6ced585c750916b1052667239a12c25a7ff34e7f06e1e6e01ecb0427657d9c4
```

安装时复制的是 CMake 实际选择的 SQLite 库。如果采用其他符合版本要求的安装，请记录自己的来源和构建选项。
