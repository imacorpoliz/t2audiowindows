# T2AudioPort: Руководство для разработчиков и агентов (AGENT_HANDOVER_GUIDE.md)

> **Статус документа:** Актуален на 28 октября 2026 г.  
> **Целевая аудитория:** AI-агенты, реверс-инженеры и разработчики ядерных драйверов Windows.  
> **Целевое оборудование:** MacBook Pro 16,1 (2019), MacBook Pro 15,1 / 15,2 / 16,2 / 16,4, iMac Pro, Mac mini 2018 (все устройства Apple с сопроцессором T2 на шине PCI `VEN_106B&DEV_1803`).

---

## 1. Введение и контекст проекта

Данный репозиторий содержит полную кодовую базу, результаты реверс-инжиниринга и документацию для работы со звуком на компьютерах Apple с чипом T2 под управлением Windows 10/11 x64.

### Компоненты системы
1. **`T2AudioPort\`**: Открытый ядерный аудиодрайвер WDM / WaveRT (`T2AudioMiniport.sys`), взаимодействующий с чипом T2 и мостом `AppleUSBVHCI.sys`.
2. **`SpeakerDSP-APO\`**: Пользовательская DSP-система (VST2 плагин `bankstown.dll` и конфигуратор `generate_config.py` для Equalizer APO), реализующая кроссоверы, эквализацию и задержки для 6 физических динамиков MacBook Pro 16,1.
3. **`MBP-Audio-driver\`**: Официальный драйвер Apple Bootcamp (`AppleAudio.sys`), установленный в качестве эталона на физической машине.
4. **`kait2en\`**: Полная рабочая реализация драйвера для Linux (`t2bce_audio`), используемая как архитектурный эталон взаимодействия с T2 BCE Mailbox и BAR0.

---

## 2. Аппаратная архитектура и протокол взаимодействия

### 2.1 Топология шины PCI и карта регистров T2
Аудиоконтроллер Apple T2 представлен на шине PCI идентификатором:
```text
PCI\VEN_106B&DEV_1803&SUBSYS_00000000&REV_01
```

Контроллер имеет следующие области памяти (BAR):
- **BAR0 / BAR1 (Shared Memory / `BufferStruct`)**:
  - Область разделяемой памяти между хостом (Intel CPU) и сопроцессором Apple T2 (ARM BridgeOS).
  - Базовое смещение структуры `BufferStruct` определяется значением регистра `GPR[2]` (обычно смещение `0x1000` или `0x2000`).
  - Сигнатура заголовка: `0x19870423` (`T2AUDIO_SIG`).
  - Содержит таблицу дескрипторов устройств `T2AUDIO_DEVICE_METADATA` с фиксированным шагом `0xBDEC` байт.
  - Устройство динамиков идентифицируется по имени `"Speaker"`.
  - Поток вывода (`OutputStreams[0]`) динамиков имеет размер аппаратного кольца **399 360 байт (`0x61800` = 16 640 кадров $\times$ 24 байта)**.
- **BAR4 (или BAR2) + `0xC000` (General Purpose Registers / GPR)**:
  - `GPR[0]` (+0x00): Версия протокола GPR (должна быть $\ge 2$).
  - `GPR[1]` (+0x04): Сигнатура GPR (строго `0x19870423`).
  - `GPR[2]` (+0x08): Смещение базового адреса `BufferStruct` внутри BAR0.

### 2.2 Транспорт сообщений BCE (Bridge Controller Entity)
Взаимодействие с прошивкой чипа T2 в Windows происходит через драйвер `AppleUSBVHCI.sys` (`\Device\AppleUSBVHCI`):
- **IOCTL-код**: Исключительно **`0x222018`** (`CTL_CODE(FILE_DEVICE_UNKNOWN, 0x0806, METHOD_BUFFERED, FILE_ANY_ACCESS)`).
- **Формат пакета BCE**:
  ```c
  #pragma pack(push, 1)
  typedef struct _T2AUDIO_MSG_HEADER {
      CHAR Tag[4];        // Уникальный тег: "Audt" или инкрементный "S001"
      UCHAR Type;         // 1 = COMMAND, 2 = RESPONSE, 3 = NOTIFICATION
      ULONG64 DeviceId;   // Идентификатор подустройства T2 (0 для общих команд)
  } T2AUDIO_MSG_HEADER;   // Размер: 13 байт

  typedef struct _T2AUDIO_MSG_BASE {
      ULONG Message;      // Код команды / опкод
      ULONG Status;       // Статус протокола (0 = SUCCESS)
  } T2AUDIO_MSG_BASE;     // Размер: 8 байт
  #pragma pack(pop)
  ```

---

## 3. Матрица исправленных критических дефектов

Следующая таблица содержит все дефекты, найденные и устранённые в ходе аудита и реверс-инжиниринга:

| № | Дефект | Исходный файл | Симптом / Последствие | Причина и Решение |
|---|---|---|---|---|
| **1** | Ошибка кодов ответов `START_IO` / `STOP_IO` | [`Phase4.c`](../src/Phase4.c) | `STATUS_DEVICE_PROTOCOL_ERROR` при включении звука | Драйвер ждал ответ `0` и `2`. REA доказало, что T2 возвращает `1` (`START_IO_RESPONSE`) и `3` (`STOP_IO_RESPONSE`). Введены правильные макросы в [`BceProtocolLogic.h`](../src/BceProtocolLogic.h). |
| **2** | BSOD `0x7E` в `ks!DispatchDeviceIoControl` | [`BceTransport.c`](../src/BceTransport.c) | Мгновенный синий экран при попытке отправки команды BCE | Ручное присвоение `irpStack->FileObject = FileObject`. В `AppleAudio.sys` (`FUN_140004a4c`) `FileObject` строго `NULL`. Назначение `FileObject` отправляло IRP в стек `ksthunk`. Ручное присвоение удалено. **Поправка:** сборка с `FileObject = NULL` тоже падала (2026-10-08 01:35) — BSOD не устранён, см. `docs/CURRENT_STATE.md`. |
| **3** | Отказ Windows Audio Engine в создании потока | [`WaveRTMiniport.c`](../src/WaveRTMiniport.c) | Ошибка `0x80070057` / отказ WASAPI открывать пин 0 | Драйвер требовал `WAVE_FORMAT_PCM`. Для потоков >2 каналов Windows Audio Engine строго требует `WAVEFORMATEXTENSIBLE` (`0xFFFE`). Добавлена полная поддержка `KSDATAFORMAT_WAVEFORMATEXTENSIBLE` (6ch, 48kHz, 32-bit, mask 5.1). |
| **4** | Статическое копирование в смещение 0 (Тишина / щелчки) | [`WaveRTStream.c`](../src/WaveRTStream.c) | Звук пропадал через 10 мс; щелчок раз в 347 мс | Драйвер копировал 11.5 КБ в смещение 0 буфера 399 КБ. T2 циклически считывал 336 мс тишины. Разработан алгоритм `T2AudioCalculateCopyChunk`, циклически заполняющий BAR1 перед курсором T2 с опережением 15 мс. |
| **5** | Отказ запуска при обычной загрузке (Code 10) | [`Driver.c`](../src/Driver.c) | `STATUS_UNSUCCESSFUL` без стороннего скрипта | Запуск блокировался отсутствием волатильного ключа `TestSession`. **Поправка:** этого изменения в коде нет; гейт `TestSession` сохранён намеренно (см. `docs/DRIVER_GUIDE.md`, §8). |
| **6** | Гонка состояний (Race Condition) в таймере DPC | [`WaveRTStream.c`](../src/WaveRTStream.c) | Искажение курсора воспроизведения, рассинхронизация | Таймерный DPC (2 мс) и `SetState` несинхронно читали `AnchorQpc` и `AnchorFrames`. Добавлен спинлок `KSPIN_LOCK StreamLock`. |
| **7** | Зацикливание шума при остановке потока | [`WaveRTStream.c`](../src/WaveRTStream.c) | Жужжание/гул после нажатия Pause/Stop | Буфер BAR1 не обнулялся при остановке. Добавлено обнуление буфера динамиков в BAR1 при переходе в `KSSTATE_STOP`. |
| **8** | Опасный вызов `KeFlushQueuedDpcs()` на повышенном IRQL | [`WaveRTStream.c`](../src/WaveRTStream.c) | `BugCheck 0x4A` при остановке таймера | **Поправка:** такой проверки в коде нет; `KeFlushQueuedDpcs()` вызывается только из PASSIVE-колбэков (`SetState`, `FreeAudioBuffer`, teardown), что и обеспечивает корректность. |

---

## 4. Архитектура кольцевого DMA зеркалирования

### 4.1 Проблема PortCls и системного буфера
Windows PortCls (`PortWaveRT`) отображает буфер в память пользовательского процесса с помощью `MmMapLockedPagesSpecifyCache(..., UserMode, ...)`. Из-за специфики подсистемы MMIO Windows, прямое отображение физических страниц BAR1 в пользовательское пространство отклоняется с ошибкой `0x8007001F`.

### 4.2 Реализованное решение
1. **Системный циклический буфер (`SystemBufferVa`)**:
   Драйвер выделяет страницы обычной оперативной памяти через `PortStream->AllocatePagesForMdl` под размер запроса клиента (обычно 10–20 мс = 11 520 или 23 040 байт).
2. **Аппаратное кольцо T2 (`context->Bar1Mapped + context->SpeakerBufferOffset`)**:
   Имеет размер $S_{\text{hw}} = 399\,360$ байт.
3. **Алгоритм `T2AudioCalculateCopyChunk`**:
   Каждые 2 мс DPC-таймер вычисляет текущий фрейм воспроизведения $F_{\text{play}}$ на основе QPC и целевой фрейм с опережением:
   $$F_{\text{target}} = F_{\text{play}} + 720\text{ (15 ms)}$$
   Все промежуточные фреймы от $F_{\text{last}}$ до $F_{\text{target}}$ копируются линейными порциями (чанками):
   $$\text{SrcOffset} = (F \times 24) \pmod{S_{\text{client}}}$$
   $$\text{DstOffset} = (F \times 24) \pmod{S_{\text{hw}}}$$
   Обеспечивается гладкое циклическое сшивание на границах обоих буферов.

---

## 5. Инструкции по сборке, тестированию и отладке

### 5.1 Сборка драйвера ядра (`T2AudioMiniport.sys`)
Сборка осуществляется через стандартный инструментарий Visual Studio 2026 / MSBuild и Windows Driver Kit:
```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    "C:\Users\othysa\Desktop\mbp\T2AudioPort\src\T2AudioMiniport.vcxproj" `
    /p:Configuration=Release /p:Platform=x64 /t:Rebuild
```
Бинарник драйвера компилируется в:
`C:\Users\othysa\Desktop\mbp\T2AudioPort\src\bin\Release\T2AudioMiniport.sys`

