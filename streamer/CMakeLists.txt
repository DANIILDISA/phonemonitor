# =============================================================================
#  PhoneMonitor Streamer - сборка приложения-стримера.
# =============================================================================
#
#  Обычное консольное приложение Win32. Единственная внешняя зависимость -
#  NVIDIA Video Codec SDK, и та опциональна.
#
#  Сборка без NVENC (только Media Foundation: AMD AMF / Intel QuickSync):
#      cmake -S streamer -B build/streamer -G "Visual Studio 17 2022" -A x64
#      cmake --build build/streamer --config Release
#
#  Сборка с NVENC:
#      cmake -S streamer -B build/streamer -G "Visual Studio 17 2022" -A x64 ^
#            -DNVENC_SDK_DIR="C:/Video_Codec_SDK_12.2.72"
#      cmake --build build/streamer --config Release
#
#  Результат: build/streamer/Release/phonemonitor-streamer.exe
# =============================================================================

cmake_minimum_required(VERSION 3.21)
project(PhoneMonitorStreamer LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Стример использует Win32, D3D11 и Media Foundation - сборка возможна только под Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Статическая линковка рантайма: стример нередко запускают на машине без
# распространяемого пакета Visual C++, и зависимость от него ломает запуск.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# -----------------------------------------------------------------------------
#  Исходники
# -----------------------------------------------------------------------------
add_executable(phonemonitor-streamer
    src/main.cpp
    src/Config.cpp
    src/Log.cpp
    src/StreamSession.cpp
    src/ipc/IpcClient.cpp
    src/net/TcpTransport.cpp
    src/net/AdbBridge.cpp
    src/encoder/EncoderFactory.cpp
    src/encoder/MediaFoundationEncoder.cpp
    src/encoder/NvencEncoder.cpp

    src/Config.h
    src/Log.h
    src/StreamSession.h
    src/ipc/IpcClient.h
    src/net/TcpTransport.h
    src/net/AdbBridge.h
    src/encoder/IEncoder.h
    src/encoder/MediaFoundationEncoder.h
    src/encoder/NvencEncoder.h

    ../common/pm_ipc.h
    ../common/pm_protocol.h
)

target_include_directories(phonemonitor-streamer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
)

target_compile_definitions(phonemonitor-streamer PRIVATE
    _UNICODE
    UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX            # иначе макросы min/max ломают std::min/std::max
    _WIN32_WINNT=0x0A00
    # Отключает предупреждения о «небезопасных» функциях CRT: мы используем
    # безопасные варианты там, где это имеет смысл, а остальное - шум.
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(phonemonitor-streamer PRIVATE
        /W4
        /permissive-
        /EHsc
        /utf-8              # исходники в UTF-8, включая русские строки
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/Gy>
    )
    target_link_options(phonemonitor-streamer PRIVATE
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

# -----------------------------------------------------------------------------
#  NVIDIA Video Codec SDK (опционально)
# -----------------------------------------------------------------------------
set(NVENC_SDK_DIR "" CACHE PATH
    "Корень NVIDIA Video Codec SDK (внутри должен быть Interface/nvEncodeAPI.h)")

set(_nvenc_header "")
if(NVENC_SDK_DIR)
    foreach(_candidate
            "${NVENC_SDK_DIR}/Interface/nvEncodeAPI.h"
            "${NVENC_SDK_DIR}/include/nvEncodeAPI.h"
            "${NVENC_SDK_DIR}/nvEncodeAPI.h")
        if(EXISTS "${_candidate}")
            set(_nvenc_header "${_candidate}")
            break()
        endif()
    endforeach()
endif()

if(_nvenc_header)
    get_filename_component(_nvenc_include_dir "${_nvenc_header}" DIRECTORY)
    target_include_directories(phonemonitor-streamer PRIVATE "${_nvenc_include_dir}")
    target_compile_definitions(phonemonitor-streamer PRIVATE PM_ENABLE_NVENC)
    message(STATUS "NVENC включён, заголовок: ${_nvenc_header}")
    # Библиотека НЕ линкуется: nvEncodeAPI64.dll загружается через LoadLibrary,
    # чтобы стример запускался и на машинах без видеокарты NVIDIA.
else()
    if(NVENC_SDK_DIR)
        message(WARNING "В ${NVENC_SDK_DIR} не найден nvEncodeAPI.h - NVENC отключён")
    else()
        message(STATUS "NVENC_SDK_DIR не задан - сборка без NVENC "
                       "(кодирование пойдёт через Media Foundation: AMD AMF / Intel QuickSync)")
    endif()
endif()

# -----------------------------------------------------------------------------
#  Системные библиотеки
# -----------------------------------------------------------------------------
target_link_libraries(phonemonitor-streamer PRIVATE
    d3d11.lib
    dxgi.lib
    dxguid.lib
    # Media Foundation: mfplat - базовые типы и MFStartup,
    # mfuuid - GUID форматов, mf - MFTEnumEx, mfreadwrite - на будущее.
    mfplat.lib
    mfuuid.lib
    mf.lib
    mfreadwrite.lib
    strmiids.lib      # GUID из codecapi.h
    ws2_32.lib        # Winsock
    avrt.lib          # MMCSS
    ole32.lib
    shlwapi.lib
    advapi32.lib
)

set_target_properties(phonemonitor-streamer PROPERTIES
    OUTPUT_NAME "phonemonitor-streamer"
    VS_DPI_AWARE "PerMonitor"
)

message(STATUS "PhoneMonitor Streamer: конфигурация завершена")
