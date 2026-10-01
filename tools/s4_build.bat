@echo off
rem S4 build env — replicates C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
set MSYSTEM=
set SHELL=
set IDF_TOOLS_PATH=C:\Espressif\tools
set IDF_COMPONENT_LOCAL_STORAGE_URL=file://C:\Espressif\tools
set IDF_PATH=D:\.espressif\v6.1\esp-idf
set ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011\
set OPENOCD_SCRIPTS=C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20260703\openocd-esp32\share\openocd\scripts
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v6.1\venv
set IDF_CCACHE_ENABLE=1
set ESP_CLANG_LIBS_PATH=C:\Espressif\tools\esp-clang-libs\esp-21.1.3_20260408/esp-clang/lib
set ESP_IDF_VERSION=6.1
set PATH=C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64;C:\Espressif\tools\esp-clang\esp-21.1.3_20260408\esp-clang\bin;C:\Espressif\tools\esp-clangd\esp-21.1.3_20260408\esp-clangd\bin;C:\Espressif\tools\esp-rom-elfs\20241011\;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;C:\Espressif\tools\idf-exe\1.0.3\;C:\Espressif\tools\ninja\1.12.1\;C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20260703\openocd-esp32\bin;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\xtensa-esp-elf\bin;C:\Espressif\tools\python\v6.1\venv\Scripts;%PATH%
cd /d D:\DeskTop\WorkSpace\Code\esp32\esp32_ai_roboot\ai_roboot
python %IDF_PATH%\tools\idf.py %1
