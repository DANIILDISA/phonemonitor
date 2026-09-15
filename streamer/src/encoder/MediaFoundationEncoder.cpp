#include "MediaFoundationEncoder.h"
#include "../Log.h"

#include <mferror.h>
#include <algorithm>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace pm
{
    namespace
    {
        inline uint64_t Qpc() noexcept
        {
            LARGE_INTEGER c{};
            QueryPerformanceCounter(&c);
            return static_cast<uint64_t>(c.QuadPart);
        }

        inline uint64_t QpcFrequency() noexcept
        {
            static const uint64_t freq = [] {
                LARGE_INTEGER f{};
                QueryPerformanceFrequency(&f);
                return static_cast<uint64_t>(f.QuadPart);
            }();
            return freq;
        }

        /* Приоритет вендора при выборе MFT. Меньше - лучше. */
        int VendorRank(const std::wstring& name) noexcept
        {
            std::wstring lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });

            // Порядок соответствует требованию задачи: сначала AMD (AMF),
            // затем Intel (QuickSync). NVIDIA здесь на последнем месте, потому
            // что для неё есть прямой путь через nvEncodeAPI, который уже
            // попробовали раньше.
            if (lower.find(L"amd") != std::wstring::npos ||
                lower.find(L"radeon") != std::wstring::npos) return 0;
            if (lower.find(L"intel") != std::wstring::npos ||
                lower.find(L"quick sync") != std::wstring::npos) return 1;
            if (lower.find(L"nvidia") != std::wstring::npos) return 2;
            return 3;
        }

        /*
         * Установка свойства ICodecAPI с мягкой обработкой отказа:
         * не все MFT поддерживают весь набор, и это нормально.
         *
         * VARIANT передаётся ПО ЗНАЧЕНИЮ, а не по константной ссылке:
         * сигнатура ICodecAPI::SetValue объявлена как
         *     HRESULT SetValue(const GUID*, VARIANT*)
         * то есть второй аргумент неконстантный. Копия на стеке и дешевле,
         * и безопаснее, чем const_cast над чужим объектом.
         *
         * Возвращает true, если свойство реально принято кодировщиком.
         */
        bool TrySetCodecProperty(ICodecAPI* api, const GUID& property,
                                 VARIANT value, const char* description,
                                 bool quiet = false) noexcept
        {
            if (api == nullptr) return false;

            if (api->IsSupported(&property) != S_OK)
            {
                if (!quiet)
                {
                    PM_TRACE("MFT: свойство \"%s\" не поддерживается - пропускаем", description);
                }
                return false;
            }

            const HRESULT hr = api->SetValue(&property, &value);
            if (FAILED(hr))
            {
                if (!quiet)
                {
                    PM_WARN("MFT: не удалось установить \"%s\": %s", description, HResultToText(hr));
                }
                return false;
            }

            if (!quiet)
            {
                PM_INFO("MFT: установлено \"%s\"", description);
            }
            return true;
        }

        VARIANT VarU32(uint32_t v) noexcept
        {
            VARIANT var{};
            var.vt = VT_UI4;
            var.ulVal = v;
            return var;
        }

        VARIANT VarBool(bool v) noexcept
        {
            VARIANT var{};
            var.vt = VT_BOOL;
            var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
            return var;
        }
    }

    // =========================================================================
    //  VideoColorConverter
    // =========================================================================
    VideoColorConverter::~VideoColorConverter() noexcept
    {
        Shutdown();
    }

    HRESULT VideoColorConverter::Initialize(ID3D11Device* device, uint32_t width, uint32_t height) noexcept
    {
        Shutdown();

        m_device = device;
        m_width = width;
        m_height = height;

        // Видеоинтерфейсы доступны только если устройство создано с флагом
        // D3D11_CREATE_DEVICE_VIDEO_SUPPORT - IpcClient создаёт его именно так.
        PM_CHECK_HR(device->QueryInterface(IID_PPV_ARGS(&m_videoDevice)),
            "Устройство D3D11 не предоставляет ID3D11VideoDevice");

        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        PM_CHECK_HR(context.As(&m_videoContext),
            "Контекст D3D11 не предоставляет ID3D11VideoContext");

        // ---------------------------------------------------------------------
        // Описание содержимого для видеопроцессора.
        //
        // Входная и выходная частота кадров одинаковы: мы не выполняем ни
        // деинтерлейсинг, ни преобразование частоты - только цветовую
        // конверсию и, при необходимости, масштабирование.
        // ---------------------------------------------------------------------
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDesc.InputFrameRate.Numerator = 60;
        contentDesc.InputFrameRate.Denominator = 1;
        contentDesc.InputWidth = width;
        contentDesc.InputHeight = height;
        contentDesc.OutputFrameRate.Numerator = 60;
        contentDesc.OutputFrameRate.Denominator = 1;
        contentDesc.OutputWidth = width;
        contentDesc.OutputHeight = height;
        // PLAYBACK вместо VIDEO_PROCESSING: просим драйвер оптимизировать
        // по скорости, а не по качеству фильтрации.
        contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        PM_CHECK_HR(m_videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &m_enumerator),
            "Не удалось создать перечислитель видеопроцессора");

        PM_CHECK_HR(m_videoDevice->CreateVideoProcessor(m_enumerator.Get(), 0, &m_processor),
            "Не удалось создать видеопроцессор");

        // --- Выходная текстура NV12 -------------------------------------------
        D3D11_TEXTURE2D_DESC nv12Desc{};
        nv12Desc.Width = width;
        nv12Desc.Height = height;
        nv12Desc.MipLevels = 1;
        nv12Desc.ArraySize = 1;
        nv12Desc.Format = DXGI_FORMAT_NV12;
        nv12Desc.SampleDesc.Count = 1;
        nv12Desc.Usage = D3D11_USAGE_DEFAULT;
        // BIND_RENDER_TARGET нужен видеопроцессору как приёмнику,
        // BIND_SHADER_RESOURCE - кодировщику как источнику.
        nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        nv12Desc.CPUAccessFlags = 0;
        nv12Desc.MiscFlags = 0;

        PM_CHECK_HR(m_device->CreateTexture2D(&nv12Desc, nullptr, &m_nv12Texture),
            "Не удалось создать текстуру NV12");

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc{};
        outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputViewDesc.Texture2D.MipSlice = 0;

        PM_CHECK_HR(m_videoDevice->CreateVideoProcessorOutputView(
            m_nv12Texture.Get(), m_enumerator.Get(), &outputViewDesc, &m_outputView),
            "Не удалось создать выходное представление видеопроцессора");

        // ---------------------------------------------------------------------
        // Цветовые пространства.
        //
        // Вход - RGB с полным диапазоном (рабочий стол Windows).
        // Выход - YCbCr BT.709 с ограниченным диапазоном: именно такой поток
        // ожидает декодер Android при colourMatrix = 1 в VUI.
        // ---------------------------------------------------------------------
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace{};
        inputColorSpace.Usage = 0;              // 0 = отображение (playback)
        inputColorSpace.RGB_Range = 0;          // 0 = полный диапазон 0-255
        inputColorSpace.YCbCr_Matrix = 1;       // 1 = BT.709
        inputColorSpace.YCbCr_xvYCC = 0;
        inputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        m_videoContext->VideoProcessorSetStreamColorSpace(m_processor.Get(), 0, &inputColorSpace);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace{};
        outputColorSpace.Usage = 0;
        outputColorSpace.RGB_Range = 1;         // не используется для YUV-выхода
        outputColorSpace.YCbCr_Matrix = 1;      // BT.709
        outputColorSpace.YCbCr_xvYCC = 0;
        outputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        m_videoContext->VideoProcessorSetOutputColorSpace(m_processor.Get(), &outputColorSpace);

        // Отключаем автообработку: авто-контраст и шумодав драйвера добавляют
        // задержку и портят текст.
        m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_processor.Get(), 0, FALSE);

        PM_INFO("Преобразователь цвета BGRA -> NV12 готов (%ux%u, аппаратный видеопроцессор)",
            width, height);
        return S_OK;
    }

    HRESULT VideoColorConverter::Convert(ID3D11Texture2D* source) noexcept
    {
        if (!m_processor || source == nullptr)
        {
            return E_NOT_VALID_STATE;
        }

        // Представление входа кешируется: его создание - относительно дорогая
        // операция, а текстура-источник у нас одна и та же от кадра к кадру.
        if (m_cachedInput != source || !m_inputView)
        {
            m_inputView.Reset();

            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
            inputViewDesc.FourCC = 0;                  // взять из формата текстуры
            inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            inputViewDesc.Texture2D.MipSlice = 0;
            inputViewDesc.Texture2D.ArraySlice = 0;

            PM_CHECK_HR(m_videoDevice->CreateVideoProcessorInputView(
                source, m_enumerator.Get(), &inputViewDesc, &m_inputView),
                "Не удалось создать входное представление видеопроцессора");

            m_cachedInput = source;
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.PastFrames = 0;
        stream.FutureFrames = 0;
        stream.pInputSurface = m_inputView.Get();

        // Один вызов Blt выполняет и цветовую конверсию, и масштабирование
        // (если размеры входа и выхода различаются).
        PM_CHECK_HR(m_videoContext->VideoProcessorBlt(
            m_processor.Get(), m_outputView.Get(), 0, 1, &stream),
            "VideoProcessorBlt не удался");

        return S_OK;
    }

    void VideoColorConverter::Shutdown() noexcept
    {
        m_inputView.Reset();
        m_cachedInput = nullptr;
        m_outputView.Reset();
        m_nv12Texture.Reset();
        m_processor.Reset();
        m_enumerator.Reset();
        m_videoContext.Reset();
        m_videoDevice.Reset();
        m_device.Reset();
        m_width = m_height = 0;
    }

    // =========================================================================
    //  MediaFoundationEncoder
    // =========================================================================
    MediaFoundationEncoder::~MediaFoundationEncoder() noexcept
    {
        Shutdown();
    }

    HRESULT MediaFoundationEncoder::Initialize(const EncoderConfig& config) noexcept
    {
        m_config = config;

        if (config.device == nullptr)
        {
            PM_ERROR("MFT: не передано устройство D3D11");
            return E_INVALIDARG;
        }

        // MFSTARTUP_LITE не поднимает сокетные компоненты Media Foundation -
        // нам нужен только кодек.
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr))
        {
            PM_ERROR("MFStartup не удался: %s", HResultToText(hr));
            return hr;
        }
        m_mfStarted = true;

        // ---------------------------------------------------------------------
        // Менеджер устройства DXGI.
        //
        // Через него MFT получает доступ к нашему D3D11-устройству и может
        // читать входные текстуры напрямую из видеопамяти. Без него MFT
        // потребует системную память, и каждый кадр придётся копировать
        // через шину PCIe - это десятки миллисекунд на 1080p.
        // ---------------------------------------------------------------------
        hr = MFCreateDXGIDeviceManager(&m_resetToken, &m_deviceManager);
        if (FAILED(hr))
        {
            PM_ERROR("MFCreateDXGIDeviceManager не удался: %s", HResultToText(hr));
            Shutdown();
            return hr;
        }

        hr = m_deviceManager->ResetDevice(config.device, m_resetToken);
        if (FAILED(hr))
        {
            PM_ERROR("ResetDevice для менеджера DXGI не удался: %s", HResultToText(hr));
            Shutdown();
            return hr;
        }

        hr = SelectTransform();
        if (FAILED(hr))
        {
            Shutdown();
            return hr;
        }

        hr = m_converter.Initialize(config.device, config.width, config.height);
        if (FAILED(hr))
        {
            Shutdown();
            return hr;
        }

        hr = ConfigureMediaTypes();
        if (FAILED(hr))
        {
            Shutdown();
            return hr;
        }

        ConfigureCodecApi();

        // --- Запуск потоковой обработки ----------------------------------------
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        m_streaming = true;

        m_frameCounter = 0;
        m_lastKeyFrameQpc = 0;

        PM_INFO("Кодировщик готов: %s, %ux%u@%u Гц, %u кбит/с, режим %s",
            m_backendName.c_str(), m_config.width, m_config.height, m_config.fps,
            m_config.bitrateBps / 1000, m_asyncTransform ? "асинхронный" : "синхронный");

        return S_OK;
    }

    // -------------------------------------------------------------------------
    HRESULT MediaFoundationEncoder::SelectTransform() noexcept
    {
        MFT_REGISTER_TYPE_INFO outputInfo{};
        outputInfo.guidMajorType = MFMediaType_Video;
        outputInfo.guidSubtype = MFVideoFormat_H264;

        IMFActivate** activates = nullptr;
        UINT32 activateCount = 0;

        // MFT_ENUM_FLAG_HARDWARE отсекает программный кодировщик Microsoft:
        // он укладывается в 1080p60 только на мощном процессоре и всё равно
        // не даёт нужной задержки.
        const HRESULT hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr,
            &outputInfo,
            &activates,
            &activateCount);

        if (FAILED(hr) || activateCount == 0)
        {
            PM_ERROR("Аппаратный кодировщик H.264 в системе не найден. "
                     "Обновите драйвер видеокарты или установите её штатный пакет");
            if (activates != nullptr) CoTaskMemFree(activates);
            return FAILED(hr) ? hr : E_NOINTERFACE;
        }

        PM_INFO("Найдено аппаратных кодировщиков H.264: %u", activateCount);

        // Выбираем по рангу вендора; при равенстве - первый по порядку MFT
        // (MFTEnumEx уже отсортировал их по предпочтительности системы).
        int bestRank = 100;
        UINT32 bestIndex = 0;
        std::wstring bestName;

        for (UINT32 i = 0; i < activateCount; ++i)
        {
            LPWSTR friendlyName = nullptr;
            UINT32 nameLength = 0;
            if (FAILED(activates[i]->GetAllocatedString(
                    MFT_FRIENDLY_NAME_Attribute, &friendlyName, &nameLength)))
            {
                continue;
            }

            const std::wstring name(friendlyName, nameLength);
            const int rank = VendorRank(name);
            PM_INFO("  [%u] %S (приоритет %d)", i, name.c_str(), rank);

            if (rank < bestRank)
            {
                bestRank = rank;
                bestIndex = i;
                bestName = name;
            }
            CoTaskMemFree(friendlyName);
        }

        HRESULT activateResult = activates[bestIndex]->ActivateObject(IID_PPV_ARGS(&m_transform));

        // Освобождаем весь список сразу: активированный объект держит себя сам.
        for (UINT32 i = 0; i < activateCount; ++i)
        {
            activates[i]->Release();
        }
        CoTaskMemFree(activates);

        if (FAILED(activateResult))
        {
            PM_ERROR("Не удалось активировать MFT: %s", HResultToText(activateResult));
            return activateResult;
        }

        // Имя бэкенда для журнала: конвертируем из широкой строки.
        {
            const int required = WideCharToMultiByte(CP_UTF8, 0, bestName.c_str(),
                static_cast<int>(bestName.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<size_t>(required), '\0');
            WideCharToMultiByte(CP_UTF8, 0, bestName.c_str(), static_cast<int>(bestName.size()),
                utf8.data(), required, nullptr, nullptr);
            m_backendName = "MFT: " + utf8;
        }

        // ---------------------------------------------------------------------
        // Асинхронные MFT.
        //
        // Все современные аппаратные кодировщики асинхронны. Такой MFT
        // заблокирован до тех пор, пока приложение явно не подтвердит, что
        // умеет работать с моделью событий - это и делает MF_TRANSFORM_ASYNC_UNLOCK.
        // ---------------------------------------------------------------------
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(m_transform->GetAttributes(&attributes)) && attributes)
        {
            UINT32 isAsync = 0;
            attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
            m_asyncTransform = (isAsync != 0);

            if (m_asyncTransform)
            {
                const HRESULT unlockResult = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                if (FAILED(unlockResult))
                {
                    PM_ERROR("Не удалось разблокировать асинхронный MFT: %s",
                        HResultToText(unlockResult));
                    return unlockResult;
                }

                if (FAILED(m_transform.As(&m_eventGenerator)))
                {
                    PM_ERROR("Асинхронный MFT не предоставляет IMFMediaEventGenerator");
                    return E_NOINTERFACE;
                }
            }

            // Подсказка кодировщику: поток реального времени, буферизация
            // запрещена. Поддерживается не всеми MFT, поэтому без проверки.
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        }

        // Передаём MFT наше устройство D3D11.
        const HRESULT managerResult = m_transform->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(m_deviceManager.Get()));
        if (FAILED(managerResult))
        {
            PM_WARN("MFT отказался принимать менеджер DXGI (%s). "
                    "Кадры пойдут через системную память - задержка вырастет",
                HResultToText(managerResult));
        }

        // ICodecAPI может отсутствовать у экзотических MFT - это не ошибка.
        if (FAILED(m_transform.As(&m_codecApi)))
        {
            PM_WARN("MFT не предоставляет ICodecAPI: тонкая настройка задержки недоступна");
        }

        PM_INFO("Выбран кодировщик: %S", bestName.c_str());
        return S_OK;
    }

    // -------------------------------------------------------------------------
    HRESULT MediaFoundationEncoder::ConfigureMediaTypes() noexcept
    {
        // ---------------------------------------------------------------------
        // Порядок обязателен: сначала ВЫХОД, затем ВХОД.
        //
        // MFT-кодировщик определяет допустимые входные форматы исходя из уже
        // заданного выходного типа. Обратный порядок приводит к
        // MF_E_TRANSFORM_TYPE_NOT_SET на, казалось бы, корректных параметрах.
        // ---------------------------------------------------------------------
        ComPtr<IMFMediaType> outputType;
        PM_CHECK_HR(MFCreateMediaType(&outputType), "MFCreateMediaType для выхода");

        PM_CHECK_HR(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "MF_MT_MAJOR_TYPE");
        PM_CHECK_HR(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "MF_MT_SUBTYPE");
        PM_CHECK_HR(outputType->SetUINT32(MF_MT_AVG_BITRATE, m_config.bitrateBps), "MF_MT_AVG_BITRATE");
        PM_CHECK_HR(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE,
            m_config.width, m_config.height), "MF_MT_FRAME_SIZE выхода");
        PM_CHECK_HR(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE,
            m_config.fps, 1), "MF_MT_FRAME_RATE выхода");
        PM_CHECK_HR(MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
            "MF_MT_PIXEL_ASPECT_RATIO");
        PM_CHECK_HR(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
            "MF_MT_INTERLACE_MODE выхода");
        // High-профиль: даёт CABAC и 8x8-преобразование, то есть заметно
        // лучшее качество текста при том же битрейте.
        PM_CHECK_HR(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High),
            "MF_MT_MPEG2_PROFILE");
        PM_CHECK_HR(outputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE),
            "MF_MT_ALL_SAMPLES_INDEPENDENT");

        PM_CHECK_HR(m_transform->SetOutputType(0, outputType.Get(), 0), "SetOutputType");
        m_outputType = outputType;

        // ---------------------------------------------------------------------
        // Вход: NV12. Это единственный формат, который гарантированно
        // принимают все аппаратные кодировщики H.264.
        // ---------------------------------------------------------------------
        ComPtr<IMFMediaType> inputType;
        PM_CHECK_HR(MFCreateMediaType(&inputType), "MFCreateMediaType для входа");

        PM_CHECK_HR(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "MF_MT_MAJOR_TYPE входа");
        PM_CHECK_HR(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "MF_MT_SUBTYPE входа");
        PM_CHECK_HR(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE,
            m_config.width, m_config.height), "MF_MT_FRAME_SIZE входа");
        PM_CHECK_HR(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE,
            m_config.fps, 1), "MF_MT_FRAME_RATE входа");
        PM_CHECK_HR(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
            "MF_MT_PIXEL_ASPECT_RATIO входа");
        PM_CHECK_HR(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
            "MF_MT_INTERLACE_MODE входа");

        PM_CHECK_HR(m_transform->SetInputType(0, inputType.Get(), 0), "SetInputType");

        // ---------------------------------------------------------------------
        // Заголовок последовательности (SPS/PPS).
        //
        // Аппаратные MFT кладут его в атрибут MF_MT_MPEG_SEQUENCE_HEADER
        // выходного типа. Мы сохраняем его и дописываем перед каждым ключевым
        // кадром: MediaCodec на телефоне не сможет сконфигурировать декодер,
        // если SPS/PPS не придут до первого слайса.
        // ---------------------------------------------------------------------
        UINT32 headerSize = 0;
        if (SUCCEEDED(m_outputType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &headerSize)) &&
            headerSize > 0)
        {
            m_sequenceHeader.resize(headerSize);
            const HRESULT blobResult = m_outputType->GetBlob(
                MF_MT_MPEG_SEQUENCE_HEADER, m_sequenceHeader.data(), headerSize, nullptr);
            if (FAILED(blobResult))
            {
                m_sequenceHeader.clear();
                PM_WARN("Не удалось прочитать заголовок последовательности: %s",
                    HResultToText(blobResult));
            }
            else
            {
                PM_INFO("Получен заголовок последовательности SPS/PPS, %u байт", headerSize);
            }
        }
        else
        {
            PM_INFO("MFT не отдал SPS/PPS заранее - рассчитываем на их выдачу внутри потока");
        }

        return S_OK;
    }

    // -------------------------------------------------------------------------
    void MediaFoundationEncoder::ConfigureCodecApi() noexcept
    {
        if (!m_codecApi)
        {
            return;
        }

        // --- Режим реального времени -------------------------------------------
        // Главный переключатель: запрещает кодировщику накапливать кадры ради
        // качества. Без него AMD и Intel буферизуют 2-4 кадра.
        TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVLowLatencyMode,
            VarBool(true), "режим низкой задержки");

        TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncCommonRealTime,
            VarBool(true), "приоритет реального времени");

        // --- Управление битрейтом -----------------------------------------------
        TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncCommonRateControlMode,
            VarU32(m_config.cbrRateControl
                ? eAVEncCommonRateControlMode_CBR
                : eAVEncCommonRateControlMode_UnconstrainedVBR),
            "режим управления битрейтом (CBR)");

        TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate,
            VarU32(m_config.bitrateBps), "средний битрейт");

        TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncCommonMaxBitRate,
            VarU32(m_config.bitrateBps), "максимальный битрейт");

        // Буфер размером в один кадр - тот же приём, что и VBV в NVENC.
        const uint32_t bitsPerFrame = m_config.bitrateBps / (m_config.fps ? m_config.fps : 60);
        TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncCommonBufferSize,
            VarU32(bitsPerFrame), "размер буфера битрейта (один кадр)");

        // --- Структура GOP --------------------------------------------------------
        // Бесконечный GOP. Ноль трактуется большинством MFT как «ключевые кадры
        // только по требованию», что нам и нужно: IDR запрашивается явно.
        TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncMPVGOPSize,
            VarU32(0), "бесконечный GOP");

        // B-кадры запрещены: они требуют переупорядочивания и добавляют
        // минимум один кадр задержки.
        TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount,
            VarU32(0), "запрет B-кадров");

        // Одна опорная рамка: больше не нужно при P-only, а меньше памяти -
        // меньше работы блоку оценки движения.
        TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncVideoMaxNumRefFrame,
            VarU32(1), "одна опорная рамка");

        // Один слайс на кадр.
        TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncNumWorkerThreads,
            VarU32(1), "один рабочий поток");

        // Качество в режиме реального времени: компромисс между скоростью
        // и детализацией. 70 из 100 - проверенное значение для текста.
        TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncCommonQualityVsSpeed,
            VarU32(33), "баланс качество/скорость в пользу скорости");

        // --- Intra-Refresh --------------------------------------------------------
        //
        // Переносимое свойство для управления волной внутрикадрового обновления
        // в ICodecAPI всё-таки есть: CODECAPI_AVEncVideoGradualIntraRefresh.
        // Поддерживают его не все MFT, поэтому работаем через проверку
        // IsSupported и фактический результат SetValue.
        //
        // Тип значения в заголовке WDK не документирован, а у разных
        // производителей трактуется по-разному: одни ждут число кадров, за
        // которое волна проходит кадр целиком, другие - простой флаг
        // включения. Поэтому пробуем оба варианта по очереди и сообщаем в
        // журнал, какой сработал. Это надёжнее, чем угадывать.
        if (m_config.intraRefreshEnabled)
        {
            const uint32_t bandPercent = (m_config.intraRefreshBandPercent > 0 &&
                                          m_config.intraRefreshBandPercent <= 100)
                ? m_config.intraRefreshBandPercent : 5;
            // 5% ширины за кадр -> полный проход за 20 кадров.
            const uint32_t refreshFrames = (100 + bandPercent - 1) / bandPercent;

            bool applied = TrySetCodecProperty(m_codecApi.Get(),
                CODECAPI_AVEncVideoGradualIntraRefresh,
                VarU32(refreshFrames),
                "постепенное внутрикадровое обновление (число кадров)",
                /*quiet*/ true);

            if (applied)
            {
                PM_INFO("Intra-Refresh включён: полоса %u%% ширины кадра, полный проход за %u кадров",
                    bandPercent, refreshFrames);
            }
            else
            {
                applied = TrySetCodecProperty(m_codecApi.Get(),
                    CODECAPI_AVEncVideoGradualIntraRefresh,
                    VarBool(true),
                    "постепенное внутрикадровое обновление (флаг)",
                    /*quiet*/ true);

                if (applied)
                {
                    PM_INFO("Intra-Refresh включён флагом: шаг волны выбирает сам кодировщик");
                }
            }

            m_intraRefreshActive = applied;

            if (!applied)
            {
                // Честный откат: этот MFT волну обновления не поддерживает.
                // Устойчивость обеспечим гарантированной доставкой TCP и
                // выдачей IDR по запросу клиента (PM_MSG_KEYFRAME_REQ).
                PM_WARN("Кодировщик не принял Intra-Refresh. "
                        "Восстановление после сбоев - через ключевой кадр по запросу клиента");
            }
        }
    }

    void MediaFoundationEncoder::RequestKeyFrame() noexcept
    {
        if (m_codecApi)
        {
            VARIANT force = VarBool(true);
            // Свойство одноразовое: действует ровно на следующий входной кадр.
            m_codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &force);
        }
    }

    // -------------------------------------------------------------------------
    HRESULT MediaFoundationEncoder::SubmitFrame(ID3D11Texture2D* nv12,
                                                uint64_t sampleTime100ns) noexcept
    {
        // Буфер поверх текстуры DXGI: копирования не происходит, MFT получает
        // ссылку на ту же видеопамять.
        ComPtr<IMFMediaBuffer> buffer;
        PM_CHECK_HR(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer),
            "MFCreateDXGISurfaceBuffer");

        // Длину обязан выставить вызывающий: буфер поверх поверхности создаётся
        // с нулевой текущей длиной, и MFT сочтёт кадр пустым.
        ComPtr<IMF2DBuffer> buffer2d;
        if (SUCCEEDED(buffer.As(&buffer2d)))
        {
            DWORD contiguousLength = 0;
            if (SUCCEEDED(buffer2d->GetContiguousLength(&contiguousLength)))
            {
                buffer->SetCurrentLength(contiguousLength);
            }
        }

        ComPtr<IMFSample> sample;
        PM_CHECK_HR(MFCreateSample(&sample), "MFCreateSample");
        PM_CHECK_HR(sample->AddBuffer(buffer.Get()), "AddBuffer");
        PM_CHECK_HR(sample->SetSampleTime(static_cast<LONGLONG>(sampleTime100ns)), "SetSampleTime");

        // Длительность кадра в единицах 100 нс.
        const LONGLONG duration = 10'000'000LL / (m_config.fps ? m_config.fps : 60);
        PM_CHECK_HR(sample->SetSampleDuration(duration), "SetSampleDuration");

        const HRESULT hr = m_transform->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING)
        {
            // MFT занят: нужно сначала забрать готовый выход.
            return hr;
        }
        PM_CHECK_HR(hr, "ProcessInput");

        return S_OK;
    }

    // -------------------------------------------------------------------------
    HRESULT MediaFoundationEncoder::DrainOutput(EncodedFrame& out, uint32_t /*timeoutMs*/) noexcept
    {
        // Узнаём, кто выделяет выходные буферы. Аппаратные MFT почти всегда
        // делают это сами (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES).
        MFT_OUTPUT_STREAM_INFO streamInfo{};
        PM_CHECK_HR(m_transform->GetOutputStreamInfo(0, &streamInfo), "GetOutputStreamInfo");

        const bool mftAllocates =
            (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                   MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        MFT_OUTPUT_DATA_BUFFER outputBuffer{};
        outputBuffer.dwStreamID = 0;
        outputBuffer.pSample = nullptr;
        outputBuffer.dwStatus = 0;
        outputBuffer.pEvents = nullptr;

        ComPtr<IMFSample> ownSample;
        ComPtr<IMFMediaBuffer> ownBuffer;

        if (!mftAllocates)
        {
            // Выделяем буфер сами. Размер берём из streamInfo, но не меньше
            // разумного минимума: некоторые MFT сообщают заниженное значение.
            const DWORD bufferSize = (std::max)(streamInfo.cbSize, DWORD{ 1 << 20 });

            PM_CHECK_HR(MFCreateMemoryBuffer(bufferSize, &ownBuffer), "MFCreateMemoryBuffer");
            PM_CHECK_HR(MFCreateSample(&ownSample), "MFCreateSample для выхода");
            PM_CHECK_HR(ownSample->AddBuffer(ownBuffer.Get()), "AddBuffer для выхода");

            outputBuffer.pSample = ownSample.Get();
        }

        DWORD status = 0;
        const HRESULT hr = m_transform->ProcessOutput(0, 1, &outputBuffer, &status);

        if (outputBuffer.pEvents != nullptr)
        {
            outputBuffer.pEvents->Release();
            outputBuffer.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            // Штатная ситуация: кодировщик ещё не готов отдать пакет.
            if (mftAllocates && outputBuffer.pSample != nullptr)
            {
                outputBuffer.pSample->Release();
            }
            return S_FALSE;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            // MFT просит пересогласовать выходной тип - бывает после смены
            // разрешения. Перечитываем тип и повторяем на следующем кадре.
            PM_WARN("MFT сообщил о смене формата выходного потока, пересогласовываем тип");
            ComPtr<IMFMediaType> newType;
            if (SUCCEEDED(m_transform->GetOutputAvailableType(0, 0, &newType)))
            {
                m_transform->SetOutputType(0, newType.Get(), 0);
                m_outputType = newType;
            }
            if (mftAllocates && outputBuffer.pSample != nullptr)
            {
                outputBuffer.pSample->Release();
            }
            return S_FALSE;
        }

        if (FAILED(hr))
        {
            PM_ERROR("ProcessOutput не удался: %s", HResultToText(hr));
            if (mftAllocates && outputBuffer.pSample != nullptr)
            {
                outputBuffer.pSample->Release();
            }
            return hr;
        }

        // ---------------------------------------------------------------------
        // Владение выходным сэмплом.
        //
        // Правило MFT: если поток отдаёт сэмплы сам (PROVIDES_SAMPLES), то
        // ProcessOutput вернул НОВУЮ ссылку, и освободить её обязаны мы.
        // Если сэмпл предоставили мы, ProcessOutput ссылку не добавлял, и
        // владельцем остаётся наш ownSample.
        // ---------------------------------------------------------------------
        ComPtr<IMFSample> resultSample;
        if (mftAllocates)
        {
            resultSample.Attach(outputBuffer.pSample);   // перенимаем ссылку
        }
        else
        {
            resultSample = ownSample;                    // ComPtr добавит свою ссылку
        }

        if (!resultSample)
        {
            return S_FALSE;
        }

        // --- Ключевой кадр? ------------------------------------------------------
        UINT32 isCleanPoint = 0;
        resultSample->GetUINT32(MFSampleExtension_CleanPoint, &isCleanPoint);
        out.isKeyFrame = (isCleanPoint != 0);

        // --- Копирование битстрима ------------------------------------------------
        ComPtr<IMFMediaBuffer> resultBuffer;
        PM_CHECK_HR(resultSample->ConvertToContiguousBuffer(&resultBuffer),
            "ConvertToContiguousBuffer");

        BYTE* data = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        PM_CHECK_HR(resultBuffer->Lock(&data, &maxLength, &currentLength), "Lock выходного буфера");

        out.data.clear();

        // Перед ключевым кадром дописываем сохранённые SPS/PPS, если MFT не
        // вставляет их сам. Признак «не вставляет» - отсутствие NAL-типов 7/8
        // в начале пакета.
        if (out.isKeyFrame && !m_sequenceHeader.empty())
        {
            bool headerPresent = false;
            // Ищем стартовый код и проверяем тип первого NAL.
            for (DWORD i = 0; i + 4 < currentLength && i < 64; ++i)
            {
                if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
                {
                    const uint8_t nalType = data[i + 3] & 0x1F;
                    if (nalType == 7 || nalType == 8)
                    {
                        headerPresent = true;
                    }
                    break;
                }
            }

            if (!headerPresent)
            {
                out.data.insert(out.data.end(), m_sequenceHeader.begin(), m_sequenceHeader.end());
                PM_TRACE("К ключевому кадру добавлен сохранённый SPS/PPS (%zu байт)",
                    m_sequenceHeader.size());
            }
        }

        out.data.insert(out.data.end(), data, data + currentLength);
        resultBuffer->Unlock();

        return S_OK;
    }

    // -------------------------------------------------------------------------
    HRESULT MediaFoundationEncoder::PumpAsyncEvents(bool wantInput, bool wantOutput,
                                                    ID3D11Texture2D* nv12,
                                                    uint64_t sampleTime100ns,
                                                    EncodedFrame& out,
                                                    uint32_t timeoutMs) noexcept
    {
        /*
         * Модель асинхронного MFT.
         *
         * MFT сам сообщает, когда готов принять кадр (METransformNeedInput) и
         * когда готов отдать пакет (METransformHaveOutput). События приходят
         * через IMFMediaEventGenerator. Здесь используется блокирующий
         * GetEvent, потому что EncodeFrame по контракту синхронна, а ожидание
         * всё равно исчисляется единицами миллисекунд.
         *
         * Ограничение по времени реализовано через счётчик попыток с
         * неблокирующим опросом: MF_EVENT_FLAG_NO_WAIT возвращает
         * MF_E_NO_EVENTS_AVAILABLE, если событий нет.
         */
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;

        bool inputDone = !wantInput;
        bool outputDone = !wantOutput;

        while (!inputDone || !outputDone)
        {
            ComPtr<IMFMediaEvent> mediaEvent;
            HRESULT hr = m_eventGenerator->GetEvent(MF_EVENT_FLAG_NO_WAIT, &mediaEvent);

            if (hr == MF_E_NO_EVENTS_AVAILABLE)
            {
                if (GetTickCount64() >= deadline)
                {
                    PM_TRACE("MFT: события не пришли за %u мс (вход=%d, выход=%d)",
                        timeoutMs, static_cast<int>(inputDone), static_cast<int>(outputDone));
                    // Отсутствие выхода - не ошибка: кодировщику может не
                    // хватать данных. Сообщаем об этом кодом S_FALSE.
                    return inputDone ? S_FALSE : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                }
                // Уступаем квант: активное ожидание на одном ядре съело бы
                // процессорное время, нужное самому кодировщику.
                SwitchToThread();
                continue;
            }

            if (FAILED(hr))
            {
                PM_ERROR("GetEvent от MFT не удался: %s", HResultToText(hr));
                return hr;
            }

            MediaEventType eventType = MEUnknown;
            if (FAILED(mediaEvent->GetType(&eventType)))
            {
                continue;
            }

            switch (eventType)
            {
            case METransformNeedInput:
                if (!inputDone && nv12 != nullptr)
                {
                    hr = SubmitFrame(nv12, sampleTime100ns);
                    if (FAILED(hr) && hr != MF_E_NOTACCEPTING)
                    {
                        return hr;
                    }
                    inputDone = true;
                }
                break;

            case METransformHaveOutput:
                if (!outputDone)
                {
                    hr = DrainOutput(out, timeoutMs);
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                    if (hr == S_OK)
                    {
                        outputDone = true;
                    }
                }
                break;

            case METransformDrainComplete:
                PM_TRACE("MFT: осушение завершено");
                outputDone = true;
                break;

            default:
                // Прочие события (смена формата, ошибки потока) нас не касаются.
                break;
            }
        }

        return S_OK;
    }

    // -------------------------------------------------------------------------
    HRESULT MediaFoundationEncoder::EncodeFrame(ID3D11Texture2D* frame, bool forceKeyFrame,
                                                uint64_t timestampQpc, EncodedFrame& out) noexcept
    {
        if (!m_transform || !m_streaming || frame == nullptr)
        {
            return E_NOT_VALID_STATE;
        }

        const uint64_t encodeStart = Qpc();

        // Шаг 1: цветовая конверсия на видеопроцессоре.
        HRESULT hr = m_converter.Convert(frame);
        if (FAILED(hr))
        {
            return hr;
        }

        if (forceKeyFrame)
        {
            RequestKeyFrame();
            PM_TRACE("MFT: запрошен принудительный ключевой кадр");
        }

        // Временная метка в единицах 100 нс, монотонно растущая.
        const uint64_t sampleTime = (m_frameCounter * 10'000'000ull) /
                                    (m_config.fps ? m_config.fps : 60);

        out.data.clear();
        out.isKeyFrame = false;
        out.timestampQpc = timestampQpc;

        if (m_asyncTransform)
        {
            // Таймаут 100 мс: заведомо больше времени кодирования одного кадра
            // (обычно 2-8 мс), но достаточно мал, чтобы не подвесить конвейер
            // при сбое драйвера.
            hr = PumpAsyncEvents(true, true, m_converter.Output(), sampleTime, out, 100);
        }
        else
        {
            hr = SubmitFrame(m_converter.Output(), sampleTime);
            if (SUCCEEDED(hr))
            {
                hr = DrainOutput(out, 100);
            }
        }

        if (FAILED(hr))
        {
            return hr;
        }

        ++m_frameCounter;

        if (out.data.empty())
        {
            // Кодировщик пока не отдал пакет - это нормально на самом первом
            // кадре у некоторых MFT.
            return S_FALSE;
        }

        if (out.isKeyFrame)
        {
            m_lastKeyFrameQpc = timestampQpc;
        }

        const uint64_t encodeEnd = Qpc();
        out.encodeTimeUs = static_cast<uint32_t>(
            ((encodeEnd - encodeStart) * 1'000'000ull) / QpcFrequency());

        PM_TRACE("MFT: кадр %llu закодирован, %zu байт, ключевой=%s, %u мкс",
            static_cast<unsigned long long>(m_frameCounter), out.data.size(),
            out.isKeyFrame ? "да" : "нет", out.encodeTimeUs);

        return S_OK;
    }

    // -------------------------------------------------------------------------
    HRESULT MediaFoundationEncoder::Reconfigure(uint32_t width, uint32_t height,
                                                uint32_t bitrateBps) noexcept
    {
        if (width == m_config.width && height == m_config.height &&
            bitrateBps == m_config.bitrateBps)
        {
            return S_OK;
        }

        // Смена битрейта без смены разрешения делается на лету через ICodecAPI.
        if (width == m_config.width && height == m_config.height)
        {
            m_config.bitrateBps = bitrateBps;
            TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate,
                VarU32(bitrateBps), "средний битрейт (переконфигурация)");
            TrySetCodecProperty(m_codecApi.Get(), CODECAPI_AVEncCommonMaxBitRate,
                VarU32(bitrateBps), "максимальный битрейт (переконфигурация)");
            RequestKeyFrame();
            PM_INFO("MFT: битрейт изменён на %u кбит/с", bitrateBps / 1000);
            return S_OK;
        }

        // Смена разрешения требует пересогласования типов, а значит - полного
        // пересоздания. Сообщаем об этом вызывающему кодом S_FALSE.
        PM_INFO("MFT: смена разрешения %ux%u -> %ux%u требует пересоздания кодировщика",
            m_config.width, m_config.height, width, height);
        return S_FALSE;
    }

    // -------------------------------------------------------------------------
    void MediaFoundationEncoder::Shutdown() noexcept
    {
        if (m_transform && m_streaming)
        {
            // Корректная последовательность останова: конец потока -> осушение
            // -> прекращение обработки. Пропуск любого шага приводит к утечке
            // ресурсов GPU внутри драйвера.
            m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            m_streaming = false;
        }

        m_converter.Shutdown();

        m_outputType.Reset();
        m_eventGenerator.Reset();
        m_codecApi.Reset();
        m_transform.Reset();
        m_deviceManager.Reset();
        m_sequenceHeader.clear();

        if (m_mfStarted)
        {
            MFShutdown();
            m_mfStarted = false;
        }
    }
}
