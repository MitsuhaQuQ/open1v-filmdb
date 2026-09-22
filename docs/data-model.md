# 数据模型草案

模型会随着真实样本调整。任何解析结果都应能追溯到原始导入数据。

## Import（导入批次）

- `id`
- `source_type`：相机、连接器、文件或手工录入
- `source_name`
- `imported_at`
- `content_hash`
- `parser_version`
- `raw_data_path`
- `warnings`

## Camera（机身）

- `id`
- `model`
- `serial_number`
- `custom_functions`

## Roll（胶卷）

- `id`
- `camera_id`
- `import_id`
- `roll_number`
- `loaded_at`
- `unloaded_at`
- `film_name`
- `film_speed_iso`
- `frame_count`
- `notes`

## Frame（单帧）

- `id`
- `roll_id`
- `frame_number`
- `captured_at`
- `lens`
- `focal_length_mm`
- `aperture`
- `shutter_speed`
- `exposure_mode`
- `metering_mode`
- `exposure_compensation_ev`
- `flash_fired`
- `multiple_exposure`
- `raw_fields`：暂未识别或需原样保留的字段
- `notes`

## 去重建议

优先使用原始数据内容摘要识别重复导入；记录级去重可组合机身序列号、胶卷编号、帧号与拍摄时间，但不能假设所有字段始终存在。

