# PhoneMonitor — сборка и установка

---

## 1. Требования

### Windows-часть

| Компонент | Версия | Обязателен |
|---|---|---|
| Windows | 10 1903+ / 11 | ✅ |
| Visual Studio 2022 (Desktop C++) | 17.6+ | ✅ |
| Windows SDK | 10.0.22621+ | ✅ |
| Windows Driver Kit (WDK) | соответствующий SDK | ✅ только для драйвера |
| CMake | 3.21+ | ✅ |
| NVIDIA Video Codec SDK | 12.x | ❌ опционально (NVENC) |

> WDK обязан быть **той же версии**, что и установленный Windows SDK. Несовпадение
> приводит к ошибкам компоновки в `WdfDriverStubUm.lib`.

### Android-часть

| Компонент | Версия |
|---|---|
| Android Studio | Iguana 2023.2+ |
| JDK | 17 |
| Android SDK Platform | API 34 |
| Android SDK Platform-Tools (adb) | последняя |
| Устройство | Android 8.0 (API 26)+ с аппаратным декодером H.264 |

---

## 2. Сборка драйвера

```powershell
cmake -S driver -B build/driver -G "Visual Studio 17 2022" -A x64 `
      -DWDK_ROOT="C:/Program Files (x86)/Windows Kits/10" `
      -DWDK_VERSION=10.0.26100.0 `
      -DUMDF_VERSION=2.33

cmake --build build/driver --config Release
```

Если `WDK_VERSION` не задана, CMake возьмёт самую свежую из `Include/`.
Результат: `build/driver/Release/PhoneMonitorIdd.dll` и рядом скопированный
`PhoneMonitor.inf`.

CMake сам проверит наличие `IddCxMonitorUpdateModes` в заголовках и включит
быстрый путь смены разрешения (`PM_HAVE_IDDCX_UPDATEMODES`). В выводе будет
строка:

```
-- IddCx поддерживает MonitorUpdateModes - включён быстрый путь смены режима
```

### 2.1 Подпись и установка

> **Проще всего — скриптом.** Он сам запросит права, создаст сертификат,
> подпишет пакет, установит его и создаст узел устройства, а вывод продублирует
> в `build\driver-install.log`:
> ```powershell
> powershell -NoProfile -ExecutionPolicy Bypass -File tools\install-driver.ps1
> ```
> Удаление — тот же скрипт с ключом `-Remove`. Ниже описано, что он делает
> вручную, — на случай отладки.

Начиная с Windows 10 1607 неподписанные драйверы не загружаются.

> **Предупреждение.** Тестовый режим подписи снижает защиту системы: в нём
> загружается любой драйвер с самоподписанным сертификатом, а не только
> проверенный Microsoft. Включайте его на машине для разработки, а не на
> рабочей, и выключайте, когда закончите:
> ```powershell
> bcdedit /set testsigning off
> ```
> Пока режим включён, в правом нижнем углу рабочего стола висит водяной знак
> «Тестовый режим» — это нормально и исчезнет после выключения и перезагрузки.

```powershell
# Один раз: включить тестовый режим и перезагрузиться
bcdedit /set testsigning on
shutdown /r /t 0
```

Создание тестового сертификата. `makecert` из старых руководств **устарел** и
в современных версиях SDK отсутствует — используйте встроенный командлет
PowerShell:

```powershell
# Один раз: создать самоподписанный сертификат подписи кода
$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject "CN=PhoneMonitor Test" `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -KeyUsage DigitalSignature `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3") `
    -NotAfter (Get-Date).AddYears(3)

# Положить его в доверенные, иначе система не примет подпись
$store = Get-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)"
Export-Certificate -Cert $store -FilePath phonemonitor.cer | Out-Null
Import-Certificate -FilePath phonemonitor.cer -CertStoreLocation "Cert:\LocalMachine\Root" | Out-Null
Import-Certificate -FilePath phonemonitor.cer -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher" | Out-Null
```

Последние две строки обязательны и требуют прав администратора: без корневого
сертификата в `Root` система не доверяет подписи, а без записи в
`TrustedPublisher` установка драйвера покажет запрос, который в тихом режиме
просто отклонится.

Сборка каталога и подпись:

```powershell
cd build\driver\Release
inf2cat /driver:. /os:10_X64
signtool sign /v /fd SHA256 /s My /n "PhoneMonitor Test" `
         /t http://timestamp.digicert.com PhoneMonitor.cat
```

`inf2cat` и `signtool` входят в WDK и лежат в
`C:\Program Files (x86)\Windows Kits\10\bin\<версия>\x64\`.

```powershell
# Установить пакет драйвера (от имени администратора)
pnputil /add-driver PhoneMonitor.inf /install

# Создать узел устройства
devgen /add /hardwareid "Root\PhoneMonitorIdd"
```

`devgen.exe` входит в WDK. Альтернатива — `devcon`:

```powershell
devcon install PhoneMonitor.inf Root\PhoneMonitorIdd
```

Проверка: в Диспетчере устройств, раздел «Видеоадаптеры», должно появиться
**PhoneMonitor Virtual Display (USB)**.

### 2.2 Удаление

```powershell
devcon remove Root\PhoneMonitorIdd
pnputil /delete-driver oemXX.inf /uninstall /force   # номер из pnputil /enum-drivers
```

---

## 3. Сборка стримера

Без NVENC (кодирование через Media Foundation — AMD AMF / Intel QuickSync):

```powershell
cmake -S streamer -B build/streamer -G "Visual Studio 17 2022" -A x64
cmake --build build/streamer --config Release
```

С NVENC:

```powershell
cmake -S streamer -B build/streamer -G "Visual Studio 17 2022" -A x64 `
      -DNVENC_SDK_DIR="C:/Video_Codec_SDK_12.2.72"
cmake --build build/streamer --config Release
```

