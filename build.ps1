# Build wrapper: activates TuyaOpen and layers sdkconfig.overlay over the
# platform defaults (ESP-IDF honours the SDKCONFIG_DEFAULTS env var).
#   .\build.ps1            build
#   .\build.ps1 clean      full clean (use after changing Kconfig/overlay)
#   .\build.ps1 flash COM5 build + flash
param([string]$Action = "build", [string]$Port = "COM5")

# No "Stop" here: PowerShell 5.1 turns native stderr (tos.py logs) into errors.
$Sdk = if ($env:OPEN_SDK_ROOT) { $env:OPEN_SDK_ROOT } else { "$HOME\TuyaOpen" }
$Project = $PSScriptRoot

if (-not $env:VIRTUAL_ENV) {
    Push-Location $Sdk
    . .\export.ps1 *> $null
    Pop-Location
}

$PlatformDefaults = Join-Path $Sdk "platform\ESP32\tuya_open_sdk\sdkconfig.defaults"
$env:SDKCONFIG_DEFAULTS = "$PlatformDefaults;$(Join-Path $Project 'sdkconfig.overlay')" -replace '\\', '/'

# tos.py flash (tyutool) drops the native USB-Serial/JTAG link mid-write on this
# board; esptool from the IDF env with compressed flash_args is reliable.
function Invoke-Flash {
    $IdfPython = Join-Path $Sdk "platform\ESP32\.espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe"
    Push-Location (Join-Path $Sdk "platform\ESP32\tuya_open_sdk\build")
    & $IdfPython -m esptool --chip esp32s3 -p $Port --before default_reset --after hard_reset write_flash "@flash_args"
    Pop-Location
}

Set-Location $Project
switch ($Action) {
    "clean" { tos.py clean -f }
    "build" { tos.py build }
    "flash" { tos.py build; if ($LASTEXITCODE -eq 0) { Invoke-Flash } }
    default { throw "unknown action: $Action" }
}
exit $LASTEXITCODE
