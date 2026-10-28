# Наушники (разъём 3,5 мм): план

## Как это устроено (KAIT2EN `modules/t2bce_audio`)

* Наушники — **отдельное BCE-устройство** с UID `"Codec Output"` (у динамиков — `"Speaker"`), со своим выходным потоком, своим кольцом в BufferStruct (`audio.c`: таблица `{"Speaker", 0}`, `{"Codec Output", 2}`) и своими START_IO/STOP_IO.
* Состояние разъёма — свойство устройства `scope 'outp'`, `selector 'jack'` (u32, 1 = вставлены), читается `GET_PROPERTY`.
* Изменения KAIT2EN получает подпиской `PROPERTY_LISTENER` (msg 11) на `'outp'/'jack'`; bridgeOS присылает `PROPERTY_CHANGED` (msg 13), после чего свойство читается заново.

## Шаги

1. **Сделано (2026-10-28, не проверено на железе):** при BCE-пробе драйвер находит `"Codec Output"` и пишет в лог
   `BCE probe: headphone jack device 0x… : PLUGGED/unplugged`. Это подтвердит UID, id устройства и чтение свойства.
2. **Отключать динамики при вставленных наушниках.** Подписка `PROPERTY_LISTENER`, обработка `PROPERTY_CHANGED` в обработчике `0x222020` (`T2AudioHandleVhciMessage`), при «вставлены» — тишина в кольцо динамиков. Логика без нового аудиотракта.
3. **Вторая аудиоточка «Наушники».** Второй WaveRT-фильтр в PortCls, кольцо `"Codec Output"` из BufferStruct (`T2AudioFindSpeakerBuffer` обобщить), его `'pft '`-формат, START_IO/STOP_IO для его id, свои часы (`UPDATE_TIMESTAMP` по `dev`), без DSP динамиков. Плюс `KSPROPERTY_JACK_DESCRIPTION`, чтобы Windows показывала «подключено/отключено».
4. Микрофоны — аналогично, входной поток (`set_input_stream_address_ranges` у KAIT2EN).

Шаги 2–4 — только после того, как шаг 1 покажет реальные данные с железа.
