# PureMCL

面向 Windows 11 x64 的非官方 Minecraft Java Edition 启动器。应用源码为 ISO C17，界面使用 Win32、Direct2D、DirectWrite、WIC 和 Windows Common Controls。微软验证由默认浏览器完成，启动器不嵌入浏览器。项目没有遥测或常驻服务。

## 构建

1. 打开 `src/auth/client_id.h`，在 `NMC_MICROSOFT_CLIENT_ID` 的引号内填写微软应用程序（客户端）ID。留空仍可编译和安装游戏文件，但无法登录或启动游戏。
2. 安装 MSVC x64（含 Windows SDK 和 C17 原子操作支持）、CMake 3.22+ 和 Ninja。将 CMake 加入 PATH，在项目目录运行：

```powershell
.\build-msvc.cmd Release
```

脚本会初始化 MSVC x64，从 PATH、Visual Studio 或已安装的 CLion 查找 Ninja，构建并运行 CTest，再把 Release 产物放到 `out/portable/PureMCL.exe`，旁边附带说明和依赖许可证。也可双击 `build-msvc.cmd`（默认 Release）。若 Ninja 位于其他目录，将环境变量 `NMC_NINJA_EXE` 指向其完整路径。构建使用本地 yyjson、miniz 压缩包，不联网。

## 使用

1. 在「版本与实例」刷新版本目录，选择官方版本或输入版本号。每个实例有自己的版本、Java、JVM 参数和游戏目录。安装会把 `latest` 固定为实际版本；要切换版本时重新选择即可。
2. 点击「安装版本」。缺少兼容 Java 时会根据元数据获取 Mojang 提供的 Windows x64 运行时。可在「设置」指定 Java 安装目录或完整的 java.exe / javaw.exe 路径。
3. 在「账户」点击「添加微软账号」，用显示的代码在系统浏览器完成验证。编译时填写的应用 ID 须支持个人微软账号和 [Device Code Flow](https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code)，并[获准访问 Minecraft Services](https://aka.ms/mce-reviewappid)。无需客户端密钥。应用使用 DPAPI 保存本机凭据；换用不同应用 ID 后需要重新登录。
4. 回到主页点击「启动游戏」。每次启动都在线核验 Minecraft Java 资格和玩家身份；断网、未登录、身份不符或资格校验失败时不启动游戏。需要完整性校验时使用「校验和修复」。移除实例只删除注册信息，保留游戏文件和存档。

版本目录固定为 `PureMCL.exe` 同级的 `versions`；请将程序放在当前用户有写权限的目录中。「设置」仅显示这个路径。存档、模组、配置和日志分开存放在 `versions/<版本>/instances/<实例>/game`。同一版本的客户端 JAR 和启动器解包的旧版 DLL 只保留一份；资源、库和 Java 运行时由所有版本共享，位于 `%LOCALAPPDATA%\PureMCL`。设置和账号也保存在这个用户目录。自定义 JVM 参数每行一项。

升级前旧位置中的文件不会自动搬动。若要保留已有存档，可在新目录生成后将旧实例的 `saves` 复制到对应版本和实例的 `game` 下。若希望复用旧版已下载的客户端 JAR，可把对应版本目录放到新程序的 `versions` 下，然后执行修复。

下载以官方源为默认。可校验文件的官方 HTTPS 响应耗时达到 1.5 秒、请求失败或读取超时后，尝试 [BMCLAPI](https://bmclapidoc.bangbang93.com/)；镜像不可用或校验失败时回退到官方。所有镜像文件按官方元数据的大小和摘要校验。账号请求、版本清单和运行时目录清单始终使用官方服务。

## 验证与范围

- CTest 包括路径验证、Unicode 命令行转义、参数规则、BCrypt 校验、超长 Classpath 桥接 JAR、原生库缺失恢复、进程交接，以及 Direct2D / DirectWrite / WIC 初始化和调整尺寸。
- 可执行真实安装测试。下面的命令将数据写到构建目录，不接触已有 Minecraft 安装；替换版本号即可测试其他官方版本：
  ```powershell
  $root = Join-Path (Get-Location) 'out\integration'
  .\out\Release\nmc_tests.exe $root 26.3 install
  .\out\Release\nmc_tests.exe $root 26.3 local
  .\out\Release\nmc_tests.exe $root 26.3 repair
  ```
  `local` 检查本地文件并运行 `java -version`，不会绕过认证启动游戏。
- 快速启动会重用本地版本 JSON、资源收据和运行时，检查必要文件大小，不会为每次启动重新散列所有资源。单个资源丢失或被同尺寸内容替换时，请使用完整修复。
- 当前以官方原版为测试范围。解析器处理官方目录中可解析的父版本链；外部 Mod 加载器、任意旧版规则正则和自定义 Maven 仓库不在支持范围。至少 1 MiB 的文件在服务器接受 HTTP Range 时可续传；所有完成文件都要通过完整摘要校验。
- 窗口首次绘制和点击到进程交接使用 QPC 计时，输出到状态区及调试器。未采集主菜单可交互时间、FPS、1% Low 或帧时间 P99；没有据此宣称游戏性能提升。
- 已用官方数据验证 26.3 和 1.8.9 的安装和启动准备，另验证 1.20.1、1.21.11 的元数据和 Java 版本解析。真实账号登录和游戏主菜单仍需使用获授权的 Client ID 验证。

