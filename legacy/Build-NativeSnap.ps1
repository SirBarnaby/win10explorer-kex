param([string]$Compiler = 'C:\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$patchRoot = $PSScriptRoot
$sourceRoot = Join-Path $patchRoot 'src'
$binaryRoot = Join-Path $patchRoot 'bin'
New-Item -ItemType Directory -Path $binaryRoot -Force | Out-Null
$resourceCompiler = Join-Path (Split-Path $Compiler) 'windres.exe'
Push-Location -LiteralPath $sourceRoot
try {
    & $resourceCompiler 'NativeSnap.rc' '-O' 'coff' '-o' (Join-Path $binaryRoot 'NativeSnap.res')
    if ($LASTEXITCODE -ne 0) { throw 'Native Snap resource compilation failed.' }
    & $Compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-Wno-missing-field-initializers' '-specs=NativeSnap.specs' '-municode' '-mwindows' '-static' '-static-libgcc' '-static-libstdc++' 'NativeSnap.cpp' (Join-Path $binaryRoot 'NativeSnap.res') '-o' (Join-Path $binaryRoot 'NativeSnap.exe') '-ldwmapi' '-lshell32' '-luser32' '-lgdi32' '-Wl,--dynamicbase,--nxcompat,--high-entropy-va'
    if ($LASTEXITCODE -ne 0) { throw 'Native Snap compilation failed.' }
} finally { Pop-Location }
Write-Output (Join-Path $binaryRoot 'NativeSnap.exe')
