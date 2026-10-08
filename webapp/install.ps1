# Crea el entorno virtual e instala las dependencias de la web.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not (Test-Path '.venv')) {
    Write-Host '==> Creando entorno virtual (.venv)...'
    py -3 -m venv .venv
}

Write-Host '==> Actualizando pip...'
& .\.venv\Scripts\python.exe -m pip install --upgrade pip --quiet

Write-Host '==> Instalando dependencias (torch tarda un rato)...'
& .\.venv\Scripts\python.exe -m pip install -r requirements.txt

Write-Host ''
Write-Host 'Listo. Arranca la web con:  .\run.ps1' -ForegroundColor Green
