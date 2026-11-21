#pragma once

#include "../core/measurement.h"
#include "../instruments/ib1500.h"

#include <QThread>
#include <memory>

class RunWorker final : public QThread {
    Q_OBJECT
public:
    RunWorker(std::shared_ptr<IB1500> instrument, MeasurementRequest request,
              QObject *parent = nullptr);
    void cancel();

signals:
    void sampleReady(QString protocolId, Sample sample);
    void progressChanged(int completed, int total);
    void logMessage(QString message);
    /*! Carries the ANALYSED result, not just its metrics.
     *
     *  Analysis fills in per-sample quantities - integrated charge and
     *  polarization - that the streamed samples cannot have yet, so the final
     *  samples must travel with the metrics or the GUI shows a stale copy.
     */
    void runCompleted(QString protocolId, MeasurementResult result);

protected:
    void run() override;

private:
    std::shared_ptr<IB1500> instrument_;
    MeasurementRequest request_;
    std::atomic_bool cancel_{false};
};
