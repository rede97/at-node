# wchble/ — WCH BLE series

> WCH（沁恒）BLE MCU 系列 AT Node：USB CDC + BLE 双模键盘与 BLE 接收器。
> 低成本、低功耗定位，是项目的原始平台。

## 变体矩阵

| 目录 | 工程类型 | 芯片 | 状态 |
|---|---|---|---|
| [mr2/](mr2/) | MounRiver Studio 2 工程（裸机 + TMOS + 预编译 BLE 栈） | **CH582F** | ✅ Active |

**MR2 命名**：`mr2/` 内是 MounRiver Studio 2 IDE 工程（`.wvproj` / `.cproject` /
`.mrs/`），使用 MounRiver RISC-V 工具链（`riscv-none-embed-gcc`）。整个目录
自包含（APP / LIB / StdPeriphDriver / RVMSIS / Startup / obj），可整体移动；
IDE 重新导入时选择该目录即可。

## 规划硬件

| 芯片 | 说明 |
|---|---|
| **CH592** | RISC-V，BLE 5.4，更低功耗/成本，引脚兼容 CH582。若其支持休眠态保持 USB，可解除 CH582 的"休眠与 USB 互斥"约束（见 [mr2/POWER.md](mr2/POWER.md) 与 [../REQUIREMENTS.md](../REQUIREMENTS.md) §7.8 T6.5） |

## 文档索引（均在 [mr2/](mr2/) 内）

| 文档 | 内容 |
|---|---|
| [mr2/HARDWARE.md](mr2/HARDWARE.md) | CH582F 硬件规格、引脚分配、USB 端点、硬件层约束 |
| [mr2/USER-MANUAL.md](mr2/USER-MANUAL.md) | AT 命令使用手册（命令/模式/参数/注意细节） |
| [mr2/DESIGN.md](mr2/DESIGN.md) | CH582 设计哲学、内存布局、BLE 回调注册、USB/低功耗互斥细节 |
| [mr2/FIELD-NOTES.md](mr2/FIELD-NOTES.md) | CH582 实战坑录（F1–F19，硬件问题与原因记录） |
| [mr2/POWER.md](mr2/POWER.md) | CH582 低功耗设计指南 |
