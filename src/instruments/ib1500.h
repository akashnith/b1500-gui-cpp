#pragma once

#include "../core/measurement.h"
#include "../core/patternbuilder.h"

class IB1500 {
public:
    virtual ~IB1500() = default;
    virtual bool connect(const QString &resource, QString *error) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual QString identity() const = 0;
    virtual QStringList errorQueue() const = 0;
    virtual MeasurementResult run(const MeasurementRequest &request,
                                  const std::atomic_bool &cancel,
                                  const SampleCallback &onSample,
                                  const ProgressCallback &onProgress) = 0;
};

class IWgfmuExecutor {
public:
    virtual ~IWgfmuExecutor() = default;
    virtual void runPlan(const PatternPlan &plan) = 0;
    virtual void abort() = 0;
};
