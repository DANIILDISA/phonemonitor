# =============================================================================
#  PhoneMonitor IDD - сборка драйвера косвенного дисплея (UMDF 2.0 + IddCx).
# =============================================================================
#
#  Драйвер UMDF 2.0 - это обычная DLL, которую загружает WUDFHost.exe.
#  Поэтому CMake здесь применим, но требует ручного указания путей WDK:
#  штатного find-модуля для WDK в CMake нет.
#
#  Пример вызова:
#      cmake -S driver -B build/driver -G "Visual Studio 17 2022" -A x64 ^
#            -DWDK_ROOT="C:/Program Files (x86)/Windows Kits/10" ^
#            -DWDK_VERSION=10.0.26100.0 ^
#            -DUMDF_VERSION=2.33
#      cmake --build build/driver --config Release
#
#  Результат: build/driver/Release/PhoneMonitorIdd.dll
#  Для установки нужен ещё PhoneMonitor.inf и подпись (см. docs/BUILD.md).
# =============================================================================

cmake_minimum_required(VERSION 3.21)
project(PhoneMonitorIdd LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Драйвер IDD собирается только под Windows")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "Требуется компилятор MSVC: UMDF-заголовки несовместимы с MinGW/Clang-GNU")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
#  Поиск Windows Driver Kit
# -----------------------------------------------------------------------------
set(WDK_ROOT "C:/Program Files (x86)/Windows Kits/10" CACHE PATH
    "Корень Windows Kits 10 (там лежат Include/, Lib/)")

# Версия SDK/WDK. Если не задана явно, берём самую свежую из Include/.
set(WDK_VERSION "" CACHE STRING "Версия WDK, например 10.0.26100.0")

if(WDK_VERSION STREQUAL "")
    file(GLOB _wdk_versions RELATIVE "${WDK_ROOT}/Include" "${WDK_ROOT}/Include/10.*")
    if(_wdk_versions)
        list(SORT _wdk_versions)
        list(REVERSE _wdk_versions)
        list(GET _wdk_versions 0 WDK_VERSION)
        message(STATUS "Версия WDK определена автоматически: ${WDK_VERSION}")
    else()
        message(FATAL_ERROR "Не удалось найти WDK в ${WDK_ROOT}. Задайте -DWDK_ROOT и -DWDK_VERSION")
    endif()
endif()

# Версия UMDF. Определяет подкаталог в Lib/wdf/umdf и значение макроса
# UMDF_VERSION_MINOR, который обязан совпадать с линкуемой stub-библиотекой.
set(PM_TRACE_LEVEL "3" CACHE STRING
    "Подробность журнала драйвера: 0 ошибки, 1 предупреждения, 2 информация, 3 трасса")

set(UMDF_VERSION "2.33" CACHE STRING "Версия UMDF, например 2.33")
string(REPLACE "." ";" _umdf_parts "${UMDF_VERSION}")
list(GET _umdf_parts 0 UMDF_VERSION_MAJOR)
list(GET _umdf_parts 1 UMDF_VERSION_MINOR)

set(_wdk_inc "${WDK_ROOT}/Include/${WDK_VERSION}")
set(_wdk_lib "${WDK_ROOT}/Lib/${WDK_VERSION}")

# ВАЖНО о раскладке WDK: заголовки и библиотеки WDF лежат НЕ внутри
# версионированного каталога SDK, а рядом с ним:
#     Include/wdf/umdf/<umdf>/wdf.h          (а НЕ Include/<sdk>/wdf/...)
#     Lib/wdf/umdf/<arch>/<umdf>/*.lib       (а НЕ Lib/<sdk>/wdf/...)
# Причина в том, что версия WDF развивается независимо от версии SDK,
# и один и тот же набор WDF обслуживает несколько версий SDK.
set(_kits_inc "${WDK_ROOT}/Include")
set(_kits_lib "${WDK_ROOT}/Lib")

foreach(_required
        "${_wdk_inc}/um"
        "${_wdk_inc}/shared"
        "${_kits_inc}/wdf/umdf/${UMDF_VERSION}")
    if(NOT EXISTS "${_required}")
        message(FATAL_ERROR "Отсутствует обязательный каталог WDK: ${_required}")
    endif()
endforeach()

# -----------------------------------------------------------------------------
#  Версия расширения класса IddCx
# -----------------------------------------------------------------------------
#  Заголовки и библиотеки IddCx лежат НЕ в общем каталоге um, а в отдельных
#  версионированных подкаталогах:
#      Include/<sdk>/um/iddcx/<ver>/IddCx.h
#      Lib/<sdk>/um/<arch>/iddcx/<ver>/iddcxstub.lib
#  Это принципиально: версия задаёт набор доступных колбэков и раскладку
#  структур, а выбирается она подключением конкретного каталога, а не макросом.
#  ПОЧЕМУ ЗДЕСЬ НЕ «БЕРИ МАКСИМУМ»:
#  версия IddCx, против которой собран драйвер, должна поддерживаться
#  РАНТАЙМОМ установленной Windows, а не только заголовками SDK. Свежий SDK
#  легко содержит версию, которой системная IddCx.dll ещё не знает, и тогда
#  расширение отказывается привязываться: EvtDriverDeviceAdd возвращает ошибку,
#  устройство получает код проблемы 31 (CM_PROB_FAILED_ADD).
#
#  Ровно это и произошло при первой установке: SDK 10.0.28000 дал IddCx 1.11,
#  а рантайм Windows был из сборки 26100, где максимум 1.10.
#
#  Поэтому по умолчанию берётся консервативная 1.10 - она есть и в SDK 26100,
#  и в 28000, и поддерживается всеми Windows 11. Повысить можно явно через
#  -DIDDCX_VERSION=1.11, понимая риск.
set(IDDCX_VERSION "1.10" CACHE STRING
    "Версия IddCx (1.10 - безопасное значение; auto - максимум из SDK, может не поддерживаться рантаймом)")

if(IDDCX_VERSION STREQUAL "auto")
    file(GLOB _iddcx_dirs RELATIVE "${_wdk_inc}/um/iddcx" "${_wdk_inc}/um/iddcx/*")
    set(_best_major 0)
    set(_best_minor 0)
    foreach(_d ${_iddcx_dirs})
        # Сравнение строк тут не годится: "1.9" лексически больше "1.10",
        # поэтому major и minor разбираются и сравниваются как числа.
        if(_d MATCHES "^([0-9]+)\\.([0-9]+)$")
            set(_maj ${CMAKE_MATCH_1})
            set(_min ${CMAKE_MATCH_2})
            if((_maj GREATER _best_major) OR ((_maj EQUAL _best_major) AND (_min GREATER _best_minor)))
                set(_best_major ${_maj})
                set(_best_minor ${_min})
            endif()
        endif()
    endforeach()
    if(_best_major EQUAL 0)
        message(FATAL_ERROR "В ${_wdk_inc}/um/iddcx не найдено ни одной версии IddCx. WDK установлен не полностью")
    endif()
    set(IDDCX_VERSION "${_best_major}.${_best_minor}")
    message(WARNING "IddCx выбран автоматически: ${IDDCX_VERSION}. Убедитесь, что рантайм Windows его поддерживает")
endif()

message(STATUS "Версия IddCx для сборки: ${IDDCX_VERSION}")

set(_iddcx_inc "${_wdk_inc}/um/iddcx/${IDDCX_VERSION}")
if(NOT EXISTS "${_iddcx_inc}/IddCx.h")
    message(FATAL_ERROR "Не найден ${_iddcx_inc}/IddCx.h")
endif()

# Разбор версии на major/minor: IddCxFuncEnum.h содержит прямые проверки
#     #ifndef IDDCX_VERSION_MAJOR / #error IDDCX_VERSION_MAJOR is not defined
# и точно такую же для MINOR. Без этих определений заголовок не собирается.
string(REPLACE "." ";" _iddcx_parts "${IDDCX_VERSION}")
list(GET _iddcx_parts 0 IDDCX_VERSION_MAJOR)
list(GET _iddcx_parts 1 IDDCX_VERSION_MINOR)

# -----------------------------------------------------------------------------
#  Цель
# -----------------------------------------------------------------------------
add_library(PhoneMonitorIdd SHARED
    src/Driver.cpp
    src/Direct3DDevice.cpp
    src/IpcServer.cpp
    src/MonitorModes.cpp
    src/SwapChainProcessor.cpp
    src/Driver.h
    src/Direct3DDevice.h
    src/IpcServer.h
    src/MonitorModes.h
    src/SwapChainProcessor.h
    src/Trace.h
    ../common/pm_ipc.h
)

target_include_directories(PhoneMonitorIdd PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
    # Порядок важен: wdf/umdf должен идти ПЕРЕД um, иначе подхватится
    # пользовательский wdf.h вместо драйверного.
    "${_kits_inc}/wdf/umdf/${UMDF_VERSION}"
    # Каталог конкретной версии IddCx - именно отсюда берётся IddCx.h.
    "${_iddcx_inc}"
    "${_wdk_inc}/um"
    "${_wdk_inc}/shared"
)

target_compile_definitions(PhoneMonitorIdd PRIVATE
    UMDF_VERSION_MAJOR=${UMDF_VERSION_MAJOR}
    UMDF_VERSION_MINOR=${UMDF_VERSION_MINOR}
    # Версия IddCx: задаёт, какой набор публичных объявлений включит заголовок.
    IDDCX_VERSION_MAJOR=${IDDCX_VERSION_MAJOR}
    IDDCX_VERSION_MINOR=${IDDCX_VERSION_MINOR}
    # Без UMDF_USING_NTSTATUS заголовки WDF объявят HRESULT там, где IddCx
    # ожидает NTSTATUS, и весь файл Driver.cpp перестанет компилироваться.
    UMDF_USING_NTSTATUS
    _UNICODE
    UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    # Минимальная поддерживаемая версия: Windows 10 1903 (там появился
    # IddCx 1.4 с нужными нам возможностями).
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    # Уровень журнала: 0 ошибки, 1 предупреждения, 2 информация, 3 трасса.
    # Для диагностики держим 3 - драйвер пишет каждый шаг в
    # C:\ProgramData\PhoneMonitor\driver.log.
    PM_TRACE_LEVEL=${PM_TRACE_LEVEL}
)

# Быстрый путь смены режима через IddCxMonitorUpdateModes.
# Функция присутствует во всех версиях IddCx начиная с 1.0, но проверку
# оставляем: она страхует от урезанной или экспериментальной поставки WDK.
file(READ "${_iddcx_inc}/IddCx.h" _iddcx_header LIMIT 800000)
if(_iddcx_header MATCHES "IddCxMonitorUpdateModes")
    message(STATUS "IddCx ${IDDCX_VERSION}: MonitorUpdateModes доступна - включён быстрый путь смены режима")
    target_compile_definitions(PhoneMonitorIdd PRIVATE PM_HAVE_IDDCX_UPDATEMODES)
else()
    message(STATUS "IddCx ${IDDCX_VERSION}: MonitorUpdateModes нет - смена режима через departure/arrival")
endif()

target_compile_options(PhoneMonitorIdd PRIVATE
    /W4
    /permissive-
    # Исключения нужны: std::vector может бросить bad_alloc. UMDF это допускает,
    # в отличие от KMDF.
    /EHsc
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Release>:/GL>
    $<$<CONFIG:Debug>:/Od>
)

# -----------------------------------------------------------------------------
#  Линковка
# -----------------------------------------------------------------------------
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_arch "x64")
else()
    set(_arch "x86")
