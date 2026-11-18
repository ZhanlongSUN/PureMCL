# PureMCL

面向 Windows 11 x64 的非官方 Minecraft Java Edition 启动器。应用源码为 ISO C17，界面使用 Win32、Direct2D、DirectWrite、WIC 和 Windows Common Controls。微软验证由默认浏览器完成，启动器不嵌入浏览器。项目没有遥测或常驻服务。

## 构建

1. 打开 `src/auth/client_id.h`，在 `NMC_MICROSOFT_CLIENT_ID` 的引号内填写微软应用程序（客户端）ID。留空仍可编译和安装游戏文件，但无法登录或启动游戏。
2. 安装 MSVC x64（含 Windows SDK 和 C17 原子操作支持）、CMake 3.22+ 和 Ninja。将 CMake 加入 PATH，在项目目录运行：

```powershell
.\build-msvc.cmd Release
```

脚本会初始化 MSVC x64，从 PATH、Visual Studio 或已安装的 CLion 查找 Ninja，构建并运行 CTest，再把 Release 产物放到 `out/portable`。使用日志追踪时，请将 `PureMCLMonitor.exe` 与 `PureMCL.exe` 放在同一目录。也可双击 `build-msvc.cmd`（默认 Release）。若 Ninja 位于其他目录，将环境变量 `NMC_NINJA_EXE` 指向其完整路径。构建使用本地 yyjson、miniz 压缩包，不联网。

## 使用

1. 在「安装」输入新的实例名，选择 Minecraft 版本和加载器，然后点击「安装版本」。可刷新官方版本目录。安装会把 `latest` 固定为实际版本；更换 Minecraft 版本或加载器请用新实例名。
2. Fabric、Forge、NeoForge 默认从官方源选择兼容版本，也可填写精确的加载器版本。Forge 和 NeoForge 的自动安装需要支持客户端命令行的官方安装器；旧版本可借助「导入加载器配置」。OptiFine 请先用[官方 OptiFine 安装器](https://optifine.net/downloads)在已有正版 Minecraft 目录生成配置，再选相同游戏版本，导入 `versions/<OptiFine 版本>/<OptiFine 版本>.json`。PureMCL 会从该安装目录复制需要的 OptiFine 库。缺少兼容 Java 时会获取 Mojang 提供的 Windows x64 运行时；也可在「设置」指定 Java。
3. 在「账户」点击「添加微软账号」，用显示的代码在系统浏览器完成验证。编译时填写的应用 ID 须支持个人微软账号和 [Device Code Flow](https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code)，并[获准访问 Minecraft Services](https://aka.ms/mce-reviewappid)。无需客户端密钥。应用使用 DPAPI 保存本机凭据；换用不同应用 ID 后需要重新登录。
4. 回到主页选择已安装的实例并点击「启动游戏」。每次启动都在线核验 Minecraft Java 资格和玩家身份。在「已安装」可校验和修复，或卸载所选实例。卸载前请备份存档：卸载会删除此实例的存档、模组和设置，保留其他实例与共享资源。

「设置」中的「追踪游戏日志」默认关闭。启用后，游戏运行期间会保留一个低优先级的监控进程，即使主窗口已经退出。检测到异常退出时会弹出系统对话框，并在找到本次运行的日志时提供「打开最新日志」。监控结束后进程退出。新配置的 JVM 初始堆为 256 MiB，已保存的实例继续使用自己的内存和 GC 设置。

导入整合包：在「安装」输入新实例名，再点击「导入整合包」。[Modrinth `.mrpack`](https://support.modrinth.com/en/articles/8802351-modrinth-modpack-format-mrpack) 会自动读取 Minecraft 与加载器版本、按 SHA-1 和 SHA-512 校验下载并应用客户端覆盖文件；默认包含标记为可选的客户端文件。ZIP、RAR、7z 和 tar 压缩包应实际包含 `mods`、`config` 等游戏文件，导入前请手动选择游戏及加载器。普通包也可包含一层包装目录。Quilt 与只包含外部项目引用的 CurseForge 清单包暂不支持。

版本目录固定为 `PureMCL.exe` 同级的 `versions`；请将程序放在当前用户有写权限的目录中。「设置」仅显示这个路径。存档、模组、配置和日志分开存放在 `versions/<版本>/instances/<实例>/game`。同一版本的客户端 JAR 和启动器解包的旧版 DLL 只保留一份；资源、库和 Java 运行时由所有版本共享，位于 `%LOCALAPPDATA%\PureMCL`。设置和账号也保存在这个用户目录。自定义 JVM 参数每行一项。

升级前旧位置中的文件不会自动搬动。若要保留已有存档，可在新目录生成后将旧实例的 `saves` 复制到对应版本和实例的 `game` 下。若希望复用旧版已下载的客户端 JAR，可把对应版本目录放到新程序的 `versions` 下，然后执行修复。

资源、游戏依赖和整合包下载可在「设置」配置 1 至 16 个工作线程。资源先按摘要去重，再放入同一下载队列。首次需要下载可镜像的文件时，启动器通过 [IP.SB GeoIP](https://ip.sb/api/) 查询当前公网出口地区，并在本次任务内复用判断结果。中国大陆出口（`CN`）优先使用 [BMCLAPI](https://bmclapidoc.bangbang93.com/)，镜像不可用时回退官方。其他地区或地区查询失败时使用官方源。所有文件仍按官方元数据校验，支持范围请求的服务器可接续已有临时文件。界面显示已完成文件数和本次任务接收的数据量。

## 验证与范围

- CTest 覆盖路径验证、Unicode 命令行、参数规则、BCrypt 校验、超长 Classpath 桥接 JAR、原生库恢复、进程交接、已安装列表、共享文件保留、整合包路径校验、按公网 IP 地区选源和镜像回退、下载尾段进度，以及 Direct2D / DirectWrite / WIC 初始化。
- 可执行真实安装测试。下面的命令将数据写到构建目录，不接触已有 Minecraft 安装；替换版本号即可测试其他官方版本：
  ```powershell
  $root = Join-Path (Get-Location) 'out\integration'
  .\out\Release\nmc_tests.exe $root 26.3 install
  .\out\Release\nmc_tests.exe $root 26.3 local
  .\out\Release\nmc_tests.exe $root 26.3 repair
  ```
  `local` 检查本地文件并运行 `java -version`，不会绕过认证启动游戏。
- 快速启动会重用本地版本 JSON、资源收据和运行时，检查必要文件大小，不会为每次启动重新散列所有资源。单个资源丢失或被同尺寸内容替换时，请使用完整修复。
- 解析器可处理官方目录中的父版本链和支持的加载器配置。任意旧版规则正则及自定义 Maven 仓库不在支持范围。至少 1 MiB 的文件在服务器接受 HTTP Range 时可续传。
- 已用官方数据验证 26.3 和 1.8.9 的安装和启动准备，另验证 1.20.1、1.21.11 的元数据和 Java 版本解析。真实账号登录和游戏主菜单仍需使用获授权的 Client ID 验证。

