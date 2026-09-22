# EOS-1V film-record field map

这份表把目前通过 EOS-1V 实机通信、原版 Memory 导出文件（EFD）和旧数据交叉核对得到的结果集中在一起。它描述的是下载记录的解析层，不改变相机通信状态机。

## 结论标记

| 标记 | 含义 |
|---|---|
| **已确认** | 已由实机返回包和/或 EFD 可重复核对 |
| **离线确认** | 在已有 EFD 中稳定出现，但尚未由新的原始 E4 包单独验证 |
| **部分确认** | 主含义已知，仍有标志位或边界值未解释 |
| **保留** | 必须原样保存，不能按猜测转换 |

## 下载状态机

| 顺序 | 包 | 作用 | 解析规则 |
|---:|---|---|---|
| 1 | `E1 02 00 nn nn` | 胶卷分段数 | `nn` 为当前相机报告的卷段数；0 时不发送 E3/E4 |
| 2 | `E3 21 ...` | 一卷的 header | 36 字节；包含本卷字段宽度、mask、胶卷 ID、DX ISO、装卷时间 |
| 3 | `E4 xx ...` | 一帧记录 | 长度由 E3 的记录宽度决定；每卷结束为 `E4 01 00 00` |
| 4 | `E3 01 00 00` | 所有卷结束 | 收到后下载完成 |

`E3/E4` 均采用命令、长度、数据、校验和结构。校验和是从命令开始按协议累加的低 8 位。解析器必须按长度切包，并保留可能尾随的异步 `F4`，不能以固定 36 字节读取所有 E4。

## E3 header（36 字节）

下表的 offset 从 E3 命令后的 data[0] 开始；未知字段仍须写入 raw packet。

| data offset | 长度 | 含义 | 状态 |
|---:|---:|---|---|
| 0–2 | 3 | 相机内部卷序/状态 | 保留 |
| 4–6 | 3 | Film ID，BCD；例如 `00 03 29` → `00-329` | **已确认** |
| 8 | 1 | 记录宽度：`20`=32 字节，`10`=16 字节，`08`=8 字节 | **已确认** |
| 9–16 | 8 | Shooting-field mask，决定 E4 字段顺序 | **已确认** |
| 18 | 1 | DX ISO wire；`48`=ISO 100 示例，`F0`=无有效 DX/使用手动 ISO | **已确认** |
| 19–24 | 6 | 装卷时间：BCD `YY MM DD hh mm ss` | **已确认** |
| 3、7、17、25–32 | — | 内部状态、保留或相机扩展 | **保留** |

日期年份应按相机/应用的世纪规则解释；未知 BCD 不应静默变成有效日期。

## E4 长度与动态布局

| E4 length byte | 总包长 | 内部记录宽度 | 说明 |
|---:|---:|---:|---|
| `21` | 36 | 32 | 默认完整记录 |
| `11` | 20 | 16 | 临时缩短字段测试 |
| `09` | 12 | 8 | 临时最小字段测试 |

E4 的字段不是固定绝对偏移：先读取 E3 mask，再按协议规定的选择位顺序消费字段。实现应保存 `mask`、`record_width`、完整 `raw_e4` 和解析结果。mask 改变时禁止沿用 32 字节布局。

## E4 默认完整 mask 的字段表

以下 offset 只适用于已验证的完整 mask `FF FF 0C 3F 00 08 7F 00`。动态记录必须使用 mask 驱动的游标。

| data offset | 字段 | 线值/转换 | 状态 |
|---:|---|---|---|
| 0 | 记录标志 | 常见 `01` | 部分确认 |
| 1 | 记录格式/状态 | 常见 `81` | 保留 |
| 2 | Frame No. | 无符号帧号 | **已确认** |
| 3–4 | Focal length | 大端毫米，如 `00 69`=105 mm | **已确认** |
| 5 | Max aperture | 相机 aperture wire code | **已确认** |
| 6 | Tv | 快门 wire code；Bulb 使用独立语义 | 部分确认 |
| 7 | Av | 实际光圈 wire code | **已确认** |
| 8 | ISO source | DX/手动 ISO 上下文，不能单独当作 ISO(M) | 部分确认 |
| 9 | Exposure compensation | `FB`=-0.7、`08`=+1.0、`FD`=-0.3、`03`=+0.3 等 | **已确认** |
| 10 | Flash exposure compensation | `FD`=-0.3、`F8`=-1.0、`00`=0 等 | **已确认** |
| 11 | Flash mode | `02`=OFF、`48`=Manual（含 Multi）、`49`=E-TTL | 部分确认 |
| 12 | Metering mode + flags | `20` Evaluative，`10` Center avg，`80` Spot，`40` Partial | 部分确认 |
| 13 | Shooting mode | `40` Av、`80` Manual、`10` Program、`04` Bulb、`20` Tv | 部分确认 |
| 14 | Film advance | `08` single，`10` 2-sec，`20` 10-sec，`40` body continuous，`01` low，`02` high | 部分确认 |
| 15 | AF mode + flags | `02/42` One-Shot，`04` AI Servo，`14` Manual focus | 部分确认 |
| 16 | Multiple exposure | `00` 通常关闭；重复曝光组中可见 `80` 标志 | 部分确认 |
| 17–18 | 其他选择字段/保留 | 由 mask 决定 | 保留 |
| 19–24 | Capture time | BCD `YY MM DD hh mm ss` | **已确认** |
| 25–32 | 扩展字段 | C.Fn snapshot、对焦点、电池装载时间等可能由 mask 插入 | 部分确认 |

## EFD（Memory 文件）映射

