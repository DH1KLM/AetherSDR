# AetherSDR — DH1KLM Fork Extensions

This unofficial fork extends [AetherSDR](https://github.com/aethersdr/AetherSDR) v26.10.4 into a broader, model-aware OpenHPSDR implementation with multiple receivers, transmit support, additional hardware controls, diversity reception and Pure Signal.

The following list follows implementation order. It covers feature additions and bug fixes developed in this fork, excluding internal refactoring and features inherited from upstream.

Hardware availability depends on the selected model and protocol. Implementation does not imply hardware validation on every supported profile.

## Supported transceivers

The DH1KLM implementation supports the following OpenHPSDR model profiles. Discovery identifies the board family rather than the exact transceiver. Select the appropriate **Receive profile** when connecting; supported DIY combinations can use an explicit **Board override**.

| Board (P2 discovery ID) | Models / profiles | Protocols | First RX DDC¹ | ADCs | RF filters | Pure Signal² | Microphone connector options | Supply telemetry | Hardware verification³ |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **Hermes (1)** | Hermes, ANAN-10, ANAN-100 | P1 / P2 | DDC0 | 1 | ALEX HPF + LPF | — | — | DIY configuration | Pending |
| **Hermes II (2)** | ANAN-10E, ANAN-100B | P1 / P2 | DDC0 | 1 | ALEX HPF + LPF | — | — | DIY configuration | Pending |
| **Angelia (3)** | ANAN-100D | P1 / P2 | DDC2 | 2 | ALEX HPF + LPF | Experimental | — | DIY configuration | Pending |
| **Orion (4)** | ANAN-200D | P1 / P2 | DDC2 | 2 | ALEX HPF + LPF | Experimental | Mic PTT, tip/ring, bias | DIY configuration | DIY override tested; native hardware pending |
| **Orion MkII (5)** | Orion MkII, ANAN-7000DLE, ANAN-8000DLE, Anvelina Pro III | P1 / P2 | DDC2 | 2 | RX1/RX2 BPF + model-dependent LPF | Experimental | Mic PTT, tip/ring, bias | Voltage + current⁴ | ANAN-7000DLE P2 RX confirmed; Anvelina configurations tested |
| **Saturn (10)** | ANAN-G2, ANAN-G2 1K | P2 | DDC0 | 2 | RX1/RX2 BPF | Experimental | Mic PTT, tip/ring, bias, 3.5 mm/XLR | Voltage + current | Original single-receiver RX path bench-qualified; extended features require validation |
| **HermesC10 (20)** | ANAN-G2E | P1 / P2 | DDC0 | 1 | BPF + LPF | — | — | Voltage + current | Pending |
| **Profile-dependent (1 or 5)** | Red Pitaya | P1 / P2 | DDC2 | 2 | Profile-dependent; native profile uses legacy HPF/LPF control | Experimental | — | DIY configuration | P1 RX, including 384 ksps, confirmed; P2 pending |

¹ **First RX DDC** describes the normal Protocol 2 receive route. Protocol 1 uses a different slot layout; two-ADC profiles normally start RX1 at DDC0. These values do not indicate the number of available receivers.

² **Pure Signal** is implemented experimentally for validated two-ADC profiles. It requires a prepared TX session and suitable attenuated PA feedback. P1 requires 192 kHz reception with no more than two receiver streams; P2 requires two spare physical DDCs. Diversity must be disabled. Hardware correction performance remains to be verified per configuration.

³ **Hardware verification** describes documented checks, not complete qualification of every mode, transmitter, peripheral or model in the row.

⁴ Automatic Orion MkII discovery is ambiguous. Voltage/current telemetry requires an applicable explicit model profile or DIY sensor configuration.

**Not enabled through this implementation:** Atlas/Metis (ID 0), Hermes-Lite 2 P2 (ID 6), and the unverified Saturn MkII identity (ID 11). Hermes-Lite 2 uses AetherSDR’s separate P1 backend.


**Please note that any testing you perform with this version is done at your own risk.**

## Feature additions

1. **Expanded OpenHPSDR discovery**  
   Recognizes additional OpenHPSDR board identities and their hardware resources.

2. **Automatic Saturn receive-filter selection**  
   Selects the appropriate receive band-pass filter from the tuned frequency.

3. **Live receive-antenna selection**  
   Switches supported RX antenna connections during operation and remembers the selected antenna.

4. **Model-aware Protocol 1 and Protocol 2 support**  
   Adds receive profiles for Hermes, ANAN-10/10E, ANAN-100/100B, ANAN-100D, ANAN-200D, ANAN-7000DLE/8000DLE, G2/G2E, Anvelina-Pro III and Red Pitaya, using the applicable protocol.

5. **Manual model selection and board overrides**  
   Allows direct-IP connections to use an explicitly selected model profile, including supported DIY hardware combinations, with saved per-device assignments.

6. **On-demand local discovery**  
   Adds an explicit scan button for searching the local network again.

7. **Integrated hardware setup**  
   Organizes model-specific hardware settings into Nereus-inspired tabs covering filters, antennas, ADCs and other hardware options.

8. **Configurable ALEX filter banks**  
   Provides model-dependent HPF, BPF and LPF configuration, bypass controls, active-filter indicators and independent Alex-2 receive ranges.

9. **Dedicated receive-input routing**  
   Exposes the separate RX inputs and antenna paths available on each model.

10. **ALEX attenuation and preamplifier presets**  
    Adds model-aware front-end attenuation and preamplifier settings.

11. **CTUN tuning**  
    Allows receiver tuning within the captured spectrum while keeping the capture center fixed.

12. **Regional band views**  
    Adds regional band markings and a spectrum view fitted to the selected band.

13. **Per-band tuning-view memory**  
    Remembers CTUN and band-view preferences separately for each band.

14. **Multiple receivers and independent receive windows**  
    Supports multiple independently demodulated slices within a capture and additional hardware receive windows, within the selected model’s resources.

15. **Live sample-rate selection**  
    Changes the receive capture rate and available spectrum span during operation.

16. **Per-band receive antennas**  
    Stores and recalls the selected RX antenna when changing bands.

17. **Band and SWL open-collector outputs**  
    Adds separate output assignments for amateur bands and shortwave-listening ranges.

18. **Manual digital outputs**  
    Provides direct operator control of supported hardware output pins.

19. **Receive frequency and level calibration**  
    Adds reference-based frequency and signal-level calibration, including separate ADC corrections and hardware-specific storage.

20. **Native Windows ASIO receive audio**  
    Adds ASIO speaker output with driver, clock and buffer settings.

21. **Separate original and extended Saturn implementations**  
    Keeps the original Saturn/G2 receiver and the DH1KLM implementation independently selectable, with separate implementation-owned settings.

22. **Automatic AGC-T per receiver**  
    Adjusts each slice’s AGC threshold using its estimated noise floor.

23. **Model-specific microphone hardware settings**  
    Adds applicable microphone wiring, bias and PTT configuration options.

24. **Protocol 2 transmit foundation**  
    Adds transmit IQ encoding, SSB DSP, packet pacing and an authorization-controlled transmit path.

25. **Per-band PA profiles and power limits**  
    Stores PA gain and operator power limits by band, model and protocol.

26. **Extended Protocol 1 and Protocol 2 transmission**  
    Adds transmit paths for USB/LSB, DIGU/DIGL, AM/SAM, DSB, FM, CW and TUNE.

27. **Digital transmit-audio integration**  
    Connects TCI audio, WSPR and AetherModem/AX.25 host audio to the ANAN transmitter.

28. **RF power and SWR telemetry**  
    Adds model-dependent forward-power, reflected-power and SWR readings, together with band-specific drive recall.

29. **External front-panel integration**  
    Adds Andromeda USB/serial and G2-V2 TCP support, including control mappings, softkey menus and panel feedback.

30. **Supply-current and DIY sensor configuration**  
    Adds model-dependent current telemetry and configurable sensor parameters for supported DIY hardware.

31. **Transmit selection across multiple receivers**  
    Allows an eligible slice to become the TX source independently of the displayed receive windows.

32. **Live PA gain and ADC routing**  
    Adds live PA-gain adjustment, ADC assignment per capture and a graphical receiver/DDC/ADC routing view.

33. **Live transmit-filter controls**  
    Allows USB/LSB low-cut and high-cut adjustment during operation.

34. **Expanded transmit-audio controls and monitoring**  
    Adds compression controls, TX audio meters, adjustment sliders and local monitoring of processed transmit audio.

35. **ASIO microphone input**  
    Adds microphone capture through ASIO, including input-channel selection and active-status indication.

36. **Coherent IQ diversity reception**  
    Combines compatible receive paths with adjustable phase and weighting.

37. **Standalone phasing controls**  
    Provides a dedicated window for adjusting the diversity combination.

38. **Pure Signal predistortion**  
    Adds a transmit-feedback and correction path intended to reduce transmitter distortion.

39. **Dedicated Pure Signal workspace**  
    Brings correction controls, feedback status and diagnostics into one operating window.

40. **TX-DSP two-tone generator**  
    Generates a two-tone test signal through the transmit DSP for adjustment and signal-quality assessment.

41. **Pure Signal feedback attenuation**  
    Coordinates attenuation of the feedback signal used by the correction path.

42. **Paired spectrum and distortion measurements**  
    Compares transmit-reference and feedback spectra and displays derived distortion measurements.

43. **Live Pure Signal feedback diagnostics**  
    Shows feedback routing and signal activity, with shortcuts to relevant hardware settings.
	
	License: GPL v3, same as upstream (see LICENSE).
