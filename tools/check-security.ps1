<#
.SYNOPSIS
    Проверка состояния защиты системы после работ с драйвером.

.DESCRIPTION
    Разработка драйвера потребовала отключить Secure Boot и включить тестовый
    режим подписи. Это снижает защиту машины, и важно понимать, в каком
    состоянии система находится сейчас.

    Скрипт ничего не меняет - только собирает факты и пишет их в файл
    build\security-status.log.

    Проверяются:
      1. BitLocker на всех томах: включена ли защита и не приостановлена ли она.
         Приостановленная защита - это незашифрованный по факту ключ на диске.
      2. Secure Boot.
      3. Режим тестовой подписи драйверов.
      4. Установленные пакеты PhoneMonitor и тестовый сертификат.
#>

[CmdletBinding()]
param()

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Требуются права администратора - перезапускаюсь с повышением..." -ForegroundColor Yellow
    Start-Process powershell.exe -ArgumentList @(
        "-NoProfile","-ExecutionPolicy","Bypass","-NoExit","-File","`"$PSCommandPath`""
    ) -Verb RunAs
    return
}

$repo = Split-Path -Parent $PSScriptRoot
$log = Join-Path $repo "build\security-status.log"
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
Start-Transcript -Path $log -Force | Out-Null

function Head($t) { Write-Host ""; Write-Host "=== $t ===" -ForegroundColor Cyan }

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Состояние защиты системы" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan

# --- 1. BitLocker ------------------------------------------------------------
Head "BitLocker"
$risky = $false
try {
    $vols = Get-BitLockerVolume -ErrorAction Stop
    foreach ($v in $vols) {
        $prot = if ($v.ProtectionStatus -eq 'On') { 'ВКЛЮЧЕНА' } else { 'ВЫКЛЮЧЕНА' }
        Write-Host ("  Том {0,-4} шифрование: {1,-14} защита: {2,-10} {3}% " -f `
            $v.MountPoint, $v.VolumeStatus, $prot, $v.EncryptionPercentage)

        # Ключевой момент: если защита выключена при зашифрованном томе,
        # значит она приостановлена и ключ лежит на диске открытым.
        if ($v.VolumeStatus -ne 'FullyDecrypted' -and $v.ProtectionStatus -ne 'On') {
            Write-Host "         ВНИМАНИЕ: том зашифрован, но защита приостановлена!" -ForegroundColor Red
            Write-Host "         Возобновить: manage-bde -protectors -enable $($v.MountPoint)" -ForegroundColor Yellow
            $risky = $true
        }

        $rp = $v.KeyProtector | Where-Object { $_.KeyProtectorType -eq 'RecoveryPassword' }
        if ($rp) { Write-Host ("         Ключ восстановления есть (ID {0})" -f $rp[0].KeyProtectorId) }
        else     { Write-Host "         Ключа восстановления НЕТ" -ForegroundColor Yellow }
    }
} catch {
    Write-Host "  Не удалось опросить BitLocker: $($_.Exception.Message)" -ForegroundColor Yellow
    & manage-bde -status
}

# --- 2. Secure Boot ----------------------------------------------------------
Head "Secure Boot"
try {
    $sb = Confirm-SecureBootUEFI
    if ($sb) { Write-Host "  ВКЛЮЧЁН" -ForegroundColor Green }
    else {
        Write-Host "  ВЫКЛЮЧЕН" -ForegroundColor Yellow
        Write-Host "  Машина не проверяет подпись загрузчика. Это было нужно для"
        Write-Host "  тестового режима подписи драйверов."
    }
} catch { Write-Host "  определить не удалось: $($_.Exception.Message)" }

# --- 3. Тестовая подпись -----------------------------------------------------
Head "Режим тестовой подписи драйверов"
$bcd = (& bcdedit /enum "{current}") -join "`n"
if ($bcd -match "(?im)^\s*testsigning\s+(Yes|Да)") {
    Write-Host "  ВКЛЮЧЁН" -ForegroundColor Yellow
    Write-Host "  Система загружает драйверы с самоподписанными сертификатами."
    Write-Host "  Отключить: bcdedit /set testsigning off   + перезагрузка"
} else {
    Write-Host "  выключен" -ForegroundColor Green
}

# --- 4. Наш драйвер и сертификат ---------------------------------------------
Head "Компоненты PhoneMonitor"
$dev = Get-CimInstance Win32_PnPEntity -EA SilentlyContinue | Where-Object { $_.Name -like "*PhoneMonitor*" }
if ($dev) { $dev | ForEach-Object { "  устройство: {0}  [{1}]" -f $_.DeviceID, $_.Status } }
else { Write-Host "  устройств PhoneMonitor нет" }

$certRoot = Get-ChildItem Cert:\LocalMachine\Root -EA SilentlyContinue | Where-Object { $_.Subject -like "*PhoneMonitor*" }
$certPub  = Get-ChildItem Cert:\LocalMachine\TrustedPublisher -EA SilentlyContinue | Where-Object { $_.Subject -like "*PhoneMonitor*" }
Write-Host ("  тестовый сертификат в Root:            {0}" -f $(if ($certRoot) { "есть" } else { "нет" }))
Write-Host ("  тестовый сертификат в TrustedPublisher: {0}" -f $(if ($certPub) { "есть" } else { "нет" }))

# --- Итог --------------------------------------------------------------------
Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
if ($risky) {
    Write-Host "  ЕСТЬ РИСК: защита BitLocker приостановлена" -ForegroundColor Red
} else {
    Write-Host "  Критичных проблем с BitLocker не найдено" -ForegroundColor Green
}
Write-Host "  Отчёт: $log" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

Stop-Transcript | Out-Null
