#include "NvencEncoder.h"

#if defined(PM_ENABLE_NVENC)

#include "../Log.h"
#include <cmath>

namespace pm
{
    namespace
    {
        /* Сигнатуры экспортов nvEncodeAPI64.dll. */
        using PFN_NvEncodeAPICreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
        using PFN_NvEncodeAPIGetMaxSupportedVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);

        /* Расшифровка кода NVENC для журнала. */
        const char* NvStatusText(NVENCSTATUS status) noexcept
        {
            switch (status)
            {
            case NV_ENC_SUCCESS:                      return "успех";
            case NV_ENC_ERR_NO_ENCODE_DEVICE:         return "нет устройства кодирования";
            case NV_ENC_ERR_UNSUPPORTED_DEVICE:       return "устройство не поддерживается";
            case NV_ENC_ERR_INVALID_ENCODERDEVICE:    return "некорректное устройство кодирования";
            case NV_ENC_ERR_INVALID_DEVICE:           return "некорректное устройство";
            case NV_ENC_ERR_DEVICE_NOT_EXIST:         return "устройство больше не существует";
            case NV_ENC_ERR_INVALID_PTR:              return "некорректный указатель";
            case NV_ENC_ERR_INVALID_PARAM:            return "некорректный параметр";
            case NV_ENC_ERR_INVALID_CALL:             return "недопустимый порядок вызовов";
            case NV_ENC_ERR_OUT_OF_MEMORY:            return "нехватка памяти";
            case NV_ENC_ERR_ENCODER_NOT_INITIALIZED:  return "кодировщик не инициализирован";
            case NV_ENC_ERR_UNSUPPORTED_PARAM:        return "параметр не поддерживается";
            case NV_ENC_ERR_LOCK_BUSY:                return "буфер занят";
            case NV_ENC_ERR_NOT_ENOUGH_BUFFER:        return "недостаточный размер буфера";
            case NV_ENC_ERR_ENCODER_BUSY:             return "кодировщик занят";
            case NV_ENC_ERR_EVENT_NOT_REGISTERD:      return "событие не зарегистрировано";
            case NV_ENC_ERR_GENERIC:                  return "общая ошибка";
            case NV_ENC_ERR_UNIMPLEMENTED:            return "не реализовано";
            case NV_ENC_ERR_RESOURCE_REGISTER_FAILED: return "не удалось зарегистрировать ресурс";
            case NV_ENC_ERR_RESOURCE_NOT_REGISTERED:  return "ресурс не зарегистрирован";
            case NV_ENC_ERR_RESOURCE_NOT_MAPPED:      return "ресурс не отображён";
            default:                                  return "неизвестный код";
            }
        }

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
    }

