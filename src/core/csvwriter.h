#pragma once

#include "measurement.h"

#include <QByteArray>

class CsvWriter {
public:
    static QByteArray samples(const QVector<Sample> &samples);
    static QByteArray metrics(const QMap<QString, double> &metrics);
    static QByteArray runData(const MeasurementRequest &request, const QString &sampleName,
                              const QString &family,
                              const QString &timestamp, const QString &runStatus,
                              const QVector<Sample> &samples,
                              const QString &backend = QStringLiteral("demo"));
};
