#pragma once

#include <QMap>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <functional>

enum class MeasurementFamily {
    Smu,
    Synapse,
    Neuron,
    Fe
};

struct ProtocolSpec {
    QString id;
    QString name;
    MeasurementFamily family;
    QString description;
};

struct ProtocolParameter {
    QString key;
    QString label;
    QString unit;
    double defaultValue = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double step = 1.0;
    int decimals = 0;
    bool voltage = false;
    QStringList choices;
};

struct MeasurementRequest {
    QString protocolId;
    int instrumentChannel = 0;
    /*! Optional second SMU, held at 0 V as the return path. 0 means none.
     *
     *  A two-terminal device can be measured either force-to-GNDU or
     *  force-to-SMU. The second arrangement needs the sink channel to be
     *  explicitly enabled and driven to zero, because CNX on the force
     *  channel OPENS every other channel - so without this the return
     *  electrode is left floating and the sweep reads an open circuit.
     */
    int sinkChannel = 0;
    double stimulusVoltageV = 0.8;
    double readVoltageV = 0.1;
    qint64 pulseWidthNs = 1000;
    qint64 delayNs = 1000;
    double depressVoltageV = -0.8;
    int pulseCount = 20;
    int cycles = 1;
    double safetyLimitV = 1.0;
    double complianceCurrentA = 1.0e-3;
    QMap<QString, double> parameters;
};

struct Sample {
    qint64 timeNs = 0;
    int channel = 1;
    double appliedVoltageV = 0.0;
    double voltageV = 0.0;
    double currentA = 0.0;
    double conductanceS = 0.0;
    int pulseIndex = 0;
    QString phase;
    QString status = QStringLiteral("OK");

    // --- extended quantities -------------------------------------------
    // Zero when the protocol did not measure them, which keeps every
    // existing protocol and CSV column unchanged.
    double capacitanceF = 0.0;      //!< MFCMU capacitance (F)
    double cmuConductanceS = 0.0;   //!< MFCMU parallel conductance (S)
    double polarizationUCcm2 = 0.0; //!< integrated polarization (uC/cm^2)
    double chargeC = 0.0;           //!< integrated switched charge (C)
    /*! The protocol's swept variable when it differs from the applied voltage.
     *  For the FTJ this is the poling amplitude while appliedVoltageV is the
     *  low read bias, which is what makes R vs V_pole plottable. */
    double independentValue = 0.0;
    /*! Unit of independentValue ("ns", "V", "Hz", "s", "cycle", "pulse").
     *
     *  Empty when the protocol sweeps nothing, which is what tells the GUI
     *  whether the swept column and the swept x axis mean anything for a run.
     */
    QString independentUnit;

    //! True when this sample carries a capacitance reading.
    bool hasCapacitance() const { return capacitanceF != 0.0 || cmuConductanceS != 0.0; }
    //! True when this sample carries a polarization reading.
    bool hasPolarization() const { return polarizationUCcm2 != 0.0 || chargeC != 0.0; }
};

struct MeasurementResult {
    QVector<Sample> samples;
    QMap<QString, double> metrics;
    QStringList warnings;
    bool canceled = false;
    QString error;
};

struct ValidationResult {
    QStringList errors;
    bool ok() const { return errors.isEmpty(); }
};

Q_DECLARE_METATYPE(Sample)
Q_DECLARE_METATYPE(MeasurementResult)

const QVector<ProtocolSpec> &protocolCatalog();
QVector<ProtocolParameter> protocolParameters(const QString &protocolId);
double parameterValue(const MeasurementRequest &request, const QString &key);

/*! A time parameter's value, in NANOSECONDS.
 *
 *  Every time parameter is stored and shown in seconds, so that one unit
 *  covers a 100 ns pulse edge and a 3600 s retention hold without the
 *  operator converting anything. The pattern builder and both backends work
 *  in integer nanoseconds, because the WGFMU places edges on a 10 ns grid and
 *  accumulating a schedule in floating-point seconds loses that grid. This is
 *  the one conversion between the two, so it happens in one place.
 */
double parameterNs(const MeasurementRequest &request, const QString &key);
QString familyName(MeasurementFamily family);
ValidationResult validateRequest(const MeasurementRequest &request);
qint64 expectedSampleCount(const MeasurementRequest &request);

/*! Choices offered for the staircase sweep's current measurement range.
 *
 *  "Auto, min X" is the instrument's *limited* auto ranging: it still ranges
 *  automatically, but never below X. That matters on a device that switches
 *  by decades - unrestricted auto ranging spends a range change on every
 *  point through the transition, and each change costs settling time that
 *  lands in the middle of the event being measured.
 */
QStringList smuCurrentRangeLabels();

/*! FLEX `TIV` irange code for an index into smuCurrentRangeLabels().
 *
 *  Codes are from the B1500 Programming Guide, Table 4-3: 0 is auto, 11..19
 *  are limited auto ranging with that minimum, and the negatives are the
 *  corresponding fixed ranges. Note that `RI` is not usable here - it applies
 *  only to XE-triggered measurements, not to the high-speed spot measurement
 *  this backend uses - so the range travels in the TIV command itself.
 */
int smuCurrentRangeCode(int choiceIndex);

/*! Number of steps of size \a step needed to reach \a magnitude from zero.
 *
 *  Rounded up, so the requested peak is always reached rather than stopped
 *  short of: a sweep that stops at 1.9 V because 2.0 is not a multiple of the
 *  step has not done what was asked. At least one step is always returned.
 */
qint64 smuSweepStepCount(double magnitude, double step);
//! Number of PUND read checkpoints in an endurance run, the pristine one included.
qint64 endurancePoints(const MeasurementRequest &request);
MeasurementResult analyzeSamples(const MeasurementRequest &request,
                                 const QVector<Sample> &samples);

using SampleCallback = std::function<void(const Sample &)>;
using ProgressCallback = std::function<void(int completed, int total)>;