endif()

set(_umdf_lib_dir "${_kits_lib}/wdf/umdf/${_arch}/${UMDF_VERSION}")
if(NOT EXISTS "${_umdf_lib_dir}")
    message(FATAL_ERROR "Не найден каталог библиотек UMDF: ${_umdf_lib_dir}")
endif()

set(_iddcx_lib_dir "${_wdk_lib}/um/${_arch}/iddcx/${IDDCX_VERSION}")
if(NOT EXISTS "${_iddcx_lib_dir}/iddcxstub.lib")
    message(FATAL_ERROR "Не найдена ${_iddcx_lib_dir}/iddcxstub.lib")
endif()

target_link_directories(PhoneMonitorIdd PRIVATE
    "${_umdf_lib_dir}"
    "${_iddcx_lib_dir}"
    "${_wdk_lib}/um/${_arch}"
)

target_link_libraries(PhoneMonitorIdd PRIVATE
    # Заглушка UMDF: содержит DllMain, который вызывает наш DriverEntry.
    WdfDriverStubUm.lib
    # Заглушка расширения класса косвенных дисплеев.
    # Имя строчными буквами - именно так файл лежит в WDK.
    iddcxstub.lib
    d3d11.lib
    dxgi.lib
    dxguid.lib
    avrt.lib      # AvSetMmThreadCharacteristics
    ole32.lib     # CoCreateGuid
    advapi32.lib  # ConvertStringSecurityDescriptorToSecurityDescriptorW
    kernel32.lib
    user32.lib
    # WdfDriverStubUm.lib ссылается на DbgPrintEx - функцию отладочного вывода
    # уровня ядра, доступную пользовательскому режиму через ntdll. Без этой
    # библиотеки компоновка падает на неразрешённом __imp_DbgPrintEx.
    ntdll.lib
)

set_target_properties(PhoneMonitorIdd PROPERTIES
    OUTPUT_NAME "PhoneMonitorIdd"
    SUFFIX ".dll"
    # Драйвер обязан быть собран без стандартной библиотеки CRT-инициализации
    # в DllMain: WdfDriverStubUm сам управляет точкой входа.
    LINK_FLAGS "/SUBSYSTEM:WINDOWS /NODEFAULTLIB:libucrt.lib"
)

# INF копируем рядом с DLL: установщик ожидает их в одной папке.
add_custom_command(TARGET PhoneMonitorIdd POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/PhoneMonitor.inf"
        "$<TARGET_FILE_DIR:PhoneMonitorIdd>/PhoneMonitor.inf"
    COMMENT "Копирование INF-файла рядом с драйвером"
)

message(STATUS "PhoneMonitor IDD: WDK=${WDK_VERSION}, UMDF=${UMDF_VERSION}, архитектура=${_arch}")
