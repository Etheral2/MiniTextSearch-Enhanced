# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec — 将 MiniTextSearch 打包为单个 EXE（带终端窗口）
用法：
    pyinstaller MiniTextSearch.spec
"""

a = Analysis(
    ['app.py'],
    pathex=[],
    binaries=[],
    datas=[
        ('mini_search.exe', '.'),
        ('index.html', '.'),
        ('stopwords.txt', '.'),
        ('docs_lib', 'docs_lib'),
    ],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='MiniTextSearch',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,          # 显示终端窗口
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=None,
)
