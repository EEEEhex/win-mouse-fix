# 配置格式 v1

`configuration.schema.json`、`ProductConfiguration.cs` 与 Host `ProductConfig` 共用本目录的 `default.json` / `custom.json` 样例。JSON 为 UTF-8，最多 64 KiB，未知字段、未知动作、非有限数值整体拒绝。Host 是唯一保存者；导入的 revision 不复活历史操作，提交时基于 expectedRevision 分配新修订。

| 字段 | 单位及语义 |
| --- | --- |
| content.thresholdCounts | 相对鼠标原始计数的累计净距离 |
| content/shell.mmPerCountX/Y | 合成触点毫米 / 原始计数；符号控制方向 |
| shell.fingers | 3 或 4；实际 OS 映射需要校准 |
| shell.directionRatio | 优势轴距离与次轴距离之比 |
| wheel.mmPerNotch | 每 120 个原始滚轮单位映射的毫米位移；符号控制方向 |
| wheel.*Milliseconds | 有界滚动计划时间，非直接输入延迟；high 不小于 regular |
| zoom.mmPerNotch | 起始半间距下扩张一格的参考毫米量；每格比例为 1+abs(mmPerNotch)/initialPinchMm，缩小取倒数，符号控制方向 |
| zoom.initialPinchMm | 双指起始半径；中心不移动 |
| timing.doubleClickMs / holdMs | 毫秒；首启默认双击时间取系统值，固定样例为 500 |
| timing.doubleClickTolerancePixels | 两次按下的屏幕坐标差容差，像素 |
| bindings.modifiers | Shift=1、Control=2、Alt=4、Windows=8，组合使用位或 |
| bindings.keys | 用户明确录入的虚拟键组合，最多 8 个；不包含文本或可执行路径 |

跨绑定检查由两侧校验器执行：ID 唯一、同按钮/修饰条件/触发方式唯一、通用 wheel 与 wheel-up/down 不重叠。Core 在保存后再次校验，不依赖列表顺序决定冲突结果。中键默认透传，左右主键不在可绑定目录。

Host 采用 SDK 10.0.28000.0 所带 C++/WinRT 2.0.250303.5 投影和系统 Windows.Data.Json，不新增下载或分发第三方 JSON DLL。投影遵循 SDK 头中的 MIT 许可；运行时随 Windows 维护，OS 构建号独立记录。[Windows.Data.Json 文档](https://learn.microsoft.com/en-us/uwp/api/windows.data.json.jsonobject.parse)。Settings 使用当前 .NET 8 的 System.Text.Json。

持久化先完整写入同目录临时文件并 FlushFileBuffers，再更新已知有效备份及替换主文件；不请求跨卷复制。原子发布使用 [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)。ACK 前进程丢失时，客户端通过实际修订查询确认结果。损坏或不兼容主文件不会自动覆盖；显式恢复先另存原件，再走正常提交路径。

自动检查仅写 `artifacts/product-validation`。`WinMouseFix.Tests.exe --config-goldens` 检查原生解析、持久化与故障语义；`dotnet run --project WinMouseFix/WinMouseFix.Tests/ConfigGoldens` 检查托管模型。两侧互读生成的规范化 JSON，运行顺序不影响已有有效样例。