EFDF 头为 512 字节；每个 EFRM 帧块通常也是 512 字节。以下 offset 均为 EFRM 起点的十六进制偏移。

| Offset | 长度 | Memory 字段 | 转换 |
|---:|---:|---|---|
| `00` | 4 | magic | `EFRM` |
| `18` | 4 | Frame No. | little-endian |
| `1C` | 4 | Focal length | mm，little-endian |
| `20` | 4 | Max aperture | f-number ×100 |
| `24` | 4 | Tv | 负的分母 ×100；`-1` 仅在 Bulb 且 Tv 有效时表示 Bulb |
| `28` | 4 | Av | f-number ×100 |
| `2C` | 4 | ISO (M) | 仅 E3 DX=`F0` 时有效 |
| `30` | 4 | Exposure compensation | EV ×100 |
| `34` | 4 | Flash exposure compensation | EV ×100 |
| `38` | 2 | Capture year | little-endian |
| `3A–3E` | 5 | Capture month/day/hour/min/sec | 单字节 |
| `40` | 4 | Flash mode | 0 OFF，1 ON，10 Manual，11 TTL，12 A-TTL，13 E-TTL |
| `44` | 4 | Film advance | 10 single，11 continuous，12 low，20 high，21 ultra-high，22 2-sec，23 10-sec |
| `48` | 4 | Multiple exposure | 0 off；非 0 表示重复曝光组 |
| `4C` | 4 | Focus-point related | 旧数据常见 99，语义未完全确定 |
| `50` | 4 | Focus-point selection | 旧数据常见 -1，语义未完全确定 |
| `54` | 4 | Metering | 0 Evaluative，1 Center，2 Spot，3 Partial |
| `58` | 4 | Shooting mode | 0 Manual，1 Program，2 Tv，3 Av，4 Depth-of-field，5 Bulb |
| `5C` | 4 | AF mode | 0 Manual，1 One-Shot，2 AI Servo |
| `64+` | — | C.Fn 展开数据 | 保留 raw；完整子字段布局尚未固定 |
| `8B` | 6+ | 电池/胶卷装载时间 | 动态 E4 样本已核对；保存原始字节 |

EFDF 头中 `0x2A` 年、`0x2C–0x30` 日期时间、`0x32` 记录数属于软件索引信息；`0x26` 是软件生成的本地卷/文件序号，不能当作相机 Film ID。

## 已知 wire 值与 EFD 枚举

| 项目 | 已确认值 | 未完成部分 |
|---|---|---|
| Flash | OFF、Manual（Multi 也显示 Manual）、E-TTL | ON、TTL、A-TTL、旧闪光模式的 E4 wire 值 |
| Metering | Evaluative、Center-weighted、Spot、Partial | 高位 flag |
| Shooting | Manual、Program、Tv、Av、Bulb | Depth-of-field 的原始 wire 值 |
| Advance | Single、2-sec、10-sec、body continuous、low/high continuous | Ultra-high 的原始 wire 值 |
| AF | Manual focus、One-Shot、AI Servo | 附加 flag 位 |
| Multi exposure | OFF；重复组标志和组内回填已在 EFD 观察到 | 相机内部各 flag 的完整定义 |

## 动态解析算法

1. 读取并校验 E3，保存 `film_header_raw`、`record_width`、`mask`。
2. 将 mask 转成字段 token 列表；按 token 顺序从 E4 data 游标消费对应长度。
3. 对每个 token 只在长度足够且 BCD/校验有效时生成 typed value，同时保留原始切片和 wire 值。
4. 遇到 `E4 01 00 00` 结束当前卷；遇到 `E3 01 00 00` 结束全部卷。
5. 任何短包、非法校验、非法 BCD 都标记为 partial/error，不能补零伪造记录；重新读取成功时按原始卷顺序追加，并由上层决定去重。

## 当前完整度

| 层 | 状态 | 说明 |
|---|---|---|
| 会话与 E1/E3/E4/E3-end | **完成** | 已连续读取 0 卷、2 卷、6 卷、8 卷、10 卷、11 卷等样本 |
| 默认 32-byte E4 可见字段 | **基本完成** | 足以生成 Memory 表格 |
| 16/8-byte 动态 E4 | **协议完成，语义部分** | 长度和 mask 已验证，所有压缩字段仍需按 mask 解析 |
| EFD 可见字段转换 | **基本完成** | 未知字段必须 raw-preserve |
| C.Fn snapshot 展开 | **部分完成** | 已识别 19 项快照和辅助字节，EFD 全部内部偏移未固定 |
| 非默认闪光/推进/景深值 | **待补样本** | 目前只应显示 raw wire 或 Unknown |

## 实现约束

- 不以“当前通常是 13312 字节”作为文件或卷长度假设。
- 不因 DX 胶卷常见 36 张而限制记录数；非 DX、长卷或相机状态可能产生其他数量。
- 保留每个包的 raw bytes、mask、record width、校验结果和解析器版本。
- EFD 中 Image path、Remarks 属于电脑端编辑字段；新生成记录可为空，不应从 E4 臆造。
- 对未确认枚举使用 `Unknown(0xNN)`，不要映射成相邻的已知选项。

## 证据来源

- `analysis/EFD与抓包对应分析.md`
- `analysis/新增胶卷记录与EFD对照分析_20260921.md`
- `analysis/DATAold_EFD统计分析_20260921.md`
- `USBconnctor/eos1v-uno-r4-interface/docs/胶卷拍摄数据记录字段设定协议_20260921.md`
- `USBconnctor/eos1v-uno-r4-interface/docs/communication-manual.zh-CN.md`
- `USBconnctor/eos1v-uno-r4-interface/docs/protocol-validation.zh-CN.md`
