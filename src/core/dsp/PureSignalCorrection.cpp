#include "PureSignalCorrection.h"

#include <QList>
#include <cmath>
#include <limits>

namespace AetherSDR {
namespace {
class Reader {
public:
    explicit Reader(const QByteArray& bytes) : m_tokens(bytes.simplified().split(' ')) {}
    bool tag(const char* expected)
    {
        return m_position < m_tokens.size() && m_tokens[m_position++] == expected;
    }
    bool number(double& value)
    {
        if (m_position >= m_tokens.size()) { return false; }
        bool ok = false;
        value = m_tokens[m_position++].toDouble(&ok);
        return ok && std::isfinite(value) && std::abs(value) <= 1e12;
    }
    bool integer(int& value, int low, int high)
    {
        if (m_position >= m_tokens.size()) { return false; }
        bool ok = false;
        const qlonglong parsed = m_tokens[m_position++].toLongLong(&ok);
        if (!ok || parsed < low || parsed > high) { return false; }
        value = static_cast<int>(parsed);
        return true;
    }
    bool end() const { return m_position == m_tokens.size(); }
private:
    QList<QByteArray> m_tokens;
    qsizetype m_position = 0;
};
} // namespace

bool validatePureSignalCorrection(const QByteArray& document, QString* error)
{
    const auto reject = [error] {
        if (error) { *error = QStringLiteral("Invalid WDSP v2 correction: check format, finite values, branch limits and checksum."); }
        return false;
    };
    if (document.isEmpty() || document.size() > 1024 * 1024 || document.contains('\0')) { return reject(); }
    Reader input(document);
    int version = 0;
    if (!input.tag("correction_file_version") || !input.integer(version,2,2)) { return reject(); }
    double checksum = 0;
    int totalPoints = 0;
    for (const char* curve : {"MAG","COS","SIN"}) {
        if (!input.tag("curve") || !input.tag(curve)) { return reject(); }
        double alpha = 0, alphaLow = 0, boundary = 0, clipLow = 0, clipHigh = 0;
        int count = 0, warmup = 0, samples = 0;
        if (!input.tag("curve_ema_alpha") || !input.number(alpha) || alpha < 0 || alpha > 1
            || !input.tag("curve_ema_alpha_lo") || !input.number(alphaLow) || alphaLow < 0 || alphaLow > 1
            || !input.tag("curve_ema_x_bnd") || !input.number(boundary) || boundary < 0 || boundary > 1
            || !input.tag("curve_ema_clip") || !input.number(clipLow) || !input.number(clipHigh)
            || clipLow > clipHigh || clipLow < -20 || clipHigh > 20
            || !input.tag("curve_ema_count") || !input.integer(count,0,std::numeric_limits<int>::max())
            || !input.tag("curve_ema_warmup") || !input.integer(warmup,0,std::numeric_limits<int>::max())
            || !input.tag("curve_ema_pts") || !input.integer(samples,256,256)) { return reject(); }
        checksum += warmup;
        checksum += alpha + alphaLow + boundary + clipLow + clipHigh + count;
        for (int i = 0; i < samples; ++i) {
            double value = 0;
            if (!input.number(value) || std::abs(value) > 20
                || (curve[0] == 'M' && value <= 0)) { return reject(); }
            checksum += value;
        }
        int branches = 0;
        if (!input.tag("n_branches") || !input.integer(branches,1,16)) { return reject(); }
        checksum += branches;
        for (int branch = 0; branch < branches; ++branch) {
            int index = 0, points = 0;
            double midpoint = 0;
            if (!input.tag("branch") || !input.integer(index,branch,branch)
                || !input.tag("n_pts") || !input.integer(points,0,4096) || points == 1
                || !input.tag("t_mid") || !input.number(midpoint) || midpoint < 0 || midpoint > 1
                || (totalPoints += points) > 16384) { return reject(); }
            checksum += points + midpoint;
            double previous = -std::numeric_limits<double>::infinity();
            for (int i = 0; i < points; ++i) {
                double x = 0, y = 0;
                if (!input.number(x) || !input.number(y) || x <= previous
                    || std::abs(x) > 20 || std::abs(y) > 20) { return reject(); }
                previous = x;
                checksum += x + y;
            }
        }
    }
    double stored = 0;
    if (!input.tag("checksum") || !input.number(stored) || !input.end()
        || std::abs(stored-checksum) > std::abs(checksum)*1e-9 + 1e-9) { return reject(); }
    if (error) { error->clear(); }
    return true;
}
} // namespace AetherSDR
