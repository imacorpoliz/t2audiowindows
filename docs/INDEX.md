# Навигатор по документации проекта (docs/INDEX.md)

В данном каталоге собрана полная техническая документация, отчеты реверс-инжиниринга и планы разработки для аудиосистемы MacBook Pro 16,1 на Windows 11.

---

## ⭐️ Главное руководство для разработчиков и агентов

| Документ | Назначение |
| :--- | :--- |
| **[DRIVER_GUIDE.md](DRIVER_GUIDE.md)** | **Подробное руководство по текущему коду**: железо T2, протокол BCE, архитектура, потоковый тракт, синхронизация, стадии и реестр, сборка/CI, тест на железе, справочник по логу, известные проблемы. |
| **[REVERSE_PLAN.md](REVERSE_PLAN.md)** | План реверса для Gemini: задачи R1–R9 (минидампы, тег сообщений, ответ на `UPDATE_TIMESTAMP`, jack, инициализация, часы, формат/каналы, питание). |
| **[analysis/RE_appleaudio_protocol.md](analysis/RE_appleaudio_protocol.md)** | Полный набор команд AppleAudio → T2, handshake и remote access в AppleUSBVHCI (`mode 1`), формат сэмплов S24_LE из цикла копирования, DSP на CPU, jack, выбор получателя `0x222020`, питание (R8), msg 28 (список controls). |
| **[analysis/RE_speaker_layout_and_clock.md](analysis/RE_speaker_layout_and_clock.md)** | Часы T2 (`UPDATE_TIMESTAMP` = QPC-нс хоста, модель опережения AppleAudio 1000 ± 500) и раскладка 6 динамиков MacBookPro16,1 (вуферы 0/1 и 3/4, твитеры 2/5); что сделано в коде. |
| **[analysis/START_IO_BSOD_ROOT_CAUSE.md](analysis/START_IO_BSOD_ROOT_CAUSE.md)** | Корневая причина BSOD `0x7E` при `START_IO` (IOCTL `0x222020` от AppleUSBVHCI) и исправление. |
| **[CURRENT_STATE.md](CURRENT_STATE.md)** | Единственный авторитетный статус-документ с хронологией и доказательствами с железа. |
| **[AGENT_HANDOVER_GUIDE.md](AGENT_HANDOVER_GUIDE.md)** | **Мастер-руководство по передаче контекста**: Аппаратная архитектура T2, спецификация протокола BCE, матрица всех исправленных критических дефектов, математика циклического кольцевого DMA зеркалирования, инструкции по сборке и тестам. |

---

## 1. Отчеты реверс-инжиниринга (docs/analysis/)

