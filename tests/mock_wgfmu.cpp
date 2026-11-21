#include <algorithm>
#include <cstring>

#ifdef _WIN32
#define MOCK_EXPORT __declspec(dllexport)
#define MOCK_CALL __stdcall
#define MOCK_LINKAGE extern "C"
#else
#define MOCK_EXPORT __attribute__((visibility("default")))
#define MOCK_CALL
#define MOCK_LINKAGE extern "C"
#endif

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_initialize();
MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_closeSession();
MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_abort();

namespace {
enum FunctionId {
    OpenSession = 1,
    CloseSession = 2,
    Initialize = 3,
    SetTimeout = 4,
    GetChannelIdSize = 5,
    GetChannelIds = 6,
    Abort = 7,
    GetErrorSize = 8,
    GetError = 9,
    ExportAscii = 10,
    Clear = 11,
    CreatePattern = 12,
    AddVector = 13,
    SetMeasureEvent = 14,
    AddSequence = 15,
    SetOperationMode = 16,
    Connect = 17,
    Execute = 18,
    WaitUntilCompleted = 19,
    GetMeasureValueSize = 20,
    GetMeasureValues = 21,
    GetMeasureValue = 22,
    // Added so the mock covers the same surface as the real B1530A library.
    Disconnect = 23,
    SetForceVoltageRange = 24,
    SetMeasureMode = 25,
    SetMeasureVoltageRange = 26,
    SetMeasureCurrentRange = 27
};

// A real pattern plan issues one addVector per waveform breakpoint, which runs
// to several hundred calls. The buffer must outlast that or later calls in the
// sequence (setMeasureEvent, addSequence, execute) are silently dropped.
char calls[4096][64]{};
int callCount = 0;
int failingFunction = 0;
int failureCode = 0;
bool sessionOpen = false;

/*  The Programming Guide example checks exact arguments. Those checks only
 *  apply while that example is being replayed, so the same mock can also be
 *  driven by WgfmuApi::runPlan() with arbitrary patterns. */
bool strictGuide = false;

constexpr int kMaxSequences = 8;
constexpr int kMaxPatterns = 8;

struct PatternEvents {
    char name[32]{};
    int events = 0;
};

PatternEvents patterns[kMaxPatterns]{};
int patternCount = 0;

struct Sequence {
    int channel = 0;
    char pattern[32]{};
};

Sequence sequences[kMaxSequences]{};
int sequenceCount = 0;

void record(const char *name)
{
    if (callCount >= static_cast<int>(sizeof(calls) / sizeof(calls[0])))
        return;
    std::strncpy(calls[callCount], name, sizeof(calls[callCount]) - 1);
    ++callCount;
}

int resultFor(int functionId)
{
    return failingFunction == functionId ? failureCode : 0;
}

PatternEvents *patternFor(const char *name, bool create)
{
    for (int i = 0; i < patternCount; ++i)
        if (std::strcmp(patterns[i].name, name) == 0)
            return &patterns[i];
    if (!create || patternCount >= kMaxPatterns)
        return nullptr;
    PatternEvents &entry = patterns[patternCount++];
    std::strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.events = 0;
    return &entry;
}

const char *patternForChannel(int channel)
{
    for (int i = 0; i < sequenceCount; ++i)
        if (sequences[i].channel == channel)
            return sequences[i].pattern;
    return nullptr;
}

int finishGuideSequence(bool failed, bool executionAttempted)
{
    int result = failed ? failureCode : 0;
    if (failed && executionAttempted)
        WGFMU_abort();
    if (sessionOpen) {
        WGFMU_initialize();
        const int closeResult = WGFMU_closeSession();
        if (result == 0)
            result = closeResult;
    }
    return result;
}
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_openSession(const char *)
{
    record("WGFMU_openSession");
    const int result = resultFor(OpenSession);
    if (result == 0)
        sessionOpen = true;
    return result;
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_closeSession()
{
    record("WGFMU_closeSession");
    const int result = resultFor(CloseSession);
    if (result == 0)
        sessionOpen = false;
    return result;
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_initialize()
{
    record("WGFMU_initialize");
    return resultFor(Initialize);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_setTimeout(double)
{
    record("WGFMU_setTimeout");
    return resultFor(SetTimeout);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_getChannelIdSize(int *size)
{
    record("WGFMU_getChannelIdSize");
    if (resultFor(GetChannelIdSize) != 0)
        return resultFor(GetChannelIdSize);
    *size = 2;
    return 0;
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_getChannelIds(int *channelIds, int *size)
{
    record("WGFMU_getChannelIds");
    if (resultFor(GetChannelIds) != 0)
        return resultFor(GetChannelIds);
    if (*size < 2)
        return -1;
    channelIds[0] = 201;
    channelIds[1] = 202;
    *size = 2;
    return 0;
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_getErrorSize(int *size)
{
    record("WGFMU_getErrorSize");
    *size = static_cast<int>(sizeof("mock injected WGFMU error"));
    return 0;
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_getError(char *error, int *size)
{
    record("WGFMU_getError");
    constexpr char message[] = "mock injected WGFMU error";
    if (*size < static_cast<int>(sizeof(message)))
        return -1;
    std::memcpy(error, message, sizeof(message));
    *size = static_cast<int>(sizeof(message));
    return 0;
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_abort()
{
    record("WGFMU_abort");
    return resultFor(Abort);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_exportAscii(const char *)
{
    record("WGFMU_exportAscii");
    return resultFor(ExportAscii);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_clear()
{
    record("WGFMU_clear");
    patternCount = 0;
    sequenceCount = 0;
    return resultFor(Clear);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_createPattern(const char *name, double)
{
    record("WGFMU_createPattern");
    patternFor(name, true);
    return resultFor(CreatePattern);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_addVector(const char *, double, double)
{
    record("WGFMU_addVector");
    return resultFor(AddVector);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_setMeasureEvent(
    const char *patternName, const char *eventName, double time, int points,
    double interval, double average, int dataMode)
{
    record("WGFMU_setMeasureEvent");
    if (strictGuide) {
        if (std::strcmp(patternName, "pulse") != 0 ||
            std::strcmp(eventName, "evt") != 0 || time != 0.0 || points != 100 ||
            interval != 0.00001 || average != 0.0 || dataMode != 12000)
            return -1;
    } else if (dataMode != 12000 || points < 1 || interval <= 0.0 || average <= 0.0 ||
               time < 0.0) {
        return -1;
    }
    if (PatternEvents *entry = patternFor(patternName, true))
        entry->events += points;
    return resultFor(SetMeasureEvent);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_addSequence(
    int channelId, const char *patternName, double loopCount)
{
    record("WGFMU_addSequence");
    if (strictGuide) {
        if (channelId != 101 || std::strcmp(patternName, "pulse") != 0 ||
            loopCount != 10.0)
            return -1;
    } else if (loopCount <= 0.0) {
        return -1;
    }
    if (sequenceCount < kMaxSequences) {
        sequences[sequenceCount].channel = channelId;
        std::strncpy(sequences[sequenceCount].pattern, patternName,
                     sizeof(sequences[sequenceCount].pattern) - 1);
        ++sequenceCount;
    }
    return resultFor(AddSequence);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_setOperationMode(
    int channelId, int operationMode)
{
    record("WGFMU_setOperationMode");
    if (strictGuide && (channelId != 101 || operationMode != 2001))
        return -1;
    if (operationMode != 2001 && operationMode != 2002 && operationMode != 2003)
        return -1;
    return resultFor(SetOperationMode);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_setForceVoltageRange(int, int range)
{
    record("WGFMU_setForceVoltageRange");
    if (range < 3000 || range > 3003)
        return -1;
    return resultFor(SetForceVoltageRange);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_setMeasureMode(int, int mode)
{
    record("WGFMU_setMeasureMode");
    if (mode != 4000 && mode != 4001)
        return -1;
    return resultFor(SetMeasureMode);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_setMeasureVoltageRange(int, int range)
{
    record("WGFMU_setMeasureVoltageRange");
    if (range < 5000 || range > 5002)
        return -1;
    return resultFor(SetMeasureVoltageRange);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_setMeasureCurrentRange(int, int range)
{
    record("WGFMU_setMeasureCurrentRange");
    if (range < 6001 || range > 6005)
        return -1;
    return resultFor(SetMeasureCurrentRange);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_connect(int)
{
    record("WGFMU_connect");
    return resultFor(Connect);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_disconnect(int)
{
    record("WGFMU_disconnect");
    return resultFor(Disconnect);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_execute()
{
    record("WGFMU_execute");
    return resultFor(Execute);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_waitUntilCompleted()
{
    record("WGFMU_waitUntilCompleted");
    return resultFor(WaitUntilCompleted);
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_getMeasureValueSize(
    int channelId, int *measuredSize, int *totalSize)
{
    record("WGFMU_getMeasureValueSize");
    if (resultFor(GetMeasureValueSize) != 0)
        return resultFor(GetMeasureValueSize);
    int points = 1;
    if (!strictGuide) {
        const char *pattern = patternForChannel(channelId);
        const PatternEvents *entry = pattern ? patternFor(pattern, false) : nullptr;
        points = entry ? entry->events : 0;
    }
    *measuredSize = points;
    *totalSize = points;
    return 0;
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_getMeasureValues(
    int, int, int *size, double *measureTimes, double *measureValues)
{
    record("WGFMU_getMeasureValues");
    if (resultFor(GetMeasureValues) != 0)
        return resultFor(GetMeasureValues);
    if (*size < 1)
        return -1;
    *size = 1;
    measureTimes[0] = 0.0;
    measureValues[0] = 0.0;
    return 0;
}

MOCK_LINKAGE MOCK_EXPORT int MOCK_CALL WGFMU_getMeasureValue(
    int channelId, int index, double *measureTime, double *measureValue)
{
    record("WGFMU_getMeasureValue");
    if (strictGuide) {
        if (channelId != 101 || index != 0)
            return -1;
        if (resultFor(GetMeasureValue) != 0)
            return resultFor(GetMeasureValue);
        *measureTime = 0.0;
        *measureValue = 0.0;
        return 0;
    }
    if (resultFor(GetMeasureValue) != 0)
        return resultFor(GetMeasureValue);
    const char *pattern = patternForChannel(channelId);
    if (!pattern || index < 0)
        return -1;
    *measureTime = static_cast<double>(index) * 1.0e-6;
    /* "force" measures voltage, "sink" measures current, as runPlan expects.
     *
     * The sink value is NEGATIVE on purpose: a real WGFMU channel reports the
     * current it sources, positive out of its own terminal, and current driven
     * through the device flows INTO the sink. Modelling a passive 50 kOhm load
     * at +0.5 V therefore means the sink reads -10 uA, and the conversion to
     * the device convention must turn that back into +10 uA.
     */
    *measureValue = std::strcmp(pattern, "sink") == 0 ? -1.0e-5 : 0.5;
    return 0;
}

extern "C" MOCK_EXPORT int MockWGFMU_runGuideFastIvExample(int abortAfterExecute)
{
    strictGuide = true;
    int result = WGFMU_clear();
    if (result != 0) {
        strictGuide = false;
        return result;
    }
    result = WGFMU_createPattern("pulse", 0.0);
    if (result == 0)
        result = WGFMU_addVector("pulse", 0.0001, 1.0);
    if (result == 0)
        result = WGFMU_addVector("pulse", 0.0004, 1.0);
    if (result == 0)
        result = WGFMU_addVector("pulse", 0.0001, 0.0);
    if (result == 0)
        result = WGFMU_addVector("pulse", 0.0004, 0.0);
    if (result == 0)
        result = WGFMU_setMeasureEvent("pulse", "evt", 0.0, 100, 0.00001, 0.0, 12000);
    if (result == 0)
        result = WGFMU_addSequence(101, "pulse", 10.0);
    if (result != 0) {
        strictGuide = false;
        return result;
    }

    result = WGFMU_openSession("MOCK::WGFMU");
    if (result != 0) {
        strictGuide = false;
        return result;
    }
    if ((result = WGFMU_initialize()) != 0) {
        const int outcome = finishGuideSequence(true, false);
        strictGuide = false;
        return outcome;
    }
    if ((result = WGFMU_setOperationMode(101, 2001)) != 0 ||
        (result = WGFMU_connect(101)) != 0) {
        const int outcome = finishGuideSequence(true, false);
        strictGuide = false;
        return outcome;
    }
    if ((result = WGFMU_execute()) != 0) {
        const int outcome = finishGuideSequence(true, true);
        strictGuide = false;
        return outcome;
    }
    if (abortAfterExecute) {
        const int outcome = finishGuideSequence(WGFMU_abort() != 0, false);
        strictGuide = false;
        return outcome;
    }

    if ((result = WGFMU_waitUntilCompleted()) != 0) {
        const int outcome = finishGuideSequence(true, true);
        strictGuide = false;
        return outcome;
    }
    int measuredSize = 0;
    int totalSize = 0;
    if ((result = WGFMU_getMeasureValueSize(101, &measuredSize, &totalSize)) != 0) {
        const int outcome = finishGuideSequence(true, true);
        strictGuide = false;
        return outcome;
    }
    for (int index = 0; index < measuredSize; ++index) {
        double time = 0.0;
        double value = 0.0;
        if ((result = WGFMU_getMeasureValue(101, index, &time, &value)) != 0) {
            const int outcome = finishGuideSequence(true, true);
            strictGuide = false;
            return outcome;
        }
    }
    const int outcome = finishGuideSequence(false, false);
    strictGuide = false;
    return outcome;
}

extern "C" MOCK_EXPORT void MockWGFMU_reset()
{
    callCount = 0;
    failingFunction = 0;
    failureCode = 0;
    sessionOpen = false;
    strictGuide = false;
    patternCount = 0;
    sequenceCount = 0;
}

extern "C" MOCK_EXPORT void MockWGFMU_failFunction(int functionId, int code)
{
    failingFunction = functionId;
    failureCode = code;
}

extern "C" MOCK_EXPORT int MockWGFMU_callCount()
{
    return callCount;
}

extern "C" MOCK_EXPORT int MockWGFMU_callName(int index, char *buffer, int capacity)
{
    if (index < 0 || index >= callCount || capacity <= 0)
        return -1;
    const int length = static_cast<int>(std::strlen(calls[index]));
    const int copied = std::min(length, capacity - 1);
    std::memcpy(buffer, calls[index], static_cast<size_t>(copied));
    buffer[copied] = '\0';
    return copied;
}
