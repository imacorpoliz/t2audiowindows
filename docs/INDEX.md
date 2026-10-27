# Навигатор по документации проекта (docs/INDEX.md)

В данном каталоге собрана полная техническая документация, отчеты реверс-инжиниринга и планы разработки для аудиосистемы MacBook Pro 16,1 на Windows 11.

---

## ⭐️ Главное руководство для разработчиков и агентов

| Документ | Назначение |
| :--- | :--- |
| **[AGENT_HANDOVER_GUIDE.md](file:///C:/Users/othysa/Desktop/mbp/docs/AGENT_HANDOVER_GUIDE.md)** | **Мастер-руководство по передаче контекста**: Аппаратная архитектура T2, спецификация протокола BCE, матрица всех исправленных критических дефектов, математика циклического кольцевого DMA зеркалирования, инструкции по сборке и тестам. |

---

## 1. Отчеты реверс-инжиниринга (docs/analysis/)

| Документ | Описание |
| :--- | :--- |
| [AppleAudio_Reverse_Engineering_Report.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/AppleAudio_Reverse_Engineering_Report.md) | **Главный отчет REA + Ghidra 12.1.4**: Декомпиляция `FUN_140004a4c` (шлюз `AppleUSBVHCI`), старт/стоп потока, распределение памяти `BufferStruct`, разгадка BSOD 0x7E. |
| [AppleAudio_BufferRegistration_Report.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/AppleAudio_BufferRegistration_Report.md) | Детальный анализ механизма регистрации буферов и колец DMA в `AppleAudio.sys`. |
| [AppleAudio_Message_Path.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/AppleAudio_Message_Path.md) | Трасса прохождения сообщений от верхнего уровня до шины PCI. |
| [AppleAudio_Playback_Path_Report.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/AppleAudio_Playback_Path_Report.md) | Архитектура тракта воспроизведения звука в оригинальном драйвере. |
| [AppleUSBVHCI_IOCTL_Analysis.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/AppleUSBVHCI_IOCTL_Analysis.md) | Предварительный анализ кодов IOCTL виртуального хост-контроллера. |
| [AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md) | Полный статический анализ IOCTL `0x222004`–`0x222018`, vtable `0x23F78`, CFG dispatch и ABCE дескрипторов. |
| [BUFFER_PATH_ANALYSIS.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/BUFFER_PATH_ANALYSIS.md) | Исследование путей передачи данных буфера между драйвером и T2. |
| [BUFFER_PATH_CORRECTIONS.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/BUFFER_PATH_CORRECTIONS.md) | Сравнение гипотез о буферах и корректировка архитектурных моделей. |
| [Function_46F8_Analysis.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/Function_46F8_Analysis.md) | Анализ `FUN_1400046F8`: проверка GPR-сигнатуры `0x19870423`, поиск BufferStruct в BAR0, регистрация устройств и стримов. |
| [IOCTL_222008_CHAIN.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/IOCTL_222008_CHAIN.md) | Трассировка цепочки вызовов IOCTL `0x222008` в `AppleUSBVHCI`. |
| [Linux_Windows_Structure_Mapping.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/Linux_Windows_Structure_Mapping.md) | Таблица соответствия структур Linux (`kait2en`) и структур Windows (`AppleAudio.sys`). |
| [T2_Audio_Protocol_Analysis.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/T2_Audio_Protocol_Analysis.md) | Базовый анализ бинарного протокола сообщений BCE Audio. |
| [TOPOLOGY_BLOCKER_REPORT.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/TOPOLOGY_BLOCKER_REPORT.md) | Анализ причин блокировки топологии PortCls (`STATUS_INVALID_DEVICE_STATE`) и решение с счетчиками пинов. |
| [TRANSPORT_DECISION.md](file:///C:/Users/othysa/Desktop/mbp/docs/analysis/TRANSPORT_DECISION.md) | Обоснование выбора транспортного механизма (IOCTL vs прямой BAR) для открытого драйвера. |

---

## 2. Планирование и рабочие журналы (docs/planning/)

| Документ | Описание |
| :--- | :--- |
| [PORT_PLAN.md](file:///C:/Users/othysa/Desktop/mbp/docs/planning/PORT_PLAN.md) | Пошаговый план портирования драйвера из Linux в архитектуру Windows WDM/WaveRT. |
| [T2_Windows_Implementation_Guide.md](file:///C:/Users/othysa/Desktop/mbp/docs/planning/T2_Windows_Implementation_Guide.md) | Практическое руководство по реализации драйвера для Windows 10/11. |
| [VERIFICATION_SUMMARY.md](file:///C:/Users/othysa/Desktop/mbp/docs/planning/VERIFICATION_SUMMARY.md) | Сводка результатов тестирования и верификации на этапах проекта. |
| [FINAL_REANALYSIS_REPORT.md](file:///C:/Users/othysa/Desktop/mbp/docs/planning/FINAL_REANALYSIS_REPORT.md) | Итоговый ретроспективный отчет повторного анализа драйверного стека. |
| [CURRENT_STATE.md](file:///C:/Users/othysa/Desktop/mbp/docs/planning/CURRENT_STATE.md) | Снимок состояния кодовой базы и нерешенных задач на октябрь 2026. |
| [CURRENT_STATUS.md](file:///C:/Users/othysa/Desktop/mbp/docs/planning/CURRENT_STATUS.md) | Промежуточный статус-отчет по стадиям разработки. |
| [WORK_LOG.md](file:///C:/Users/othysa/Desktop/mbp/docs/planning/WORK_LOG.md) | Подробный хронологический журнал выполнения работ. |
| [NOTES.md](file:///C:/Users/othysa/Desktop/mbp/docs/planning/NOTES.md) | Заметки разработчика по аппаратуре, форматам и ограничениям T2. |
| [DISASM_EVIDENCE.md](file:///C:/Users/othysa/Desktop/mbp/docs/planning/DISASM_EVIDENCE.md) | Сводка ассемблерных доказательств по ключевым веткам кода. |
| [RESEARCH_NOTES_INITIAL.md](file:///C:/Users/othysa/Desktop/mbp/docs/planning/RESEARCH_NOTES_INITIAL.md) | Первоначальные рабочие заметки исследователя по 6-канальному звуку. |

---

## 3. Первичные дампы (evidence/traces/)

Каталог `evidence/traces/` содержит 36 текстовых файлов дизассемблерных выборок, таблиц виртуальных методов (`vtable_*.txt`), отчетов зондирования (`trace_*.txt`) и дампов функций (`func_*.txt`), использовавшихся в качестве исходных доказательств при составлении отчетов.