| Документ | Описание |
| :--- | :--- |
| **[RE_minidump.md](analysis/RE_minidump.md)** | **Задача R1 (ДОКАЗАНО)**: Подтверждение причины BSOD по минидампам ядра (`ks!DispatchDeviceIoControl+0x15`, `FileObject == NULL`, вызов из `AppleUSBVHCI+0x259a`). |
| **[RE_tag_semantics.md](analysis/RE_tag_semantics.md)** | **Задача R2 (ДОКАЗАНО)**: Семантика поля `tag` (`1..127` в `AppleAudio.sys`), механизм синхронного ожидания `0x222018` в `AppleUSBVHCI.sys` без фильтрации по тегу. |
| **[RE_update_timestamp.md](analysis/RE_update_timestamp.md)** | **Задача R3 (ДОКАЗАНО)**: Доказательство отправки `UPDATE_TIMESTAMP_RESPONSE (34)` самим `AppleUSBVHCI.sys` (`0x140001DE8`); аудиодрайвер не должен отправлять ответ. |
| [AppleAudio_Reverse_Engineering_Report.md](analysis/AppleAudio_Reverse_Engineering_Report.md) | **Главный отчет REA + Ghidra 12.1.4**: Декомпиляция `FUN_140004a4c` (шлюз `AppleUSBVHCI`), старт/стоп потока, распределение памяти `BufferStruct`, разгадка BSOD 0x7E. |
| [AppleAudio_BufferRegistration_Report.md](analysis/AppleAudio_BufferRegistration_Report.md) | Детальный анализ механизма регистрации буферов и колец DMA в `AppleAudio.sys`. |
| [AppleAudio_Message_Path.md](analysis/AppleAudio_Message_Path.md) | Трасса прохождения сообщений от верхнего уровня до шины PCI. |
| [AppleAudio_Playback_Path_Report.md](analysis/AppleAudio_Playback_Path_Report.md) | Архитектура тракта воспроизведения звука в оригинальном драйвере. |
| [AppleUSBVHCI_IOCTL_Analysis.md](analysis/AppleUSBVHCI_IOCTL_Analysis.md) | Предварительный анализ кодов IOCTL виртуального хост-контроллера. |
| [AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md](analysis/AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md) | Полный статический анализ IOCTL `0x222004`–`0x222018`, vtable `0x23F78`, CFG dispatch и ABCE дескрипторов. |
| [BUFFER_PATH_ANALYSIS.md](analysis/BUFFER_PATH_ANALYSIS.md) | Исследование путей передачи данных буфера между драйвером и T2. |
| [BUFFER_PATH_CORRECTIONS.md](analysis/BUFFER_PATH_CORRECTIONS.md) | Сравнение гипотез о буферах и корректировка архитектурных моделей. |
| [Function_46F8_Analysis.md](analysis/Function_46F8_Analysis.md) | Анализ `FUN_1400046F8`: проверка GPR-сигнатуры `0x19870423`, поиск BufferStruct в BAR0, регистрация устройств и стримов. |
| [IOCTL_222008_CHAIN.md](analysis/IOCTL_222008_CHAIN.md) | Трассировка цепочки вызовов IOCTL `0x222008` в `AppleUSBVHCI`. |
| [Linux_Windows_Structure_Mapping.md](analysis/Linux_Windows_Structure_Mapping.md) | Таблица соответствия структур Linux (`kait2en`) и структур Windows (`AppleAudio.sys`). |
| [T2_Audio_Protocol_Analysis.md](analysis/T2_Audio_Protocol_Analysis.md) | Базовый анализ бинарного протокола сообщений BCE Audio. |
| [TOPOLOGY_BLOCKER_REPORT.md](analysis/TOPOLOGY_BLOCKER_REPORT.md) | Анализ причин блокировки топологии PortCls (`STATUS_INVALID_DEVICE_STATE`) и решение с счетчиками пинов. |
| [TRANSPORT_DECISION.md](analysis/TRANSPORT_DECISION.md) | Обоснование выбора транспортного механизма (IOCTL vs прямой BAR) для открытого драйвера. |

---

## 2. Архив (docs/archive/)

Устаревшие документы ранних этапов (октябрь 2026, до прохождения стадий B–D на железе). Хранятся только для истории: их выводы могут противоречить текущему коду. Актуальный статус — только в [CURRENT_STATE.md](CURRENT_STATE.md).

| Каталог / документ | Что там |
| :--- | :--- |
| [archive/planning/](archive/planning/) | Первоначальный план портирования, рабочие журналы, ранние статус-отчёты. |
| [archive/research/](archive/research/) | Исходники ранних утилит разбора BufferStruct и кольца (бинарники удалены). |
| [archive/DEBUGGING_LOG.md](archive/DEBUGGING_LOG.md) | Журнал отладки до 2026-10-07 (MapResources, BAR, топология). |
| `archive/BAR_MAPPING_*`, `MAPRESOURCES_ROOT_CAUSE`, `RESOURCE_ANALYSIS`, `PHASE3_*`, `PHASE4_*`, `SESSION_COMPLETE`, `DIAGNOSTIC_SESSION_20261005`, `TOPOLOGY_BLOCKER_REPORT_20261006` | Разбор ранних блокеров запуска устройства — все решены. |
| `archive/INSTALLATION_SUMMARY`, `PHASE*_COMPLETE`, `PROJECT_COMPLETE` | Ранние отчёты «готово», **не подтверждённые** железом. |

---

## 3. Первичные дампы (evidence/traces/)

Каталог `evidence/traces/` (в репозиторий не входит, хранится локально) содержал 36 текстовых файлов дизассемблерных выборок, таблиц виртуальных методов (`vtable_*.txt`), отчетов зондирования (`trace_*.txt`) и дампов функций (`func_*.txt`), использовавшихся в качестве исходных доказательств при составлении отчетов.
