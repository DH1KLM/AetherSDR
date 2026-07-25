#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum AetherWdspRxMode
{
    AETHER_WDSP_RX_LSB = 0,
    AETHER_WDSP_RX_USB = 1,
    AETHER_WDSP_RX_DSB = 2,
    AETHER_WDSP_RX_CWL = 3,
    AETHER_WDSP_RX_CWU = 4,
    AETHER_WDSP_RX_FM = 5,
    AETHER_WDSP_RX_AM = 6,
    AETHER_WDSP_RX_DIGU = 7,
    AETHER_WDSP_RX_SPEC = 8,
    AETHER_WDSP_RX_DIGL = 9,
    AETHER_WDSP_RX_SAM = 10,
    AETHER_WDSP_RX_DRM = 11,
    AETHER_WDSP_RX_WBFM = 12
};

void OpenChannel(int channel, int inputSize, int dspSize, int inputSampleRate,
                 int dspSampleRate, int outputSampleRate, int type, int state,
                 double delayUp, double slewUp, double delayDown,
                 double slewDown, int blockForOutput);
void CloseChannel(int channel);
void fexchange2(int channel, float* inputI, float* inputQ,
                float* outputLeft, float* outputRight, int* error);
void SetRXAMode(int channel, int mode);
void SetRXABandpassFreqs(int channel, double lowHz, double highHz);
// Canonical passband setter. RXASetPassband() is what both reference clients
// (Thetis, pihpsdr) call: it sets the bandpass AND the SNBA output bandwidth
// AND the NBP stage. SetRXABandpassFreqs() alone touches only the first, which
// leaves the filter actually in circuit untouched — no sideband selection, so
// USB and LSB demodulate identically and filter edges have no audible effect.
void RXASetPassband(int channel, double lowHz, double highHz);
// The two stages RXASetPassband sets in addition to the bandpass, exposed
// separately so their effects can be attributed independently.
void RXANBPSetFreqs(int channel, double lowHz, double highHz);
void SetRXASNBAOutputBandwidth(int channel, double lowHz, double highHz);
void SetRXAAGCMode(int channel, int mode);
void SetRXAAGCTop(int channel, double maximumGainDb);
void SetTXAMode(int channel, int mode);
void SetTXABandpassFreqs(int channel, double lowHz, double highHz);
int GetWDSPVersion(void);

uint64_t wdspPortAllocationSequence(void);
uint64_t wdspPortOutstandingAllocations(void);

#ifdef __cplusplus
}
#endif
