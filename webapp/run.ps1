# Arranca la web en http://127.0.0.1:5000
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not (Test-Path '.\.venv\Scripts\python.exe')) {
    Write-Host 'Falta el entorno virtual. Ejecuta primero: .\install.ps1' -ForegroundColor Yellow
    exit 1
}

& .\.venv\Scripts\python.exe app.py
