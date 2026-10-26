[CmdletBinding()]
param([string]$BuildDirectory=(Join-Path $PSScriptRoot 'build-native-verified'))
$ErrorActionPreference='Stop'
$cmake=(Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake='C:\mingw64\bin\cmake.exe' }
if (-not (Test-Path -LiteralPath $cmake)) { throw 'CMake 3.24+ and Visual Studio 2022 C++ Build Tools are required.' }
& $cmake -S (Join-Path $PSScriptRoot 'native') -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw 'Native configuration failed.' }
& $cmake --build $BuildDirectory --config Release --parallel 1
if ($LASTEXITCODE) { throw 'Native build failed.' }
Write-Output (Join-Path $BuildDirectory 'Release')
