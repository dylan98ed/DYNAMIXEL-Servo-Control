@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel%==0 (
  set PYTHON=py
) else (
  set PYTHON=python
)

echo ===========================================
echo   TP2 - Panel DYNAMIXEL H42 / Flask
 echo ===========================================
echo.
echo Instalando/verificando dependencias...
%PYTHON% -m pip install -r requirements.txt
if errorlevel 1 (
  echo.
  echo ERROR: No se pudieron instalar las dependencias.
  echo Revisar Python y la conexion a Internet.
  pause
  exit /b 1
)

echo.
echo IMPORTANTE: cerrar el Monitor Serie de Arduino.
echo Abrir la pagina http://127.0.0.1:5000
start "" "http://127.0.0.1:5000"
%PYTHON% app.py
pause
