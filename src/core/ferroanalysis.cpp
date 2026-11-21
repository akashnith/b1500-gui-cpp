#include "ferroanalysis.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ferro {
namespace {

double medianOf(QVector<double> values)
{
    if (values.isEmpty())
        return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    const int n = values.size();
    return n % 2 ? values.at(n / 2) : 0.5 * (values.at(n / 2 - 1) + values.at(n / 2));
}

//! Element of \a values nearest \a reference; median when reference is NaN.
double pickNearest(const QVector<double> &values, double reference)
{
    if (values.isEmpty())
        return std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(reference))
        return medianOf(values);
    double best = values.first();
    double bestDistance = std::abs(best - reference);
    for (const double value : values) {
        const double distance = std::abs(value - reference);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = value;
        }
    }
    return best;
}

}  // namespace

double slope(const QVector<double> &x, const QVector<double> &y)
{
    const int n = std::min(x.size(), y.size());
    if (n < 2)
        return 0.0;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (int i = 0; i < n; ++i) {
        sx += x.at(i);
        sy += y.at(i);
        sxx += x.at(i) * x.at(i);
        sxy += x.at(i) * y.at(i);
    }
    const double denominator = n * sxx - sx * sx;
    if (std::abs(denominator) < 1.0e-300)
        return 0.0;
    return (n * sxy - sx * sy) / denominator;
}

QVector<double> cumulativeCharge(const QVector<double> &t, const QVector<double> &current,
                                 bool removeDrift, double dt)
{
    const int n = std::min(t.size(), current.size());
    QVector<double> charge(n, 0.0);
    if (n == 0)
        return charge;

    if (dt > 0.0) {
        // Uniform box-averaged samples: exact running sum with a half-sample
        // back-shift so the charge is referenced to the sample centre.
        double running = 0.0;
        for (int i = 0; i < n; ++i) {
            running += current.at(i) * dt;
            charge[i] = running - 0.5 * current.at(i) * dt;
        }
    } else {
        charge[0] = 0.0;
        for (int i = 1; i < n; ++i)
            charge[i] = charge.at(i - 1) +
                        0.5 * (current.at(i) + current.at(i - 1)) * (t.at(i) - t.at(i - 1));
    }

    if (removeDrift && n > 1 && t.at(n - 1) > t.at(0)) {
        // Net charge over a closed loop must be zero; whatever remains is the
        // DC leakage ramp. Remove it linearly in time.
        const double total = charge.at(n - 1) + (dt > 0.0 ? 0.5 * current.at(n - 1) * dt : 0.0);
        const double span = t.at(n - 1) - t.at(0);
        for (int i = 0; i < n; ++i)
            charge[i] -= total * (t.at(i) - t.at(0)) / span;
    }
    return charge;
}

QVector<double> polarization(const QVector<double> &charge, double areaCm2, bool centre)
{
    QVector<double> p(charge.size(), 0.0);
    if (charge.isEmpty() || !(areaCm2 > 0.0))
        return p;
    for (int i = 0; i < charge.size(); ++i)
        p[i] = charge.at(i) / areaCm2 * 1.0e6;  // C/cm^2 -> uC/cm^2
    if (centre) {
        const auto bounds = std::minmax_element(p.cbegin(), p.cend());
        const double offset = 0.5 * (*bounds.first + *bounds.second);
        for (double &value : p)
            value -= offset;
    }
    return p;
}

QVector<bool> risingMask(const QVector<double> &v)
{
    const int n = v.size();
    QVector<bool> mask(n, true);
    if (n < 2)
        return mask;

    QVector<double> sign(n - 1, 0.0);
    for (int i = 0; i < n - 1; ++i) {
        const double d = v.at(i + 1) - v.at(i);
        sign[i] = d > 0.0 ? 1.0 : (d < 0.0 ? -1.0 : 0.0);
    }
    // Fill leading zeros with the first real direction, then let each flat
    // stretch inherit the direction before it.
    double first = 0.0;
    for (const double s : sign)
        if (s != 0.0) { first = s; break; }
    if (first == 0.0)
        first = 1.0;
    for (int i = 0; i < sign.size(); ++i) {
        if (sign.at(i) == 0.0)
            sign[i] = i == 0 ? first : sign.at(i - 1);
    }
    for (int i = 0; i < n - 1; ++i)
        mask[i] = sign.at(i) > 0.0;
    mask[n - 1] = mask.at(n - 2);
    return mask;
}

QVector<double> crossValues(const QVector<double> &x, const QVector<double> &y,
                            double level, const QVector<bool> &mask, int direction)
{
    QVector<double> out;
    const int n = std::min({x.size(), y.size(), mask.size()});
    for (int i = 0; i + 1 < n; ++i) {
        if (!mask.at(i))
            continue;
        const double x0 = x.at(i);
        const double x1 = x.at(i + 1);
        if ((x0 - level) * (x1 - level) > 0.0 || x1 == x0)
            continue;
        if (direction > 0 && !(x1 > x0))
            continue;
        if (direction < 0 && !(x1 < x0))
            continue;
        const double f = (level - x0) / (x1 - x0);
        out.append(y.at(i) + f * (y.at(i + 1) - y.at(i)));
    }
    return out;
}

