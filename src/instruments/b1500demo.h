#pragma once

#include "ib1500.h"

class B1500Demo final : public IB1500 {
public:
    bool connect(const QString &resource, QString *error) override;
    void disconnect() override;
    bool isConnected() const override { return connected_; }
    QString identity() const override;
    QStringList errorQueue() const override;
    MeasurementResult run(const MeasurementRequest &request,
                          const std::atomic_bool &cancel,
                          const SampleCallback &onSample,
                          const ProgressCallback &onProgress) override;

private:
    bool connected_ = false;
    QString resource_;
};
