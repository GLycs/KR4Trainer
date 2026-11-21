# KR4 修改器（Kingdom Rush Vengeance Trainer）

面向《Kingdom Rush Vengeance》的单机内存修改器，以 DLL 注入 + OpenGL 覆盖窗口（ImGui）形式工作，不修改任何游戏文件。

当前版本：**v0.6**

## 功能

| 分组 | 功能 |
| --- | --- |
| 金币相关 | 金币锁定、金币数指定、金币倍率（1–100 倍，仅放大正向增量） |
| 英雄相关 | 英雄经验锁定、经验倍率（1–10）、技能点免消耗、升级点锁定 |
| 基础功能 | 生命锁定与血量设置、友军无敌、一击必杀、伤害倍率、游戏速率（0.10x–4.00x，含 1.00x 复位）、一键安装全部 / 全部开启 / 全部关闭 |

热键：`INSERT` 显示 / 隐藏面板；`END` 卸载全部 Hook。

## 目录结构

```
src/                 overlay.cpp（面板与 Hook 主体）、injector.cpp（注入器）
build.bat            MinGW-w64 构建脚本
docs/                使用说明（Markdown / docx）
release/             v0.6 可直接使用的分发包与 zip
```

## 快速上手

1. 先启动游戏，进入关卡；
2. 双击 `release\KR4修改器_v0.6\启动修改器.bat`（或直接运行 `kr4injector.exe`）；
3. 按 `INSERT` 呼出面板。

手动注入：

```
kr4injector.exe --dll kr4mod.dll [--name "Kingdom Rush Vengeance.exe" | --pid <PID>] [--wait <秒>]
```

退出码与常见问题见 [使用说明](docs/KR4修改器_使用说明_v0.5.md)。

## 命令通道与日志

- 命令文件：`%TEMP%\kr4mod_cmd.txt`，内容变化时执行一次
- 日志文件：`%TEMP%\kr4mod.log`，可用环境变量 `KR4MOD_LOG` 覆盖路径
- 渲染开关：创建 `%TEMP%\kr4mod_render.off` 后 DLL 停止绘制

## 构建

依赖：

- MinGW-w64 GCC（x64，UCRT 运行时）
- [Dear ImGui 1.90.9](https://github.com/ocornut/imgui)（含 `backends/`）
- [MinHook](https://github.com/TsudaKageyu/minhook)

将两者解压到 `third_party\imgui-1.90.9` 与 `third_party\minhook`，然后：

```
set MINGW=C:\mingw64\bin
build.bat
```

产物为 `bin\kr4mod.dll` 与 `bin\kr4injector.exe`。

## 运行环境

- Windows 10 / 11 x64；Windows 7 / 8 需先安装 KB2999226（UCRT）
- 游戏版本 1.16.4.0（Steam appid 1367550）已验证；其他版本需重新定位特征码
- 同一进程只允许注入一个 kr4mod DLL

## 免责声明

仅供个人学习与单机存档体验使用。不含、不分发任何游戏本体文件。使用修改器可能影响成就与游戏平衡，请自行承担风险；请勿用于联机作弊或商业用途。

## 许可

本项目代码采用 MIT 许可，见 [LICENSE](LICENSE)。第三方组件（Dear ImGui、MinHook）遵循各自许可，见 [THIRD_PARTY.md](THIRD_PARTY.md)。