void branchDifference(const QVector<double> &v, const QVector<double> &current,
                      const QVector<double> &vref, QVector<double> *voltageOut,
                      QVector<double> *differenceOut)
{
    if (voltageOut)
        voltageOut->clear();
    if (differenceOut)
        differenceOut->clear();
    const int n = std::min({v.size(), current.size(), vref.size()});
    if (n < 6)
        return;

    const QVector<bool> rise = risingMask(vref);
    QVector<double> vRise, iRise, vFall, iFall;
    for (int i = 0; i < n; ++i) {
        if (rise.at(i)) { vRise.append(v.at(i)); iRise.append(current.at(i)); }
        else            { vFall.append(v.at(i)); iFall.append(current.at(i)); }
    }
    if (vRise.size() < 3 || vFall.size() < 3)
        return;

    // Sort the falling branch by voltage so it can be interpolated.
    QVector<int> order(vFall.size());
    for (int i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [&vFall](int a, int b) { return vFall.at(a) < vFall.at(b); });
    QVector<double> vFallSorted, iFallSorted;
    vFallSorted.reserve(order.size());
    iFallSorted.reserve(order.size());
    for (const int index : order) {
        vFallSorted.append(vFall.at(index));
        iFallSorted.append(iFall.at(index));
    }

    // Rising branch ascending, so the output is monotonic in V.
    QVector<int> riseOrder(vRise.size());
    for (int i = 0; i < riseOrder.size(); ++i)
        riseOrder[i] = i;
    std::sort(riseOrder.begin(), riseOrder.end(),
              [&vRise](int a, int b) { return vRise.at(a) < vRise.at(b); });

    for (const int index : riseOrder) {
        const double voltage = vRise.at(index);
        // Linear interpolation of the falling branch at this voltage.
        double interpolated = iFallSorted.first();
        if (voltage <= vFallSorted.first()) {
            interpolated = iFallSorted.first();
        } else if (voltage >= vFallSorted.last()) {
            interpolated = iFallSorted.last();
        } else {
            const auto it = std::lower_bound(vFallSorted.cbegin(), vFallSorted.cend(), voltage);
            const int hi = static_cast<int>(it - vFallSorted.cbegin());
            const int lo = std::max(0, hi - 1);
            const double span = vFallSorted.at(hi) - vFallSorted.at(lo);
            const double f = std::abs(span) < 1.0e-300
                ? 0.0 : (voltage - vFallSorted.at(lo)) / span;
            interpolated = iFallSorted.at(lo) + f * (iFallSorted.at(hi) - iFallSorted.at(lo));
        }
        if (voltageOut)
            voltageOut->append(voltage);
        if (differenceOut)
            differenceOut->append(iRise.at(index) - interpolated);
    }
}

