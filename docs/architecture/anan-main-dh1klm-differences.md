# ANAN Features: DH1KLM anan-tx-implementation Branch



Main provides a single-receiver ANAN-G2 implementation. Our branch expands this into a model-aware OpenHPSDR implementation with multiple receivers, transmit support and additional hardware controls, while retaining the original G2 implementation as a separate selectable option.

| Feature | Main | DH1KLM anan-tx-implementation branch |
| --- | --- | --- |
| **Radio models** | ANAN-G2 / Saturn | Hermes, ANAN-10/10E, 100/100B, 100D, 200D, 7000DLE/8000DLE, G2, G2E, Anvelina-Pro III and Red Pitaya profiles |
| **Network protocols** | OpenHPSDR Protocol 2 | Protocol 1 and Protocol 2, selected according to the radio profile |
| **Discovery and connection** | G2 discovery and direct IP connection | P1/P2 discovery, model selection, saved device profiles and explicit model overrides |
| **G2 implementation selection** | Original G2 receiver | Separately selectable original and extended G2 implementations, with independent settings |
| **Receivers and panadapters** | One receiver and one panadapter | Multiple independently demodulated slices and multiple hardware receive windows, within each profile's resources |
| **ADC/DDC routing** | ADC0 or ADC1 for the G2 receiver | Model-specific DDC routing, live ADC assignment per capture and a graphical view of receiver/DDC/ADC connections |
| **Tuning and band views** | Live tuning and sample-rate-based display span | CTUN, regional band view, per-window display spans and stored band-view settings |
| **RF filters and attenuation** | G2 filter-bank and ADC attenuator controls | Model-specific HPF/BPF/LPF control, shared-filter coordination across receivers and ADC/ALEX attenuation |
| **Antenna control** | G2 receive-input selection | Live RX antenna routing, per-band RX antenna settings and separate TX antenna selection |
| **Hardware setup** | G2 front-end settings | Integrated model-aware setup covering connection, antenna/ALEX, ADC routing, OC outputs, calibration and transmit |
| **AGC-T** | Manual AGC-T adjustment | Manual adjustment plus automatic noise-floor-based AGC-T for each slice |
| **Receive calibration** | Relative receive levels and G2 droop correction | Reference-signal frequency and receive-level calibration, separate ADC corrections and hardware-specific calibration storage |
| **Transmit** | Receive-only ANAN implementation | P1/P2 transmit with USB/LSB, DIGU/DIGL, AM/SAM, DSB, FM, CW and TUNE; TX source selectable among open slices |
| **TX power and audio controls** | Receive-only ANAN implementation | Per-band PA gain and power limits, live PA-gain adjustment, remembered RF drive, live USB/LSB Low/High Cut, MON, compression and TX audio meters |
| **Digital transmit audio** | Receive-only ANAN implementation | TCI audio for external digital-mode applications, WSPR and AetherModem/AX.25 host-audio integration |
| **Hardware outputs and microphone options** | G2 front-end and speaker controls | RX band/SWL open-collector assignments, manual digital outputs and model-dependent microphone wiring, bias and PTT configuration |
| **Telemetry** | G2 voltage and link status | Model-dependent voltage/current display with configurable DIY sensors, plus forward/reflected RF power and SWR |
| **Front panels** | Standard Aether controls | Andromeda USB/COM and G2-V2 TCP integration, configurable mappings, standard Andromeda mapping and panel feedback |
| **PC receive audio** | Standard audio-device output | Standard output plus native Windows ASIO receive output, driver/channel/buffer selection and an active-status indicator |
