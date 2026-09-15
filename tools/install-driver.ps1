<#
.SYNOPSIS
    Установка драйвера косвенного дисплея PhoneMonitor IDD.

.DESCRIPTION
    Скрипт сам запрашивает повышение прав, поэтому запускать его можно из
    обычного окна PowerShell.

    Почему PowerShell, а не .cmd: cmd.exe читает batch-файл по байтовым
    смещениям, и команда chcp внутри скрипта с кириллицей сдвигает позицию
    чтения - команды начинают обрезаться на середине ("ho" вместо "echo").
    У PowerShell такой проблемы нет.

    Порядок действий:
      1. Повышение прав и проверка режима тестовой подписи.
      2. Импорт тестового сертификата в Root и TrustedPublisher. Без первого
         система не доверяет подписи, без второго установка молча отклоняется.
      3. Установка пакета через pnputil.
      4. Создание узла устройства через devgen: драйвер виртуальный, реального
         оборудования под ним нет, поэтому узел создаётся вручную.

    Весь вывод дублируется в файл build\driver-install.log.

.PARAMETER Remove
    Вместо установки удалить устройство и пакет драйвера.
#>

[CmdletBinding()]
param(
    [switch]$Remove
)

$ErrorActionPreference = "Continue"

# --- Самоповышение прав ------------------------------------------------------
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)

if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Требуются права администратора - перезапускаюсь с повышением..." -ForegroundColor Yellow
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit", "-File", "`"$PSCommandPath`"")
    if ($Remove) { $argList += "-Remove" }
    try {
        Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Verb RunAs
    } catch {
        Write-Host "Повышение прав отклонено." -ForegroundColor Red
    }
    return
}

# --- Пути --------------------------------------------------------------------
$repo = Split-Path -Parent $PSScriptRoot
$pkg  = Join-Path $repo "build\driver\Release"
$log  = Join-Path $repo "build\driver-install.log"

Start-Transcript -Path $log -Force | Out-Null

function Say {
    param([string]$Text, [string]$Status = "")
    switch ($Status) {
        "ok"   { Write-Host "[ОК]      $Text" -ForegroundColor Green }
        "err"  { Write-Host "[ОШИБКА]  $Text" -ForegroundColor Red }
        "warn" { Write-Host "[ВНИМАНИЕ] $Text" -ForegroundColor Yellow }
        default { Write-Host $Text }
    }
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  PhoneMonitor IDD - $(if ($Remove) { 'удаление' } else { 'установка' })" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Say "Права администратора получены." "ok"

# --- Поиск инструментов WDK --------------------------------------------------
$toolsRoot = "C:\Program Files (x86)\Windows Kits\10\Tools"
$devgen = Get-ChildItem $toolsRoot -Filter "devgen.exe" -Recurse -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -like "*\x64\*" } | Sort-Object FullName | Select-Object -Last 1
$devcon = Get-ChildItem $toolsRoot -Filter "devcon.exe" -Recurse -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -like "*\x64\*" } | Sort-Object FullName | Select-Object -Last 1

# =============================================================================
#  Режим удаления
# =============================================================================
if ($Remove) {
    # --- Шаг 1: узлы устройств ------------------------------------------------
    #
    # Удалять по hardware id ("Root\PhoneMonitorIdd") нельзя: devgen создаёт
    # узел с идентификатором вида SWD\DEVGEN\{GUID}, и по hardware id он не
    # находится. Поэтому перечисляем реальные экземпляры через WMI и сносим
    # каждый по его настоящему Device Instance ID.
    Say "Поиск узлов устройств PhoneMonitor..."
    $devices = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -like "*PhoneMonitor*" -or $_.DeviceID -like "*PHONEMONITORIDD*" }

    if ($devices) {
        foreach ($d in $devices) {
            Say "Удаляю узел $($d.DeviceID)"
            & pnputil /remove-device "$($d.DeviceID)"
            if ($LASTEXITCODE -ne 0) {
                Say "pnputil /remove-device вернул код $LASTEXITCODE" "warn"
            }
        }
    } else {
        Say "Узлов устройств PhoneMonitor не найдено."
    }

    # --- Шаг 2: пакеты драйверов ----------------------------------------------
    #
    # Оставшийся старый пакет - причина, по которой переустановка не помогает:
    # при равном DriverVer система продолжает использовать уже привязанный
    # пакет и новый попросту игнорирует. Поэтому сносим ВСЕ пакеты PhoneMonitor.
    Say "Поиск установленных пакетов PhoneMonitor..."
    $enum = (& pnputil /enum-drivers) -join "`n"

    # Разбиваем вывод на блоки по каждому пакету и оставляем наши.
    $blocks = $enum -split "(?m)^(?=\s*(?:Опубликованное имя|Published Name))" |
              Where-Object { $_ -match "PhoneMonitor" -or $_ -match "phonemonitor\.inf" }

    $removed = 0
    foreach ($b in $blocks) {
        if ($b -match "(oem\d+\.inf)") {
            $oem = $Matches[1]
            Say "Удаляю пакет $oem..."
            & pnputil /delete-driver $oem /uninstall /force
            if ($LASTEXITCODE -eq 0) { $removed++ }
            else { Say "pnputil /delete-driver вернул код $LASTEXITCODE" "warn" }
        }
    }

    if ($removed -eq 0) { Say "Пакетов PhoneMonitor не найдено или удалить не удалось." "warn" }
    else { Say "Удалено пакетов: $removed" "ok" }

    # --- Шаг 3: контроль -------------------------------------------------------
    Write-Host ""
    Write-Host "=== Осталось в системе ===" -ForegroundColor Cyan
    $left = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like "*PhoneMonitor*" }
    if ($left) { $left | ForEach-Object { "  {0}  [{1}]" -f $_.DeviceID, $_.Status } }
    else { Write-Host "  чисто, устройств PhoneMonitor нет" }

    Write-Host ""
    Say "Удаление завершено." "ok"
    Stop-Transcript | Out-Null
    return
}

