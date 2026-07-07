@echo off
chcp 65001 >nul
setlocal

echo =============================================
echo   MiniTextSearch — PyInstaller 打包脚本
echo =============================================
echo.

:: ── 切换到项目根目录 ──
cd /d "%~dp0.."

:: ── 1. 确保 C 引擎已编译 ──
if not exist "mini_search.exe" (
    echo [1/3] 编译 C 引擎...
    gcc -g -Wall main_proj/main.c ^
        dll_src/text_io/text_io.c ^
        dll_src/index_build/index_build.c ^
        dll_src/query_handle/query_handle.c ^
        dll_src/rank_score/rank_score.c ^
        dll_src/result_out/result_out.c ^
        -o mini_search.exe -lm
    if %ERRORLEVEL% neq 0 (
        echo [错误] C 引擎编译失败！
        pause
        exit /b 1
    )
    echo [1/3] C 引擎编译完成
) else (
    echo [1/3] C 引擎已存在，跳过编译
)

:: ── 2. 检查 PyInstaller ──
echo [2/3] 检查 PyInstaller...
pip show pyinstaller >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo         正在安装 PyInstaller...
    pip install pyinstaller
)

:: ── 3. 打包 ──
echo [3/3] 开始打包...
echo.
pyinstaller --clean --noconfirm "MiniTextSearch.spec"

if %ERRORLEVEL% equ 0 (
    echo.
    echo =============================================
    echo   打包成功！
    echo   输出文件: dist\MiniTextSearch.exe
    echo =============================================
) else (
    echo.
    echo [错误] 打包失败，请检查上方输出
)

endlocal
pause
