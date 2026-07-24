# CH582 工具链兼容性验证报告

> 结论：**CH582 CI 暂时搁置**。WCH 官方工具链（MounRiver "RISC-V Embedded GCC"）
> 无公开直链、受下载限制，CI 无法自动获取；标准 xPack/上游 GCC **无法正确构建当前源码**
> （会产生"构建成功但运行即崩"的固件）。CH582 固件构建继续走本地 `source env.sh` 流程。

验证日期：2025-07-24
验证对象：xPack riscv-none-elf-gcc **15.2.0-1**（上游 GCC 15.2，CI 友好、有直链）
对照组：WCH MounRiver GCC 8.2（`riscv-none-embed-gcc`，本地 `~/wch`）

## 三个阻塞问题（实测证据）

### 1. 致命且静默：`WCH-Interrupt-fast` 属性不被识别

上游 GCC 的 `interrupt` 属性只认 `user/supervisor/machine`。遇到
`WCH-Interrupt-fast` 时**只产生 4 条 warning，不报错**，然后把中断 handler
编译成普通函数。涉及 `usb_dev.c`、`hws_ir.c`、`CH58x_common.h` 的 `__INTERRUPT` 宏。

同一函数反汇编对照：

```
官方 WCH GCC 8.2:              xPack 15.2.0（上游）:
USB_IRQHandler:                USB_IRQHandler:
  auipc ra, ...                  auipc t1, ...
  jalr USB_DevTransProcess       jr   USB_DevTransProcess   <- 尾调用
  mret          <- 正确          (ret) <- 普通返回，中断现场不恢复！
```

青稞内核由硬件压栈（`startup_CH583.S` 向量表第 4 项魔法数 `0xF3F9BDA9` 开启 HPE），
handler 必须以 `mret` 返回。用 `ret` 意味着**第一次 USB 中断即崩溃**。
最危险的是 CI 构建"成功"、hex 正常产出，问题只有上板才暴露。

### 2. `-march=rv32imac` 在上游 GCC >= 13 不再隐含 `zicsr/zifencei`

编译直接报错（`CH58x_sys.h` 内联汇编）：

```
Error: unrecognized opcode `fence.i', extension `zifencei' required
Error: unrecognized opcode `csrw 0x800,a5', extension `zicsr' required
```

改为 `rv32imac_zicsr_zifencei` 可解决，但 **WCH GCC 8.2 不认识这些扩展名**
（扩展拆分发生在 GCC 12 / binutils 2.38 之后）——同一 march 字符串无法通吃两个工具链。

### 3. `-lprintf` 是 WCH 私有库

`libprintf.a` 只随 WCH 工具链分发，xPack 没有 -> 链接失败 `cannot find -lprintf`。
去掉后由 newlib nano 提供 printf，实测可链接（FLASH 仅 +800 B）。

## 非阻塞差异

| | WCH GCC 8.2 | WCH GCC 15.2 | xPack 15.2 |
|---|---|---|---|
| `WCH-Interrupt-fast` | OK | OK | 静默忽略（见问题 1） |
| FLASH (kbd) | 170764 B | 170188 B | 171560 B（绕过问题 2/3 后） |
| RAM 报告 | 68.15% | 67.61% | 100.00%（`.stack` 段被计入，观感问题） |
| 预编译库链接 | OK | OK | OK（`libCH58xBLE.a`/`libISP583.a` ABI 兼容） |
| nano/nosys specs | OK | OK | OK |

注：WCH GCC 12.2 / 15.2（`riscv-wch-elf-gcc` / `riscv32-wch-elf-gcc`）均实测可完整构建，
只是命令前缀与 makefile 硬编码的 `riscv-none-embed-` 不同，需改 makefile 或做别名。

## 若未来要启用 xPack CI（路线 B，已验证可行）

源码侧 3 处小改即可让 xPack 构建出正确固件：

1. **中断属性**：`__attribute__((interrupt("WCH-Interrupt-fast")))` 改为标准
   `__attribute__((interrupt))`（共 3 处）。已实测两种工具链都正确生成 `mret`。
   代价：硬件压栈之上再做一次全量软件保存，IRQ 延迟略增（60 MHz 下预计无感），
   **必须上板回归 USB 枚举和收发**。
2. **march**：makefile 按 GCC 版本注入 `_zicsr_zifencei`（>= 13 时）。
3. **-lprintf**：条件化或直接去掉（用 newlib nano printf）。

无论走哪条路线，CI 都应加两道加固（挡住"假成功"）：

```bash
# 构建日志 gate：出现中断属性 warning 或汇编 Error 即失败
grep -E "warning: argument to .interrupt.|Error:" build.log && exit 1
# 产物 smoke check：USB_IRQHandler 末尾必须是 mret
riscv-none-embed-objdump -d at-node.elf --disassemble=USB_IRQHandler | grep -q mret
```

## 当前决策（2025-07-24）

- CH582 CI **搁置**：官方工具链无直链无法自动获取；xPack 需要上述源码改动且需上板回归，非必要需求。
- `tools/ci/ci.sh` 中 CH582 构建改为 **opt-in**：仅当本机存在 WCH 工具链
  （`source env.sh` 后能找到 `riscv-none-embed-gcc`）时才构建，否则跳过并提示。
- CI 保留 ESP32 构建、Python 检查、文档检查。
