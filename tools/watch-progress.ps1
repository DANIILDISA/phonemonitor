<#
.SYNOPSIS
    Примитивный универсальный индикатор прогресса для долгих операций.

.DESCRIPTION
    Проблема, которую решает скрипт: установщики и сборщики (winget, Gradle,
    sdkmanager, MSBuild) часто работают молча по 10-30 минут, и отличить
    работающий процесс от зависшего невозможно. Скрипт измеряет РОСТ каталога
    во времени и показывает скорость, объём и оценку оставшегося времени.

    Работает с чем угодно, что пишет на диск: не знает ни про Gradle, ни про
    winget, просто следит за размером папки.

.PARAMETER Path
    Каталог (или несколько), за размером которого следим.

.PARAMETER Preset
    Готовая цель вместо -Path:
        gradle   - кэш зависимостей Gradle (~/.gradle)
        android  - портативный Android SDK (D:\android-sdk)
        apk      - сборка Android-клиента
        build    - сборка Windows-компонентов
        all      - всё сразу

.PARAMETER ExpectedMB
    Ожидаемый итоговый размер в МБ. Если задан, считается процент и остаток
    времени. Если не задан, показываются только объём и скорость.

.PARAMETER IntervalSec
    Период обновления, секунды. По умолчанию 2.

.PARAMETER Process
    Имена процессов, за которыми следить дополнительно (без .exe).
    Если все они исчезли, скрипт завершается - операция закончилась.

.EXAMPLE
    .\tools\watch-progress.ps1 -Preset gradle -ExpectedMB 1200
    Следить за скачиванием зависимостей Gradle, ожидая примерно 1,2 ГБ.

.EXAMPLE
    .\tools\watch-progress.ps1 -Path "D:\BuildTools" -Process vs_installer
    Следить за установкой Visual Studio и выйти, когда установщик закроется.
#>

[CmdletBinding()]
param(
    [string[]]$Path,
    [ValidateSet("gradle", "android", "apk", "build", "all", "")]
    [string]$Preset = "",
    [double]$ExpectedMB = 0,
    [int]$IntervalSec = 2,
    [string[]]$Process = @()
)

# --- Разрешение пресета в конкретные каталоги --------------------------------
$repo = Split-Path -Parent $PSScriptRoot

$presets = @{
    gradle  = @{ Paths = @("$env:USERPROFILE\.gradle");                  Procs = @("java") }
    android = @{ Paths = @("D:\android-sdk");                            Procs = @("java") }
    apk     = @{ Paths = @("$repo\android-client\app\build",
                           "$env:USERPROFILE\.gradle");                  Procs = @("java") }
    build   = @{ Paths = @("$repo\build");                               Procs = @("MSBuild", "cl") }
    all     = @{ Paths = @("$env:USERPROFILE\.gradle", "D:\android-sdk",
                           "$repo\build", "$repo\android-client\app\build");
                 Procs = @("java", "MSBuild", "cl") }
}

if ($Preset -and $presets.ContainsKey($Preset)) {
    if (-not $Path)    { $Path = $presets[$Preset].Paths }
    if (-not $Process) { $Process = $presets[$Preset].Procs }
}

if (-not $Path) {
    Write-Host "Не задано, за чем следить. Укажите -Path или -Preset." -ForegroundColor Red
    Write-Host "Доступные пресеты: gradle, android, apk, build, all"
    exit 1
}

# --- Измерение размера -------------------------------------------------------
function Get-TotalMB {
    param([string[]]$Targets)
    $sum = 0.0
    foreach ($t in $Targets) {
        if (Test-Path $t) {
            # SilentlyContinue: во время активной записи часть файлов может
            # быть недоступна, и это нормально - просто пропускаем их.
            $m = Get-ChildItem $t -Recurse -File -ErrorAction SilentlyContinue |
                 Measure-Object -Property Length -Sum
            if ($m.Sum) { $sum += $m.Sum / 1MB }
        }
    }
    return [math]::Round($sum, 1)
}

function Format-Duration {
    param([double]$Seconds)
    if ($Seconds -lt 0 -or [double]::IsInfinity($Seconds) -or [double]::IsNaN($Seconds)) { return "неизвестно" }
    if ($Seconds -lt 90)   { return ("{0:N0} с" -f $Seconds) }
    if ($Seconds -lt 5400) { return ("{0:N0} мин" -f ($Seconds / 60)) }
    return ("{0:N1} ч" -f ($Seconds / 3600))
}