`NVENC_SDK_DIR` должен содержать `Interface/nvEncodeAPI.h`. Сам
`nvEncodeAPI64.dll` линковать не нужно — он загружается через `LoadLibrary`,
чтобы стример запускался и на машинах без видеокарты NVIDIA.

Результат: `build/streamer/Release/phonemonitor-streamer.exe`.
Рантайм MSVC линкуется статически, отдельный распространяемый пакет не нужен.

---

## 4. Сборка Android-клиента

```powershell
cd android-client

# Один раз, если нет обёртки Gradle:
gradle wrapper --gradle-version 8.7

.\gradlew.bat assembleRelease
.\gradlew.bat installRelease      # на подключённое устройство
```

Либо просто откройте `android-client/` в Android Studio и нажмите Run.

APK: `android-client/app/build/outputs/apk/release/app-release.apk`.

---

## 5. Запуск

1. Включите на телефоне **Отладку по USB** (Настройки → Для разработчиков).
2. Подключите кабель, подтвердите доверие к компьютеру.
3. Запустите приложение **PhoneMonitor** на телефоне — оно откроет порт 6100
   и покажет «Ожидание подключения компьютера».
4. На ПК:

```powershell
.\phonemonitor-streamer.exe
```

Стример сам найдёт `adb`, дождётся устройства, создаст форвард, подключится и
согласует разрешение с драйвером.

### Полезные ключи

```powershell
# Подробный журнал и запись в файл
.\phonemonitor-streamer.exe --verbose --log-file streamer.log

# Принудительный выбор бэкенда
.\phonemonitor-streamer.exe --encoder nvenc
.\phonemonitor-streamer.exe --encoder mf

# Другой битрейт и частота
.\phonemonitor-streamer.exe --bitrate 25000 --fps 60

# Форвард создан вручную
adb forward tcp:6100 tcp:6100
.\phonemonitor-streamer.exe --no-adb

# Отключить реакцию на нагрев
.\phonemonitor-streamer.exe --no-thermal
```

Полный список: `phonemonitor-streamer.exe --help`.

---

## 5a. Возврат защиты системы после работ

Тестовый режим подписи и выключенный Secure Boot снижают защиту машины.
Когда драйвер больше не нужен, восстановите их — но в правильном порядке,
иначе BitLocker потребует ключ восстановления.

```cmd
manage-bde -protectors -disable C: -RebootCount 2
manage-bde -protectors -disable D: -RebootCount 2
bcdedit /set testsigning off
```

Перезагрузка → включить Secure Boot в UEFI → загрузиться → проверить:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-security.ps1
```

Скрипт отдельно предупреждает о самом опасном состоянии: том зашифрован,
а защита приостановлена. Тогда ключ шифрования лежит на диске открытым, и
шифрование не защищает ни от чего. Возобновляется одной командой без
перезагрузки:

```cmd
manage-bde -protectors -enable C:
```

---

## 6. Диагностика

### Журнал драйвера

Драйвер пишет в `OutputDebugStringW`. Смотреть через **DebugView**
(Sysinternals), запущенный **от администратора** с включённым
*Capture → Capture Global Win32* — иначе сообщения из сессии 0 не видны.

Фильтр: `[PhoneMonitor]`.

### Типовые проблемы

| Симптом | Причина и решение |
|---|---|
| «Секция разделяемой памяти не появилась» | Драйвер не установлен или не в состоянии D0. Проверьте Диспетчер устройств |
| «Несовместимая версия IPC» | Драйвер и стример собраны из разных ревизий. Пересоберите оба |
| «Не найден адаптер, на котором можно открыть общие текстуры» | Драйвер ещё не получил swapchain. Сделайте виртуальный монитор активным в «Параметрах экрана» |
| «Драйвер не выдал хендл фенса» | `OpenProcess(PROCESS_DUP_HANDLE)` не прошёл. Запустите стример от администратора |
| «Аппаратный кодировщик H.264 не найден» | Драйвер видеокарты установлен через Центр обновления (урезанный). Поставьте полный пакет с сайта производителя |
| «Устройство не авторизовано» | Подтвердите отладку по USB на экране телефона |
| Чёрный экран на телефоне, соединение есть | Нет ключевого кадра. Проверьте журнал на наличие SPS/PPS; клиент запрашивает IDR автоматически |
| Задержка растёт со временем | Проверьте сводку: если `queueDepth` держится у 8, декодер не успевает — снизьте разрешение или битрейт |
| Форвард остался после аварийного выхода | `adb forward --remove tcp:6100` |

### Сводка стримера

Каждые 300 кадров (настраивается `--stats-interval`):

```
Сводка: кадров 1200 (отправлено 1198, пропущено 2), 59.8 к/с, 14.21 Мбит/с |
задержка Windows: средняя 8.43 мс, пик 19.10 мс | кодирование 3.12 мс |
потери в драйвере: 0
```

* **задержка Windows** — от публикации драйвером до отправки в сокет;
* **кодирование** — чистое время работы кодировщика;
* **потери в драйвере** — кадры, которые стример не успел забрать.

Задержка декодирования на телефоне приходит отдельно в `PM_MSG_STATS` и
выводится строкой «Телеметрия клиента».

---

## 7. Структура сборочных артефактов

```
build/
  driver/Release/
    PhoneMonitorIdd.dll
    PhoneMonitor.inf
    PhoneMonitor.cat          (после inf2cat)
  streamer/Release/
    phonemonitor-streamer.exe

android-client/app/build/outputs/apk/release/
    app-release.apk
```