### 5.2 Сборка и запуск модульных тестов
Модульные тесты не зависят от ядра и компилируются обычным компилятором MSVC `cl.exe`:
```cmd
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d "C:\Users\othysa\Desktop\mbp\T2AudioPort"

cl /nologo /W4 /Fe:tests\BceProtocolLogicTest.exe tests\BceProtocolLogicTest.c
tests\BceProtocolLogicTest.exe

cl /nologo /W4 /Fe:tests\T2AudioBufferLogicTest.exe tests\T2AudioBufferLogicTest.c
tests\T2AudioBufferLogicTest.exe
```
Оба теста должны завершаться с выводом `ALL PASSED (0 failures)`.

### 5.3 Безопасное тестирование на реальном оборудовании
1. **Тестовая подпись**: В Windows должна быть включена тестовая подпись (`bcdedit /set testsigning on`).
2. **Экстренный откат**: Проверенный скрипт [`tools/Restore-AppleAudioDriver.ps1`](../tools/Restore-AppleAudioDriver.ps1). В случае необходимости он мгновенно восстанавливает оригинальный `AppleAudio.sys`.
3. **Мониторинг отладочных сообщений**: Запустите утилиту DebugView (`C:\Users\othysa\Desktop\mbp\DebugView\Dbgview.exe`) с включенной опцией *Capture Kernel* для просмотра префиксов `T2Audio:`.

---

## 6. Связь с файлами и навигация

- **Анализ протокола AppleAudio**: [`docs/analysis/AppleAudio_Reverse_Engineering_Report.md`](analysis/AppleAudio_Reverse_Engineering_Report.md)
- **Индекс всех отчетов**: [`docs/INDEX.md`](INDEX.md)
- **Матрица статуса проекта**: [`CURRENT_STATE.md`](CURRENT_STATE.md)
- **Исходный код драйвера**:
  - Транспорт BCE: [`T2AudioPort/src/BceTransport.c`](../src/BceTransport.c)
  - Команды Start/Stop: [`T2AudioPort/src/Phase4.c`](../src/Phase4.c)
  - Поток WaveRT и таймер DMA: [`T2AudioPort/src/WaveRTStream.c`](../src/WaveRTStream.c)
  - Минипорт и валидация форматов: [`T2AudioPort/src/WaveRTMiniport.c`](../src/WaveRTMiniport.c)
  - Инициализация PnP и диспетчеризация: [`T2AudioPort/src/Driver.c`](../src/Driver.c)
  - Выделение BAR и поиск спикера: [`T2AudioPort/src/Device.c`](../src/Device.c)
