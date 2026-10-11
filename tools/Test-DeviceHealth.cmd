@echo off
cd /d "%~dp0.."
if not exist build\devicehealth-test-obj mkdir build\devicehealth-test-obj
cl /nologo /EHsc /std:c++20 /O2 /W4 /WX tools\DeviceHealthRegression.cpp /Febuild\Release\DeviceHealthRegression.exe /Fobuild\devicehealth-test-obj\
if errorlevel 1 exit /b 1
build\Release\DeviceHealthRegression.exe
