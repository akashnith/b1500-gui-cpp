#pragma once

#include "measurement.h"

#include <QVector>
#include <QString>

struct VoltageVector {
    qint64 timeNs = 0;
    double voltageV = 0.0;
    int pulseIndex = -1;
    QString phase;
};

struct MeasureWindow {
    qint64 startNs = 0;
    qint64 endNs = 0;
    int pulseIndex = 0;
    QString phase;
    double appliedVoltageV = 0.0;
    double measuredVoltageV = 0.0;
    double independentValue = 0.0;
    QString independentUnit;
};

struct PatternPlan {
    QVector<VoltageVector> channel1;
    QVector<VoltageVector> channel2;
    QVector<MeasureWindow> measureWindows;
    qint64 durationNs = 0;
    QStringList errors;
    bool valid() const { return errors.isEmpty(); }
};

class PatternBuilder {
public:
    static PatternPlan forRequest(const MeasurementRequest &request);
    static PatternPlan pairedWriteRead(const MeasurementRequest &request);
    static QByteArray channelCsv(const QVector<VoltageVector> &vectors);
};
