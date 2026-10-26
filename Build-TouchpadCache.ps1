$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'bin') -Force | Out-Null
& (Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe') /nologo /optimize+ /platform:x64 /r:System.Web.Extensions.dll ('/out:'+(Join-Path $PSScriptRoot 'bin\TouchpadCache.exe')) (Join-Path $PSScriptRoot 'src\TouchpadCache.cs')
if ($LASTEXITCODE -ne 0) { throw 'Touchpad cache helper build failed.' }
