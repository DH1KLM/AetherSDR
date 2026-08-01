#include "core/VaraTurboCodec.h"

#include <cstdio>

using namespace AetherSDR::Vara;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

void testQppInterleaver()
{
    // Test that QPP interleaver is a true bijection (permutation)
    int k = 256;
    QVector<int> pi = TurboCodec::generateQppInterleaver(k);
    expect(pi.size() == k, "QPP interleaver length matches block size");

    QVector<bool> seen(k, false);
    bool validPermutation = true;
    for (int idx : pi) {
        if (idx < 0 || idx >= k || seen[idx]) {
            validPermutation = false;
            break;
        }
        seen[idx] = true;
    }
    expect(validPermutation, "QPP interleaver generates unique 1-to-1 permutation (no collisions)");
}

void testRate13Encoding()
{
    int k = 64;
    QVector<uint8_t> inBits(k, 1);
    QVector<uint8_t> coded = TurboCodec::encodeRate13(inBits);

    // 3 * K systematic/parity + 12 tail bits
    expect(coded.size() == 3 * k + 12, "Rate 1/3 codeword has 3*K + 12 tail bits");
}

void testPayloadEncoding()
{
    QByteArray payload = "AetherSDR VARA HF OFDM Turbo Test Payload";
    int targetBits = 1000;
    QVector<uint8_t> bits = TurboCodec::encodePayload(payload, targetBits);
    expect(bits.size() == targetBits, "Punctured codeword matches target bit length exactly");
}

} // namespace

int main()
{
    std::printf("=== Running vara_turbo_codec_test ===\n");
    testQppInterleaver();
    testRate13Encoding();
    testPayloadEncoding();

    std::printf("Summary: %d failures\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
