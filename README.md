# EOS-1V Film Recorder

用于导入、整理和浏览 Canon EOS-1V 拍摄数据的本地应用项目。

## 项目目标

- 从相机、连接器或导出文件导入拍摄记录
- 保留原始数据，并记录导入来源和时间
- 解析机身、胶卷、镜头、曝光和拍摄时间等信息
- 支持按胶卷、日期、镜头和参数筛选、排序与浏览
- 对重复导入、异常记录和缺失字段提供明确提示
- 支持将整理后的数据导出为通用格式

## 建议的首个里程碑

1. 收集一份真实的 EOS-1V 原始数据样本，并记录获取方式。
2. 定义原始数据到标准记录的字段映射。
3. 实现只读解析器和 JSON/CSV 导出。
4. 建立本地数据存储及重复检测。
5. 实现胶卷列表、单卷详情和单帧详情浏览。

## 数据处理原则

- 原始导入文件不可修改，解析结果与原始数据分开保存。
- 每次导入记录来源、时间、文件摘要和解析器版本。
- 未知字段原样保留，避免因早期理解不完整而丢失数据。
- 时间、曝光补偿等容易产生歧义的字段同时保留原始值与规范化值。

## 目录规划

```text
docs/       需求、数据格式与设计文档
fixtures/   可公开且已脱敏的测试样本
src/        后续实现代码
tests/      自动化测试
```

## 协议字段表

- [E3/E4/EFD 胶卷记录字段表](docs/e3-e4-efd-field-map.md)
- [数据模型](docs/data-model.md)
- [测试夹具说明](fixtures/README.md)

## CLI 下载器

本项目包含 Windows x64 命令行下载器，通过 `external/open1V-cli` Git 子模块
引用 `open1v-core.vcxproj`，复用其 UNO R4 串口/WinUSB 传输层和
EOS-1V 会话状态机。

克隆时请同时取得子模块：

```powershell
git clone --recurse-submodules https://github.com/MitsuhaQuQ/open1v-filmdb.git
```

```powershell
.\build.cmd
.\x64\Release\film-record.exe self-test
.\x64\Release\film-record.exe download
.\x64\Release\film-record.exe download --port COM3 --format csv --output .\exports\film-records.csv
```

默认写入 `film-record.exe` 同级目录的 `film-records.sqlite3`，不受启动时当前目录影响。
重复运行会在同一数据库中新建导入批次，
不会覆盖之前下载的数据。也可通过 `--output` 指定数据库位置，或用
`--format json|csv` 进行文件导出。

SQLite 数据库包含 `imports`、`rolls`、`frames` 三张表。帧表按照协议字段表保存
已解码字段；C.Fn 的 19 个选项保存在 `cfn_values`，格式为由逗号分隔的 19 个数字。
传输原始值不拆成逐字段列：每卷只用 `raw_e3`、每帧只用 `raw_e4` 保存完整原文。

已确认字段保存便于浏览的解码值，包括 DX/手动 ISO、
焦距、最大/实际光圈、快门时间、曝光补偿、闪光补偿、闪光模式、测光模式、
拍摄模式、过片模式、AF 模式、多重曝光、拍摄时间、C.Fn、对焦点选择、合焦点
原始位域和电池装入时间。mask 未启用的字段保存为 `NULL`；未确认的枚举显示为
`Unknown(0xNN)`，不映射成相邻选项。

相机需要处于 PC 模式。下载是只读操作；成功或失败时都会尝试发送会话退出序列。
JSON 会保留完整 E3/E4 原始包、字段 mask 与 record width。只有已经确认的默认
32 字节布局会展开字段，未知或缩短布局不会被猜测解析。
