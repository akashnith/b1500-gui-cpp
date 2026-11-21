#include "runworker.h"

#include <exception>
#include <memory>
#include <utility>

RunWorker::RunWorker(std::shared_ptr<IB1500> instrument, MeasurementRequest request,
                     QObject *parent)
    : QThread(parent), instrument_(std::move(instrument)), request_(std::move(request))
{
}

void RunWorker::cancel()
{
    cancel_.store(true);
}

void RunWorker::run()
{
    try {
        MeasurementResult result = instrument_->run(
            request_, cancel_,
            [this](const Sample &sample) { emit sampleReady(request_.protocolId, sample); },
            [this](int completed, int total) { emit progressChanged(completed, total); });
        for (const QString &warning : result.warnings)
            emit logMessage(QStringLiteral("WARNING: ") + warning);
        emit runCompleted(request_.protocolId, result);
    } catch (const std::exception &error) {
        const QString message = QString::fromUtf8(error.what());
        emit logMessage(QStringLiteral("ERROR: ") + message);
        MeasurementResult failed;
        failed.error = message;
        emit runCompleted(request_.protocolId, failed);
    }
}
