#include "csvwriter.h"

#include <QTextStream>

namespace {
QString csvField(QString value)
{
    value.replace('"', "\"\"");
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QString number(double value)
{
    return QString::number(value, 'g', 12);
}
}

QByteArray CsvWriter::samples(const QVector<Sample> &samples)
{
    QByteArray bytes;
    QTextStream stream(&bytes);
    stream << "time_ns,channel,applied_voltage_V,measured_voltage_V,current_A,conductance_S,"
              "capacitance_F,cmu_conductance_S,polarization_uC_cm2,charge_C,independent_value,"
              "pulse_index,phase,status\n";
    for (const Sample &sample : samples) {
        QString phase = sample.phase;
        phase.replace('"', "\"\"");
        QString status = sample.status;
        status.replace('"', "\"\"");
        stream << sample.timeNs << ',' << sample.channel << ','
               << QString::number(sample.appliedVoltageV, 'g', 12) << ','
               << QString::number(sample.voltageV, 'g', 12) << ','
               << QString::number(sample.currentA, 'g', 12) << ','
               << QString::number(sample.conductanceS, 'g', 12) << ','
               << QString::number(sample.capacitanceF, 'g', 12) << ','
               << QString::number(sample.cmuConductanceS, 'g', 12) << ','
               << QString::number(sample.polarizationUCcm2, 'g', 12) << ','
               << QString::number(sample.chargeC, 'g', 12) << ','
               << QString::number(sample.independentValue, 'g', 12) << ','
               << sample.pulseIndex << ",\"" << phase << "\",\"" << status << "\"\n";
    }
    return bytes;
}

QByteArray CsvWriter::runData(const MeasurementRequest &request, const QString &sampleName,
                              const QString &family,
                              const QString &timestamp, const QString &runStatus,
                              const QVector<Sample> &samples, const QString &backend)
{
    QByteArray bytes;
    QTextStream stream(&bytes);
    stream << "sample_name,protocol_id,family,backend,instrument_channel,run_timestamp,run_status,compliance_current_A,"
              "safety_limit_V,stimulus_voltage_V,depress_voltage_V,read_voltage_V,"
              "pulse_width_ns,delay_ns,pulse_count,cycles";
    for (auto it = request.parameters.cbegin(); it != request.parameters.cend(); ++it)
        stream << ",parameter_" << it.key();
    stream << ",time_ns,channel,applied_voltage_V,measured_voltage_V,current_A,"
              "conductance_S,capacitance_F,cmu_conductance_S,polarization_uC_cm2,charge_C,independent_value,"
              "pulse_index,phase,sample_status\n";

    for (const Sample &sample : samples) {
        stream << csvField(sampleName) << ',' << csvField(request.protocolId) << ',' << csvField(family)
               << ',' << csvField(backend) << ',' << request.instrumentChannel << ','
               << csvField(timestamp) << ','
               << csvField(runStatus) << ','
               << number(request.complianceCurrentA) << ',' << number(request.safetyLimitV) << ','
               << number(request.stimulusVoltageV) << ',' << number(request.depressVoltageV) << ','
               << number(request.readVoltageV) << ',' << request.pulseWidthNs << ','
               << request.delayNs << ',' << request.pulseCount << ',' << request.cycles;
        for (auto it = request.parameters.cbegin(); it != request.parameters.cend(); ++it)
            stream << ',' << number(it.value());
        stream << ',' << sample.timeNs << ',' << sample.channel << ','
               << number(sample.appliedVoltageV) << ',' << number(sample.voltageV) << ','
               << number(sample.currentA) << ',' << number(sample.conductanceS) << ','
               << number(sample.capacitanceF) << ',' << number(sample.cmuConductanceS) << ','
               << number(sample.polarizationUCcm2) << ',' << number(sample.chargeC) << ','
               << number(sample.independentValue) << ','
               << sample.pulseIndex << ',' << csvField(sample.phase) << ','
               << csvField(sample.status) << '\n';
    }
    return bytes;
}

QByteArray CsvWriter::metrics(const QMap<QString, double> &metrics)
{
    QByteArray bytes;
    QTextStream stream(&bytes);
    stream << "metric,value\n";
    for (auto it = metrics.cbegin(); it != metrics.cend(); ++it)
        stream << it.key() << ',' << QString::number(it.value(), 'g', 12) << '\n';
    return bytes;
}
