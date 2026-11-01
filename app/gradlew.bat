@echo off
REM Gradle wrapper batch script

set GRADLE_HOME=C:\gradle
if exist "%GRADLE_HOME%\bin\gradle.bat" (
    "%GRADLE_HOME%\bin\gradle.bat" %*
) else (
    echo Gradle not found. Please install Gradle or set GRADLE_HOME.
    exit /b 1
)
