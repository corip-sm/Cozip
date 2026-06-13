#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace cozip::storage
{
class IStorageFactory;
}

namespace cozip::core
{
enum class ProgressPhase : std::uint8_t
{
    Started,
    ScanningInputs,
    ProcessingItems,
    WritingOutput,
    Completed,
};

struct ProgressEvent
{
    ProgressPhase phase = ProgressPhase::Started;
    std::size_t completed_items = 0;
    std::size_t total_items = 0;
    std::uint64_t completed_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::string current_path;
    std::string message;
};

enum class DiagnosticSeverity : std::uint8_t
{
    Info,
    Warning,
    Error,
};

struct DiagnosticEvent
{
    DiagnosticSeverity severity = DiagnosticSeverity::Info;
    std::string path;
    std::string message;
};

class IProgressSink
{
public:
    virtual ~IProgressSink() = default;
    virtual void OnProgress(const ProgressEvent& event) = 0;
};

class IDiagnosticSink
{
public:
    virtual ~IDiagnosticSink() = default;
    virtual void OnDiagnostic(const DiagnosticEvent& event) = 0;
};

class ICancelToken
{
public:
    virtual ~ICancelToken() = default;
    [[nodiscard]] virtual bool IsCancellationRequested() const noexcept = 0;
};

class IThreadPool
{
public:
    virtual ~IThreadPool() = default;
    virtual void Enqueue(std::function<void()> work) = 0;
};

class ILogger
{
public:
    virtual ~ILogger() = default;
    virtual void Log(DiagnosticSeverity severity, std::string_view message) = 0;
};

struct ExecutionEnvironment
{
    storage::IStorageFactory* storage_factory = nullptr;
    IThreadPool* thread_pool = nullptr;
    ILogger* logger = nullptr;
};

struct ExecutionContext
{
    IProgressSink* progress = nullptr;
    IDiagnosticSink* diagnostics = nullptr;
    ICancelToken* cancel = nullptr;
    const ExecutionEnvironment* environment = nullptr;
};

class NeverCancelToken final : public ICancelToken
{
public:
    [[nodiscard]] bool IsCancellationRequested() const noexcept override
    {
        return false;
    }
};
}