# --- Шапка -------------------------------------------------------------------
Write-Host ""
Write-Host "Слежу за ростом:" -ForegroundColor Cyan
foreach ($p in $Path) {
    $mark = if (Test-Path $p) { "есть" } else { "нет пока" }
    Write-Host ("  {0}  [{1}]" -f $p, $mark)
}
if ($Process) { Write-Host ("Процессы: {0}" -f ($Process -join ", ")) }
if ($ExpectedMB -gt 0) { Write-Host ("Ожидаемый объём: {0:N0} МБ" -f $ExpectedMB) }
Write-Host "Ctrl+C - выход" -ForegroundColor DarkGray
Write-Host ""

# --- Основной цикл -----------------------------------------------------------
$startMB   = Get-TotalMB $Path
$startTime = Get-Date
$prevMB    = $startMB
$prevTime  = $startTime

# Скользящее среднее скорости: мгновенная скорость скачет слишком сильно,
# чтобы по ней что-то оценивать. Вес 0.3 даёт заметную реакцию на изменение
# при достаточном сглаживании.
$avgRate = 0.0
$idleTicks = 0

while ($true) {
    Start-Sleep -Seconds $IntervalSec

    $nowMB   = Get-TotalMB $Path
    $nowTime = Get-Date

    $deltaMB  = $nowMB - $prevMB
    $deltaSec = ($nowTime - $prevTime).TotalSeconds
    $rate     = if ($deltaSec -gt 0) { $deltaMB / $deltaSec } else { 0 }

    if ($avgRate -eq 0) { $avgRate = $rate } else { $avgRate = 0.3 * $rate + 0.7 * $avgRate }

    # Живы ли процессы
    $alive = @()
    foreach ($pn in $Process) {
        if (Get-Process -Name $pn -ErrorAction SilentlyContinue) { $alive += $pn }
    }

    # Полоса прогресса
    $barWidth = 34
    if ($ExpectedMB -gt 0) {
        $pct = [math]::Min(100, [math]::Max(0, ($nowMB / $ExpectedMB) * 100))
        $filled = [int]([math]::Round($barWidth * $pct / 100))
        $bar = ("#" * $filled) + ("." * ($barWidth - $filled))
        $remainMB = [math]::Max(0, $ExpectedMB - $nowMB)
        $eta = if ($avgRate -gt 0.01) { Format-Duration ($remainMB / $avgRate) } else { "неизвестно" }
        $line = "[{0}] {1,5:N1}%  {2,8:N0} / {3:N0} МБ  {4,6:N2} МБ/с  осталось ~{5}" -f `
                $bar, $pct, $nowMB, $ExpectedMB, $avgRate, $eta
    }
    else {
        # Размер неизвестен - показываем «бегущую» отметку и абсолютные цифры.
        $tick = [int]((Get-Date).Ticks / 10000000) % $barWidth
        $bar = ("." * $tick) + ">" + ("." * ($barWidth - $tick - 1))
        $grown = $nowMB - $startMB
        $line = "[{0}] {1,8:N0} МБ (+{2:N0} с начала)  {3,6:N2} МБ/с" -f `
                $bar, $nowMB, $grown, $avgRate
    }

    if ($alive.Count -gt 0) { $line += "  | активно: " + ($alive -join ",") }

    # \r возвращает каретку - строка перерисовывается на месте, а не сыплется вниз.
    Write-Host ("`r" + $line.PadRight(120)) -NoNewline

    # --- Условия выхода ------------------------------------------------------
    if ($ExpectedMB -gt 0 -and $nowMB -ge $ExpectedMB) {
        Write-Host ""
        Write-Host "Ожидаемый объём достигнут." -ForegroundColor Green
        break
    }

    if ($deltaMB -le 0.05) { $idleTicks++ } else { $idleTicks = 0 }

    # Процессы кончились и рост прекратился - операция завершена.
    if ($Process.Count -gt 0 -and $alive.Count -eq 0 -and $idleTicks -ge 2) {
        Write-Host ""
        Write-Host "Процессы завершились, рост прекратился - операция закончена." -ForegroundColor Green
        break
    }

    # Долгий простой при живом процессе - вероятное зависание.
    if ($idleTicks -ge [math]::Ceiling(120 / $IntervalSec)) {
        Write-Host ""
        Write-Host "Уже 2 минуты без роста. Процесс жив, но на диск ничего не пишет:" -ForegroundColor Yellow
        Write-Host "возможно, ждёт скрытое окно, распаковывает большой архив или упёрся в сеть." -ForegroundColor Yellow
        $idleTicks = 0
    }

    $prevMB   = $nowMB
    $prevTime = $nowTime
}

$totalSec = ((Get-Date) - $startTime).TotalSeconds
Write-Host ""
Write-Host ("Итог: {0:N0} МБ, прирост {1:N0} МБ за {2}." -f `
    (Get-TotalMB $Path), ((Get-TotalMB $Path) - $startMB), (Format-Duration $totalSec)) -ForegroundColor Cyan
