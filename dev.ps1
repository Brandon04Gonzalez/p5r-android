# Development environment for SwitchGamesRecomp (Windows).
# Usage, in PowerShell:  . .\dev.ps1
$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"  # vswhere, which VsDevCmd calls
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
# clang, cmake and ninja from Visual Studio ahead of MinGW
$cm = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake"
$env:PATH = "$vs\VC\Tools\Llvm\x64\bin;$cm\CMake\bin;$cm\Ninja;$env:PATH"
$env:PATH = "$PSScriptRoot\private\tools\nstool;$env:PATH"
# Vulkan SDK: headers, vk.xml (tools/gen_vk.py), validation layer (NX_VK_VALIDATION=1)
if (-not $env:VULKAN_SDK) { $env:VULKAN_SDK = (Get-ChildItem C:\VulkanSDK -Directory | Sort-Object Name | Select-Object -Last 1).FullName }
$env:PATH = "$env:VULKAN_SDK\Bin;$env:PATH"
Set-Location $PSScriptRoot
Write-Host "SwitchGamesRecomp dev env: clang $((clang-cl --version)[0] -replace '.*version ','') | $(Get-Location)"
