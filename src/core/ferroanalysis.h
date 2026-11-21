#pragma once

#include <QString>
#include <QVector>
#include <QMap>

/*! Ferroelectric analysis maths.
 *
 *  Pure numerics - no Qt widgets, no instrument I/O - so every formula here is
 *  unit-testable without hardware. Ported from the validated implementation in
 *  the lab's Python tool (ferro_charge / ferro_metrics / rising_mask /
 *  cross_vals / branch_difference).
 *
 *  Sign and unit conventions used throughout:
 *    - charge Q in coulomb, current in ampere, time in seconds
 *    - electrode area in cm^2
 *    - polarization P in uC/cm^2  (P = Q / A * 1e6)
 *    - field in MV/cm             (E = 10 * V / d_nm)
 */
namespace ferro {

//! Remanent/coercive figures extracted from one closed P-V loop.
struct LoopMetrics {
    double prPlus = 0.0;      //!< P at V=0 on the falling branch   [uC/cm^2]
    double prMinus = 0.0;     //!< P at V=0 on the rising branch    [uC/cm^2]
    double twoPr = 0.0;       //!< prPlus - prMinus                 [uC/cm^2]
    double pMax = 0.0;        //!< largest P reached                [uC/cm^2]
    double vcPlus = 0.0;      //!< positive coercive voltage        [V]
    double vcMinus = 0.0;     //!< negative coercive voltage        [V]
    double vcMean = 0.0;      //!< (vcPlus - vcMinus)/2             [V]
    double imprint = 0.0;     //!< (vcPlus + vcMinus)/2             [V]
    double ecPlus = 0.0;      //!< positive coercive field          [MV/cm]
    double ecMinus = 0.0;     //!< negative coercive field          [MV/cm]
    double peakCurrent = 0.0; //!< max |I| anywhere in the loop     [A]
    double epsR = 0.0;        //!< relative permittivity from the loop tips
    bool valid = false;
};

//! Switched-charge figures from a P-U-N-D sequence.
struct PundMetrics {
    double qP = 0.0, qU = 0.0, qN = 0.0, qD = 0.0;  //!< integrated charge [C]
    double pSwitchedPlus = 0.0;   //!< (qP - qU)/A  = dP+   [uC/cm^2]
    double pSwitchedMinus = 0.0;  //!< (qN - qD)/A  = dP-   [uC/cm^2]
    double pStar = 0.0;           //!< qP/A   switching + non-switching
    double pHat = 0.0;            //!< qU/A   non-switching only
    double twoPr = 0.0;           //!< (|dP+| + |dP-|)/2    [uC/cm^2]
    double pr = 0.0;              //!< twoPr / 2            [uC/cm^2]
    bool valid = false;
};

/*! Cumulative charge by integrating current.
 *
 *  With a uniform sample interval pass \a dt > 0: the exact running sum with a
 *  half-sample back-shift is used, which matches hardware box-averaged samples.
 *  Otherwise the trapezoid rule over \a t is used.
 *
 *  When \a removeDrift is true the net charge accumulated over the record is
 *  subtracted linearly in time. That removes the DC leakage ramp and closes the
 *  loop, which is mandatory before remanence can be read off at V = 0.
 */
QVector<double> cumulativeCharge(const QVector<double> &t, const QVector<double> &current,
                                 bool removeDrift = true, double dt = 0.0);

//! Charge -> polarization in uC/cm^2, vertically centred on the loop midpoint.
QVector<double> polarization(const QVector<double> &charge, double areaCm2,
                             bool centre = true);

//! Per-sample mask: true where the drive voltage is rising.
QVector<bool> risingMask(const QVector<double> &v);

/*! Linear interpolation of \a y wherever \a x crosses \a level.
 *  Only crossings whose direction matches \a direction (+1 rising, -1 falling)
 *  and whose sample pair is inside \a mask are returned.
 */
QVector<double> crossValues(const QVector<double> &x, const QVector<double> &y,
                            double level, const QVector<bool> &mask, int direction);

/*! Branch difference I_rise(V) - I_fall(V).
 *  Ohmic leakage is identical on both branches and cancels, leaving the
 *  switching peaks. Returns the rising-branch voltages (ascending) and the
 *  matching difference current.
 */
void branchDifference(const QVector<double> &v, const QVector<double> &current,
                      const QVector<double> &vref, QVector<double> *voltageOut,
                      QVector<double> *differenceOut);

/*! Full loop extraction.
 *  \a vref is the programmed drive voltage used to decide branch direction;
 *  pass the measured \a v for it when no separate reference exists.
 *  \a thicknessNm <= 0 skips the field and permittivity figures.
 */
LoopMetrics analyzeLoop(const QVector<double> &v, const QVector<double> &p,
                        const QVector<double> &current, const QVector<double> &vref,
                        double thicknessNm);

//! PUND figures from the four integrated pulse charges.
PundMetrics analyzePund(double qP, double qU, double qN, double qD, double areaCm2);

/*! Two-frequency (DLCC) leakage separation.
 *  I_leak(V) = [f2*I1(V) - f1*I2(V)] / (f2 - f1), evaluated on the f1 grid.
 *  Returns an empty vector when the inputs are unusable.
 */
QVector<double> leakageFromTwoFrequencies(const QVector<double> &v1,
                                          const QVector<double> &i1, double f1,
                                          const QVector<double> &v2,
                                          const QVector<double> &i2, double f2);

//! Relative permittivity from a capacitance: eps_r = C*d / (eps0*A).
double permittivity(double capacitanceF, double areaCm2, double thicknessNm);

//! Least-squares slope of y against x; returns 0 when undetermined.
double slope(const QVector<double> &x, const QVector<double> &y);

//! Vacuum permittivity [F/m].
constexpr double Eps0 = 8.8541878128e-12;

}  // namespace ferro
