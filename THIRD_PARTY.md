# 第三方组件

| 组件 | 版本 | 位置 / 用途 | 许可 |
| --- | --- | --- | --- |
| ChaLi JSON | db593483 源码快照 | cpp/vendor/chali_json，C++17 原生 JSON 值、解析和序列化 | Apache-2.0，licenses/ChaLi-JSON-Apache-2.0.txt 和 ChaLi-JSON-NOTICE.txt |
| Gson | 2.11.0 | java/lib/gson-2.11.0.jar，Java JSON | Apache-2.0，licenses/Gson-Apache-2.0.txt |
| cJSON | 1.7.19 | c/vendor/cjson，C99 JSON 解析和序列化；保留上游源码 | MIT，c/vendor/cjson/LICENSE |
| libcurl | 7.75.0-DEV（Windows 开发依赖） | 当前 Windows C/C++ 二进制静态链接；源码构建可选择其他兼容版本 | curl license，licenses/curl-COPYING.txt |
| OpenSSL | 1.1.1w（Windows 开发依赖） | 当前 Windows libcurl 的静态 TLS 依赖 | OpenSSL/SSLeay license，licenses/OpenSSL-LICENSE.txt |
| zlib | 1.2.13（Windows 开发依赖） | 当前 Windows libcurl 的静态依赖 | zlib license，licenses/zlib-README.txt |

Gson SHA-256：`57928d6e5a6edeb2abd3770a8f95ba44dce45f3b23b7a9dc2b309c581552a78b`。构建脚本固定版本及校验值，桌面包包含该依赖以便直接运行。C++ 库及控制台源码不绑定上述 libcurl/OpenSSL 版本，宿主可通过 CMake 依赖前缀使用自己的版本。

本框架采用 GNU AGPL-3.0 许可，见 LICENSE.txt。API Key 由用户在运行时输入，发行包不包含凭据。Java 文档由 JDK javadoc 从带注释的公开接口生成。

预编译开发依赖位于 `deps/windows-x64`，供 Windows MSVC 构建使用；相关许可文件位于 licenses。源码也支持由宿主指定独立安装的 libcurl 开发包。

JSON 源文件与选定快照保持一致，框架通过独立的 json_util.hpp/json_util.cpp 提供严格类型读取及协议边界检查。该组件不要求安装 ChaLi。

- `cpp/vendor/chali_json/json.hpp` SHA-256：`f37a4d0f88bb93d7682c4a8bcd516d83e0a3af0e49ac0ce8b4b410dede67553f`。
- `cpp/vendor/chali_json/json.cpp` SHA-256：`099a31f83f20956777d7a77dd93b21ee326caf340fd204a13562d8f1c3f5b3e7`。

## 新增语言依赖

- Python 3.10+ 版本运行时仅使用标准库 json、http.client、ssl、subprocess、threading、queue、pathlib 等，无第三方 MCP/模型 SDK 依赖。可选的 pip 包安装使用 setuptools 构建后端，直接运行源码无需安装构建工具。

- Go 使用标准库 encoding/json、net/http、os/exec；密钥输入使用 golang.org/x/term v0.29.0，其间接依赖 x/sys v0.30.0。版本由 go.mod/go.sum 锁定，BSD 许可文件见 licenses/go，Go 运行时许可见 Go-runtime-LICENSE.txt。
- Rust 使用 serde_json、reqwest（blocking + rustls TLS）、rpassword；Cargo.lock 锁定具体依赖。当前 Windows 构建的依赖清单及原许可文件见 licenses/rust/INDEX.md。许可证集合包含 MIT、Apache-2.0、BSD、ISC、Unicode 和 CDLA 等，按各组件原条款保留。
- C# 使用 .NET 9 标准库 System.Text.Json、HttpClient 和 Process，无额外 NuGet 包；按需使用宿主安装的 .NET 运行时。许可见 licenses/DotNet-LICENSE.txt。
