@echo off
rem 烧录 assets FAT 分区 (台词/音效 WAV 包)
rem 前置: 插板; 镜像由 tools\gen_tts_wavs.py fat 重新生成
set MSYSTEM=
set IDF_TOOLS_PATH=C:\Espressif\tools
set IDF_PATH=D:\.espressif\v6.1\esp-idf
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v6.1\venv
set ESP_IDF_VERSION=6.1
set PATH=%IDF_PYTHON_ENV_PATH%\Scripts;C:\Espressif\tools\ninja\1.12.1\;C:\Espressif\tools\cmake\4.0.3\bin;%PATH%
cd /d D:\DeskTop\WorkSpace\Code\esp32\esp32_ai_roboot\ai_roboot
python %IDF_PATH%\components\partition_table\parttool.py -p COM3 write_partition --partition-name assets --input tools\assets_fat.img
