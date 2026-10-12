# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Set-ExecutionPolicy -Scope Process Bypass
# . scripts\env.ps1 [-Isolated]
# -> prepend bundled tools to PATH; CC=clang, CXX=clang++ for this shell.
# Requires bootstrap.ps1. -Isolated keeps only bundled tools, Git + Windows on PATH.
param([switch]$Isolated)

$repo = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $repo 'deps\tools'
# PATH order comes from bootstrap.ps1.
$list = Join-Path $tools 'path.txt'
if (-not (Test-Path $list)) {
    throw 'deps\tools\path.txt missing; run scripts\bootstrap.ps1'
}
$paths = @(Get-Content $list | Where-Object { $_ } | ForEach-Object { Join-Path $tools $_ })
if (-not (Test-Path (Join-Path $paths[0] 'clang.exe'))) {
    throw "Clang missing: $($paths[0])\clang.exe"
}
$env:CC = 'clang'; $env:CXX = 'clang++'

if ($Isolated) {
    $system = Join-Path $env:SystemRoot 'System32'
    $paths += @($system, $env:SystemRoot, (Join-Path $system 'WindowsPowerShell\v1.0'))
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) { $paths += Split-Path -Parent $git.Source }
    $env:PATH = $paths -join ';'
} else {
    $env:PATH = ($paths -join ';') + ';' + $env:PATH
}
Write-Host 'PATH: ScarletNX tools; CC=clang; CXX=clang++'