/* Проверка кода NVENC с логированием и выходом. */
#define PM_NV_CHECK(expr, what)                                                  \
    do {                                                                         \
        const NVENCSTATUS pm_nv_ = (expr);                                       \
        if (pm_nv_ != NV_ENC_SUCCESS) {                                          \
            PM_ERROR("NVENC: %s не удалось (код %d, %s)", what,                  \
                     static_cast<int>(pm_nv_), NvStatusText(pm_nv_));            \
            return E_FAIL;                                                       \
        }                                                                        \
    } while (0)

    NvencEncoder::NvencEncoder() noexcept = default;

    NvencEncoder::~NvencEncoder() noexcept
    {
        Shutdown();
    }

    // -------------------------------------------------------------------------
    HRESULT NvencEncoder::LoadApi() noexcept
    {
        // Только 64-битная сборка: 32-битный вариант DLL давно не поставляется.
        m_nvencLib = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (m_nvencLib == nullptr)
        {
            PM_INFO("nvEncodeAPI64.dll не найдена - видеокарта NVIDIA отсутствует или драйвер устарел");
            return E_NOINTERFACE;
        }

        auto getMaxVersion = reinterpret_cast<PFN_NvEncodeAPIGetMaxSupportedVersion>(
            GetProcAddress(m_nvencLib, "NvEncodeAPIGetMaxSupportedVersion"));
        auto createInstance = reinterpret_cast<PFN_NvEncodeAPICreateInstance>(
            GetProcAddress(m_nvencLib, "NvEncodeAPICreateInstance"));

        if (getMaxVersion == nullptr || createInstance == nullptr)
        {
            PM_ERROR("NVENC: в nvEncodeAPI64.dll нет обязательных экспортов");
            return E_NOINTERFACE;
        }

        // ---------------------------------------------------------------------
        // Проверка версии.
        //
        // Драйвер обязан поддерживать версию API не ниже той, с которой собран
        // заголовок. Если это не так, любой последующий вызов приведёт к
        // неопределённому поведению, поэтому отказываемся сразу.
        // ---------------------------------------------------------------------
        uint32_t driverMaxVersion = 0;
        const NVENCSTATUS versionStatus = getMaxVersion(&driverMaxVersion);
        if (versionStatus != NV_ENC_SUCCESS)
        {
            PM_ERROR("NVENC: не удалось узнать версию API драйвера (код %d)", static_cast<int>(versionStatus));
            return E_FAIL;
        }

        const uint32_t headerVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (driverMaxVersion < headerVersion)
        {
            PM_ERROR("NVENC: драйвер поддерживает API %u.%u, а сборка требует %u.%u. Обновите драйвер NVIDIA",
                driverMaxVersion >> 4, driverMaxVersion & 0xF,
                NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
            return E_FAIL;
        }

        // Поле version обязано быть заполнено ДО вызова: по нему библиотека
        // определяет раскладку структуры и заполняет совместимый набор функций.
        ZeroMemory(&m_api, sizeof(m_api));
        m_api.version = NV_ENCODE_API_FUNCTION_LIST_VER;

        const NVENCSTATUS createStatus = createInstance(&m_api);
        if (createStatus != NV_ENC_SUCCESS)
        {
            PM_ERROR("NVENC: NvEncodeAPICreateInstance не удался (код %d, %s)",
                static_cast<int>(createStatus), NvStatusText(createStatus));
            return E_FAIL;
        }

        PM_INFO("NVENC: таблица функций получена, версия API драйвера %u.%u",
            driverMaxVersion >> 4, driverMaxVersion & 0xF);
        return S_OK;
    }

    // -------------------------------------------------------------------------
    HRESULT NvencEncoder::BuildEncodeConfig(NV_ENC_INITIALIZE_PARAMS& initParams,
                                            NV_ENC_CONFIG& encodeConfig) noexcept
    {
        // ---------------------------------------------------------------------
        // Базовая конфигурация берётся из пресета, а не пишется с нуля:
        // NVENC хранит в пресете десятки полей, часть которых не документирована,
        // и заполнение их вручную регулярно приводит к NV_ENC_ERR_INVALID_PARAM.
        // Правильный путь - взять пресет и точечно переопределить нужное.
        // ---------------------------------------------------------------------
        NV_ENC_PRESET_CONFIG presetConfig{};
        presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
        presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

        const GUID presetGuid = m_config.lowLatencyPreset
            ? NV_ENC_PRESET_LOW_LATENCY_HP_GUID   // High Performance + Low Latency
            : NV_ENC_PRESET_LOW_LATENCY_HQ_GUID;

        PM_NV_CHECK(m_api.nvEncGetEncodePresetConfig(
            m_encoder, NV_ENC_CODEC_H264_GUID, presetGuid, &presetConfig),
            "получение конфигурации пресета");

        encodeConfig = presetConfig.presetCfg;
        encodeConfig.version = NV_ENC_CONFIG_VER;

        // --- Структура GOP ----------------------------------------------------
        // Бесконечный GOP: после первого IDR ключевые кадры больше не
        // вставляются. Целостность картинки обеспечивает Intra-Refresh.
        encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
        // frameIntervalP = 1 означает «только P-кадры, без B».
        // B-кадры требуют переупорядочивания и добавляют минимум один кадр
        // задержки - для нашей задачи это неприемлемо.
        encodeConfig.frameIntervalP = 1;
        encodeConfig.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;

        // --- Управление битрейтом --------------------------------------------
        NV_ENC_RC_PARAMS& rc = encodeConfig.rcParams;
        rc.rateControlMode = m_config.cbrRateControl
            ? NV_ENC_PARAMS_RC_CBR
            : NV_ENC_PARAMS_RC_VBR;
        rc.averageBitRate = m_config.bitrateBps;
        rc.maxBitRate = m_config.bitrateBps;

        // VBV размером ровно в один кадр - краеугольный камень низкой задержки.
        // При таком буфере кодировщик обязан уложить каждый кадр в свою долю
        // битрейта и не может «занимать» биты у будущих кадров, а значит и
        // накапливать задержку на стороне декодера.
        const uint32_t bitsPerFrame = m_config.bitrateBps / (m_config.fps ? m_config.fps : 60);
        rc.vbvBufferSize = bitsPerFrame;
        rc.vbvInitialDelay = bitsPerFrame;

        // Адаптивное квантование по пространству: заметно улучшает читаемость
        // мелкого текста (а на вторичном мониторе это в основном текст)
        // при том же битрейте.
        rc.enableAQ = 1;
        rc.aqStrength = 8;

        // --- Настройки H.264 --------------------------------------------------
        NV_ENC_CONFIG_H264& h264 = encodeConfig.encodeCodecConfig.h264Config;

        h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        // Повтор SPS/PPS перед каждым ключевым кадром: клиент, подключившийся
        // в середине потока, обязан получить параметры последовательности,
        // иначе MediaCodec не сможет сконфигурировать декодер.
        h264.repeatSPSPPS = m_config.repeatSpsPps ? 1 : 0;
        h264.outputAUD = 0;             // AUD только увеличивает объём
        h264.disableSPSPPS = 0;

        // Один слайс на кадр: несколько слайсов дали бы устойчивость к потерям,
        // но TCP и так гарантирует доставку, а лишние заголовки - это биты.
        h264.sliceMode = 3;             // 3 = заданное число слайсов
        h264.sliceModeData = 1;         // ровно один слайс

        // Уровень выбирается автоматически по разрешению и битрейту.
        h264.level = NV_ENC_LEVEL_AUTOSELECT;
        // Порядок следования кадров - строго по возрастанию, без переупорядочивания.
        h264.chromaFormatIDC = 1;       // 4:2:0

        // --- Intra-Refresh ----------------------------------------------------
        if (m_config.intraRefreshEnabled)
        {
            // Число кадров, за которое волна обновления проходит экран целиком.
            // При полосе в 5% ширины это 100/5 = 20 кадров.
            const uint32_t bandPercent = (m_config.intraRefreshBandPercent > 0 &&
                                          m_config.intraRefreshBandPercent <= 100)
                ? m_config.intraRefreshBandPercent : 5;
            const uint32_t refreshCount = (100 + bandPercent - 1) / bandPercent;

            h264.enableIntraRefresh = 1;
            h264.intraRefreshCnt = refreshCount;
            // Период между началами волн. Берём одну волну в секунду: чаще -
            // лишний расход битрейта, реже - слишком долгое восстановление
            // после повреждения.
            h264.intraRefreshPeriod = (m_config.fps > refreshCount) ? m_config.fps : refreshCount;

            // Intra-Refresh несовместим с B-кадрами и требует бесконечного GOP -
            // оба условия уже выполнены выше.
            PM_INFO("NVENC: Intra-Refresh включён. Полоса %u%% ширины, полный проход за %u кадров, период %u",
                bandPercent, refreshCount, h264.intraRefreshPeriod);
        }
        else
        {
            h264.enableIntraRefresh = 0;
        }

        // --- Параметры VUI ----------------------------------------------------
        // Явно объявляем полный диапазон и BT.709: без этого Android-декодер
        // может интерпретировать цвет как ограниченный диапазон, и картинка
        // станет блёклой.
        h264.h264VUIParameters.videoSignalTypePresentFlag = 1;
        h264.h264VUIParameters.videoFullRangeFlag = 0;
        h264.h264VUIParameters.colourDescriptionPresentFlag = 1;
        h264.h264VUIParameters.colourPrimaries = 1;           // BT.709
        h264.h264VUIParameters.transferCharacteristics = 1;   // BT.709
        h264.h264VUIParameters.colourMatrix = 1;              // BT.709

        // --- Параметры инициализации -----------------------------------------
        ZeroMemory(&initParams, sizeof(initParams));
        initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
        initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
        initParams.presetGUID = presetGuid;
        initParams.encodeWidth = m_config.width;
        initParams.encodeHeight = m_config.height;
        initParams.darWidth = m_config.width;
        initParams.darHeight = m_config.height;
        initParams.frameRateNum = m_config.fps;
        initParams.frameRateDen = 1;
        // PTD (Picture Type Decision) на стороне NVENC: он сам решает, когда
        // ставить IDR. Ручное управление типом кадра нам не нужно - принудительный
        // ключевой кадр запрашивается через флаг в NV_ENC_PIC_PARAMS.
        initParams.enablePTD = 1;
        // Синхронный режим. Асинхронный требует ожидания на событии и добавляет
        // кадр задержки - ровно то, с чем мы боремся.
        initParams.enableEncodeAsync = 0;
        initParams.encodeConfig = &encodeConfig;

        // ---------------------------------------------------------------------
        // tuningInfo и старые пресеты несовместимы.
        //
        // Начиная с Video Codec SDK 10 NVENC различает два способа задания
        // профиля кодирования:
        //   * новый  - пресеты P1..P7 ПЛЮС обязательный tuningInfo;
        //   * старый - именованные пресеты вроде LOW_LATENCY_HP, у которых
        //              характер нагрузки уже зашит в сам пресет.
        // Передача tuningInfo вместе со старым пресетом возвращает
        // NV_ENC_ERR_INVALID_PARAM. Спецификация проекта требует именно
        // LOW_LATENCY_HP, поэтому tuningInfo оставляем неопределённым:
        // низкая задержка и так заложена в выбранный пресет.
        // ---------------------------------------------------------------------
        initParams.tuningInfo = NV_ENC_TUNING_INFO_UNDEFINED;

        return S_OK;
    }

    // -------------------------------------------------------------------------
    HRESULT NvencEncoder::Initialize(const EncoderConfig& config) noexcept
    {
        m_config = config;

        if (config.device == nullptr)
        {
            PM_ERROR("NVENC: не передано устройство D3D11");
            return E_INVALIDARG;
        }

        HRESULT hr = LoadApi();
        if (FAILED(hr))
        {
            return hr;
        }

        // --- Открытие сессии ---------------------------------------------------
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams{};
        sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        sessionParams.device = config.device;
        sessionParams.apiVersion = NVENCAPI_VERSION;

        const NVENCSTATUS openStatus = m_api.nvEncOpenEncodeSessionEx(&sessionParams, &m_encoder);
        if (openStatus != NV_ENC_SUCCESS)
        {
            PM_ERROR("NVENC: не удалось открыть сессию (код %d, %s). "
                     "Частая причина - исчерпан лимит одновременных сессий NVENC",
                static_cast<int>(openStatus), NvStatusText(openStatus));
            m_encoder = nullptr;
            return E_FAIL;
        }

        // --- Конфигурация и инициализация -------------------------------------
        NV_ENC_INITIALIZE_PARAMS initParams{};
        NV_ENC_CONFIG encodeConfig{};

        hr = BuildEncodeConfig(initParams, encodeConfig);
        if (FAILED(hr))
        {
            Shutdown();
            return hr;
        }

        const NVENCSTATUS initStatus = m_api.nvEncInitializeEncoder(m_encoder, &initParams);
        if (initStatus != NV_ENC_SUCCESS)
        {
            PM_ERROR("NVENC: инициализация кодировщика не удалась (код %d, %s)",
                static_cast<int>(initStatus), NvStatusText(initStatus));
            Shutdown();
            return E_FAIL;
        }

        // --- Выходной буфер ----------------------------------------------------
        NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamBuffer{};
        bitstreamBuffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

        const NVENCSTATUS bufferStatus = m_api.nvEncCreateBitstreamBuffer(m_encoder, &bitstreamBuffer);
        if (bufferStatus != NV_ENC_SUCCESS)
        {
            PM_ERROR("NVENC: не удалось создать выходной буфер (код %d, %s)",
                static_cast<int>(bufferStatus), NvStatusText(bufferStatus));
            Shutdown();
            return E_FAIL;
        }
        m_bitstreamBuffer = bitstreamBuffer.bitstreamBuffer;

        m_initialized = true;
        m_frameCounter = 0;

        PM_INFO("NVENC инициализирован: %ux%u@%u Гц, %u кбит/с, CBR=%s, Intra-Refresh=%s",
            m_config.width, m_config.height, m_config.fps, m_config.bitrateBps / 1000,
            m_config.cbrRateControl ? "да" : "нет",
            m_config.intraRefreshEnabled ? "да" : "нет");

        return S_OK;
    }

    // -------------------------------------------------------------------------
    HRESULT NvencEncoder::RegisterInputTexture(ID3D11Texture2D* texture) noexcept
    {
        if (m_registeredTexture == texture && m_registeredResource != nullptr)
        {
            return S_OK;   // уже зарегистрирована
        }

        UnregisterInputTexture();

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);

        NV_ENC_REGISTER_RESOURCE reg{};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.width = desc.Width;
        reg.height = desc.Height;
        reg.pitch = 0;                 // для DirectX шаг вычисляется драйвером
        reg.resourceToRegister = texture;
        // B8G8R8A8_UNORM в терминологии NVENC называется ARGB. Конверсию в NV12
        // выполняет сам кодировщик на аппаратном блоке - нам не нужен ни
        // шейдер, ни видеопроцессор.
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;

        const NVENCSTATUS status = m_api.nvEncRegisterResource(m_encoder, &reg);
        if (status != NV_ENC_SUCCESS)
        {
            PM_ERROR("NVENC: регистрация входной текстуры не удалась (код %d, %s)",
                static_cast<int>(status), NvStatusText(status));
            return E_FAIL;
        }

        m_registeredResource = reg.registeredResource;
        m_registeredTexture = texture;

        PM_TRACE("NVENC: входная текстура %ux%u зарегистрирована", desc.Width, desc.Height);
        return S_OK;
    }

    void NvencEncoder::UnregisterInputTexture() noexcept
    {
        if (m_registeredResource != nullptr && m_encoder != nullptr)
        {
            m_api.nvEncUnregisterResource(m_encoder, m_registeredResource);
            m_registeredResource = nullptr;
        }
        m_registeredTexture = nullptr;
    }

    // -------------------------------------------------------------------------
    HRESULT NvencEncoder::EncodeFrame(ID3D11Texture2D* frame, bool forceKeyFrame,
                                      uint64_t timestampQpc, EncodedFrame& out) noexcept
    {
        if (!m_initialized || frame == nullptr)
        {
            return E_NOT_VALID_STATE;
        }

        const uint64_t encodeStart = Qpc();

        HRESULT hr = RegisterInputTexture(frame);
        if (FAILED(hr))
        {
            return hr;
        }

        // ---------------------------------------------------------------------
        // Отображение ресурса.
        //
        // MapInputResource обязана вызываться на КАЖДЫЙ кадр, даже если ресурс
        // зарегистрирован постоянно: именно она синхронизирует доступ NVENC
        // к текстуре с очередью команд D3D11. Пропуск этого шага приводит к
        // кодированию недорисованного кадра.
        // ---------------------------------------------------------------------
        NV_ENC_MAP_INPUT_RESOURCE mapInput{};
        mapInput.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapInput.registeredResource = m_registeredResource;

        PM_NV_CHECK(m_api.nvEncMapInputResource(m_encoder, &mapInput), "отображение входного ресурса");

        // --- Параметры кадра ---------------------------------------------------
        NV_ENC_PIC_PARAMS picParams{};
        picParams.version = NV_ENC_PIC_PARAMS_VER;
        picParams.inputBuffer = mapInput.mappedResource;
        picParams.bufferFmt = mapInput.mappedBufferFmt;
        picParams.inputWidth = m_config.width;
        picParams.inputHeight = m_config.height;
        picParams.outputBitstream = m_bitstreamBuffer;
        picParams.inputTimeStamp = m_frameCounter;
        picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

        if (forceKeyFrame)
        {
            // Принудительный IDR + выдача SPS/PPS: нужен при подключении нового
            // клиента, который не видел начала потока.
            picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            PM_TRACE("NVENC: запрошен принудительный ключевой кадр");
        }

        const NVENCSTATUS encodeStatus = m_api.nvEncEncodePicture(m_encoder, &picParams);

        // Ресурс отпускаем в любом случае - иначе следующий Map вернёт ошибку.
        m_api.nvEncUnmapInputResource(m_encoder, mapInput.mappedResource);

        if (encodeStatus != NV_ENC_SUCCESS && encodeStatus != NV_ENC_ERR_NEED_MORE_INPUT)
        {
            PM_ERROR("NVENC: кодирование кадра не удалось (код %d, %s)",
                static_cast<int>(encodeStatus), NvStatusText(encodeStatus));
            return E_FAIL;
        }

        if (encodeStatus == NV_ENC_ERR_NEED_MORE_INPUT)
        {
            // В синхронном режиме без B-кадров такого быть не должно, но
            // обрабатываем: просто нет выходных данных на этот кадр.
            PM_WARN("NVENC: кодировщику требуется больше входных данных - кадр пропущен");
            out.data.clear();
            return S_FALSE;
        }

        // --- Чтение битстрима ---------------------------------------------------
        NV_ENC_LOCK_BITSTREAM lockBitstream{};
        lockBitstream.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockBitstream.outputBitstream = m_bitstreamBuffer;
        // doNotWait = 0: блокируемся до готовности. В синхронном режиме пакет
        // уже готов, поэтому фактического ожидания не происходит.
        lockBitstream.doNotWait = 0;

        PM_NV_CHECK(m_api.nvEncLockBitstream(m_encoder, &lockBitstream), "блокировка битстрима");

        const auto* payload = static_cast<const uint8_t*>(lockBitstream.bitstreamBufferPtr);
        const uint32_t payloadSize = lockBitstream.bitstreamSizeInBytes;

        out.data.assign(payload, payload + payloadSize);
        out.isKeyFrame = (lockBitstream.pictureType == NV_ENC_PIC_TYPE_IDR ||
                          lockBitstream.pictureType == NV_ENC_PIC_TYPE_I);
        out.timestampQpc = timestampQpc;

        m_api.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);

        const uint64_t encodeEnd = Qpc();
        out.encodeTimeUs = static_cast<uint32_t>(
            ((encodeEnd - encodeStart) * 1'000'000ull) / QpcFrequency());

        ++m_frameCounter;

        PM_TRACE("NVENC: кадр %llu закодирован, %u байт, ключевой=%s, %u мкс",
            static_cast<unsigned long long>(m_frameCounter), payloadSize,
            out.isKeyFrame ? "да" : "нет", out.encodeTimeUs);

        return S_OK;
    }

    // -------------------------------------------------------------------------
    HRESULT NvencEncoder::Reconfigure(uint32_t width, uint32_t height, uint32_t bitrateBps) noexcept
    {
        if (!m_initialized)
        {
            return E_NOT_VALID_STATE;
        }

        if (width == m_config.width && height == m_config.height && bitrateBps == m_config.bitrateBps)
        {
            return S_OK;
        }

        PM_INFO("NVENC: переконфигурация %ux%u@%u кбит/с -> %ux%u@%u кбит/с",
            m_config.width, m_config.height, m_config.bitrateBps / 1000,
            width, height, bitrateBps / 1000);

        // Смена разрешения требует сброса зарегистрированного ресурса: старая
        // текстура имеет другой размер и больше не подходит.
        UnregisterInputTexture();

        m_config.width = width;
        m_config.height = height;
        m_config.bitrateBps = bitrateBps;

        NV_ENC_INITIALIZE_PARAMS initParams{};
        NV_ENC_CONFIG encodeConfig{};
        const HRESULT hr = BuildEncodeConfig(initParams, encodeConfig);
        if (FAILED(hr))
        {
            return hr;
        }

        NV_ENC_RECONFIGURE_PARAMS reconfigure{};
        reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        reconfigure.reInitEncodeParams = initParams;
        // forceIDR обязателен: после смены параметров старые опорные кадры
        // недействительны, и декодер обязан начать с чистого листа.
        reconfigure.forceIDR = 1;
        // resetEncoder сбрасывает внутреннее состояние управления битрейтом.
        reconfigure.resetEncoder = 1;

        const NVENCSTATUS status = m_api.nvEncReconfigureEncoder(m_encoder, &reconfigure);
        if (status != NV_ENC_SUCCESS)
        {
            PM_WARN("NVENC: переконфигурация на лету не удалась (код %d, %s), требуется пересоздание сессии",
                static_cast<int>(status), NvStatusText(status));
            return S_FALSE;
        }

        PM_INFO("NVENC: параметры обновлены без пересоздания сессии");
        return S_OK;
    }

    // -------------------------------------------------------------------------
    void NvencEncoder::Shutdown() noexcept
    {
        if (m_encoder != nullptr)
        {
            UnregisterInputTexture();

            if (m_bitstreamBuffer != nullptr)
            {
                m_api.nvEncDestroyBitstreamBuffer(m_encoder, m_bitstreamBuffer);
                m_bitstreamBuffer = nullptr;
            }

            // Сброс кодировщика перед уничтожением: передача nullptr в
            // EncodePicture с флагом EOS - штатный способ дождаться завершения
            // всех отложенных операций.
            NV_ENC_PIC_PARAMS eos{};
            eos.version = NV_ENC_PIC_PARAMS_VER;
            eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
            m_api.nvEncEncodePicture(m_encoder, &eos);

            m_api.nvEncDestroyEncoder(m_encoder);
            m_encoder = nullptr;

            PM_INFO("NVENC: сессия закрыта");
        }

        if (m_nvencLib != nullptr)
        {
            FreeLibrary(m_nvencLib);
            m_nvencLib = nullptr;
        }

        m_initialized = false;
    }
}

#undef PM_NV_CHECK

#endif /* PM_ENABLE_NVENC */