LoopMetrics analyzeLoop(const QVector<double> &vIn, const QVector<double> &pIn,
                        const QVector<double> &currentIn, const QVector<double> &vrefIn,
                        double thicknessNm)
{
    LoopMetrics m;
    const int n = std::min({vIn.size(), pIn.size(), currentIn.size(), vrefIn.size()});
    if (n < 4)
        return m;

    // Close the seam so the last-to-first transition is a real segment.
    QVector<double> v = vIn.mid(0, n);
    QVector<double> p = pIn.mid(0, n);
    QVector<double> current = currentIn.mid(0, n);
    QVector<double> vref = vrefIn.mid(0, n);
    v.append(v.first());
    p.append(p.first());
    current.append(current.first());
    vref.append(vref.first());

    const QVector<bool> rise = risingMask(vref);
    QVector<bool> fall(rise.size());
    for (int i = 0; i < rise.size(); ++i)
        fall[i] = !rise.at(i);

    // Voltage at the switching-current extremes, used to disambiguate Vc.
    double vSwitchPlus = std::numeric_limits<double>::quiet_NaN();
    double vSwitchMinus = std::numeric_limits<double>::quiet_NaN();
    double bestPlus = -std::numeric_limits<double>::infinity();
    double bestMinus = std::numeric_limits<double>::infinity();
    for (int i = 0; i < current.size(); ++i) {
        if (rise.at(i) && current.at(i) > bestPlus) {
            bestPlus = current.at(i);
            vSwitchPlus = v.at(i);
        }
        if (fall.at(i) && current.at(i) < bestMinus) {
            bestMinus = current.at(i);
            vSwitchMinus = v.at(i);
        }
    }

    m.prPlus = medianOf(crossValues(v, p, 0.0, fall, -1));
    m.prMinus = medianOf(crossValues(v, p, 0.0, rise, +1));
    if (std::isfinite(m.prPlus) && std::isfinite(m.prMinus))
        m.twoPr = m.prPlus - m.prMinus;

    m.vcPlus = pickNearest(crossValues(p, v, 0.0, rise, +1), vSwitchPlus);
    m.vcMinus = pickNearest(crossValues(p, v, 0.0, fall, -1), vSwitchMinus);
    if (std::isfinite(m.vcPlus) && std::isfinite(m.vcMinus)) {
        m.vcMean = 0.5 * (m.vcPlus - m.vcMinus);
        m.imprint = 0.5 * (m.vcPlus + m.vcMinus);
        if (thicknessNm > 0.0) {
            m.ecPlus = 10.0 * m.vcPlus / thicknessNm;    // V/nm -> MV/cm
            m.ecMinus = 10.0 * m.vcMinus / thicknessNm;
        }
    }

    m.pMax = *std::max_element(p.cbegin(), p.cend());
    double peak = 0.0;
    for (const double value : current)
        peak = std::max(peak, std::abs(value));
    m.peakCurrent = peak;

    // Permittivity from the slope of the loop tips, where switching is done.
    if (thicknessNm > 0.0 && v.size() > 8) {
        double maxAbsV = 0.0;
        for (const double value : v)
            maxAbsV = std::max(maxAbsV, std::abs(value));
        QVector<double> slopes;
        for (const int sign : {1, -1}) {
            QVector<double> tipV, tipP;
            for (int i = 0; i < v.size(); ++i) {
                if (sign * v.at(i) > 0.9 * maxAbsV) {
                    tipV.append(v.at(i));
                    tipP.append(p.at(i));
                }
            }
            if (tipV.size() >= 3)
                slopes.append(slope(tipV, tipP));
        }
        if (!slopes.isEmpty()) {
            double mean = 0.0;
            for (const double s : slopes)
                mean += s;
            mean /= slopes.size();
            // mean is uC/cm^2 per V -> C/m^2 per V is *1e-2
            m.epsR = mean * 1.0e-2 * thicknessNm * 1.0e-9 / Eps0;
        }
    }

    m.valid = std::isfinite(m.twoPr);
    return m;
}

PundMetrics analyzePund(double qP, double qU, double qN, double qD, double areaCm2)
{
    PundMetrics m;
    m.qP = qP;
    m.qU = qU;
    m.qN = qN;
    m.qD = qD;
    if (!(areaCm2 > 0.0))
        return m;
    const double scale = 1.0e6 / areaCm2;  // C -> uC/cm^2
    m.pStar = qP * scale;
    m.pHat = qU * scale;
    m.pSwitchedPlus = (qP - qU) * scale;
    m.pSwitchedMinus = (qN - qD) * scale;
    m.twoPr = 0.5 * (std::abs(m.pSwitchedPlus) + std::abs(m.pSwitchedMinus));
    m.pr = 0.5 * m.twoPr;
    m.valid = true;
    return m;
}

QVector<double> leakageFromTwoFrequencies(const QVector<double> &v1,
                                          const QVector<double> &i1, double f1,
                                          const QVector<double> &v2,
                                          const QVector<double> &i2, double f2)
{
    QVector<double> leak;
    const int n1 = std::min(v1.size(), i1.size());
    const int n2 = std::min(v2.size(), i2.size());
    if (n1 < 2 || n2 < 2 || !(f2 > f1) || !(f1 > 0.0))
        return leak;

    // Sort the second record by voltage so it can be sampled on the first grid.
    QVector<int> order(n2);
    for (int i = 0; i < n2; ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [&v2](int a, int b) { return v2.at(a) < v2.at(b); });
    QVector<double> vs, is;
    vs.reserve(n2);
    is.reserve(n2);
    for (const int index : order) {
        vs.append(v2.at(index));
        is.append(i2.at(index));
    }

    leak.reserve(n1);
    for (int i = 0; i < n1; ++i) {
        const double voltage = v1.at(i);
        double other = is.first();
        if (voltage <= vs.first()) {
            other = is.first();
        } else if (voltage >= vs.last()) {
            other = is.last();
        } else {
            const auto it = std::lower_bound(vs.cbegin(), vs.cend(), voltage);
            const int hi = static_cast<int>(it - vs.cbegin());
            const int lo = std::max(0, hi - 1);
            const double span = vs.at(hi) - vs.at(lo);
            const double f = std::abs(span) < 1.0e-300
                ? 0.0 : (voltage - vs.at(lo)) / span;
            other = is.at(lo) + f * (is.at(hi) - is.at(lo));
        }
        leak.append((f2 * i1.at(i) - f1 * other) / (f2 - f1));
    }
    return leak;
}

double permittivity(double capacitanceF, double areaCm2, double thicknessNm)
{
    if (!(areaCm2 > 0.0) || !(thicknessNm > 0.0) || !(capacitanceF > 0.0))
        return 0.0;
    const double areaM2 = areaCm2 * 1.0e-4;
    const double thicknessM = thicknessNm * 1.0e-9;
    return capacitanceF * thicknessM / (Eps0 * areaM2);
}

}  // namespace ferro