# =============================================================================
#  Режим установки
# =============================================================================

# --- Шаг 1: режим тестовой подписи -------------------------------------------
$bcd = (& bcdedit /enum "{current}") -join "`n"
if ($bcd -match "(?im)^\s*testsigning\s+(Yes|Да)") {
    Say "Режим тестовой подписи включён." "ok"
} else {
    Say "Режим тестовой подписи, похоже, ВЫКЛЮЧЕН." "warn"
    Write-Host "         Драйвер с самоподписанным сертификатом система не загрузит."
    Write-Host "         Включается так:  bcdedit /set testsigning on   затем перезагрузка."
    Write-Host ""
    $answer = Read-Host "         Продолжить всё равно? (y/n)"
    if ($answer -notmatch "^[yYдД]") { Stop-Transcript | Out-Null; return }
}

# --- Шаг 2: файлы пакета ------------------------------------------------------
$required = @("PhoneMonitor.inf", "PhoneMonitorIdd.dll", "phonemonitor.cat", "PhoneMonitorTest.cer")
$missing = $required | Where-Object { -not (Test-Path (Join-Path $pkg $_)) }
if ($missing) {
    Say "В $pkg не хватает файлов: $($missing -join ', ')" "err"
    Write-Host "         Соберите драйвер и подпишите пакет заново."
    Stop-Transcript | Out-Null
    return
}
Say "Файлы пакета на месте ($pkg)." "ok"

# --- Шаг 3: импорт сертификата -------------------------------------------------
Write-Host ""
Say "Импорт тестового сертификата..."
$cer = Join-Path $pkg "PhoneMonitorTest.cer"
foreach ($store in @("Root", "TrustedPublisher")) {
    $out = & certutil -addstore -f $store $cer 2>&1
    if ($LASTEXITCODE -eq 0) {
        Say "Сертификат добавлен в хранилище $store." "ok"
    } else {
        Say "Не удалось добавить сертификат в $store (код $LASTEXITCODE)." "err"
        $out | Select-Object -Last 3 | ForEach-Object { "         $_" }
    }
}

# --- Шаг 4: установка пакета драйвера -------------------------------------------
Write-Host ""
Say "Установка пакета драйвера через pnputil..."
$inf = Join-Path $pkg "PhoneMonitor.inf"
& pnputil /add-driver $inf /install
$pnpCode = $LASTEXITCODE
if ($pnpCode -eq 0) {
    Say "Пакет установлен." "ok"
} else {
    Say "pnputil вернул код $pnpCode." "err"
    Write-Host "         Подробности: $env:SystemRoot\INF\setupapi.dev.log"
}

# --- Шаг 5: создание узла устройства ---------------------------------------------
Write-Host ""
Say "Создание узла устройства Root\PhoneMonitorIdd..."
if ($devgen) {
    & $devgen.FullName /add /hardwareid "Root\PhoneMonitorIdd"
    Say "devgen отработал (код $LASTEXITCODE)."
} elseif ($devcon) {
    & $devcon.FullName install $inf "Root\PhoneMonitorIdd"
    Say "devcon отработал (код $LASTEXITCODE)."
} else {
    Say "Не найдены ни devgen.exe, ни devcon.exe в $toolsRoot" "err"
}

# --- Шаг 6: проверка результата ----------------------------------------------------
Write-Host ""
Write-Host "=== Что видно в системе ===" -ForegroundColor Cyan
$dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
       Where-Object { $_.Name -like "*PhoneMonitor*" -or $_.DeviceID -like "*PHONEMONITORIDD*" }
if ($dev) {
    foreach ($d in $dev) {
        Write-Host ("  Имя:      {0}" -f $d.Name)
        Write-Host ("  ID:       {0}" -f $d.DeviceID)
        Write-Host ("  Статус:   {0}" -f $d.Status)
        Write-Host ("  Проблема: {0}" -f $d.ConfigManagerErrorCode)
    }
} else {
    Say "Устройство PhoneMonitor в системе не обнаружено." "warn"
}

Write-Host ""
Write-Host "=== Мониторы ===" -ForegroundColor Cyan
Get-CimInstance Win32_DesktopMonitor -ErrorAction SilentlyContinue |
    ForEach-Object { "  {0} ({1})" -f $_.Name, $_.DeviceID }

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Готово. Журнал: $log" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Если устройство появилось, но со статусом ошибки - смотрите журнал"
Write-Host "драйвера в DebugView (от администратора, Capture Global Win32),"
Write-Host "фильтр [PhoneMonitor]."
Write-Host ""

Stop-Transcript | Out-Null
