#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

class MoveOnlyTask
{
public:
    MoveOnlyTask() = default;

    template <typename TCallable>
        requires (!std::is_same_v<std::remove_cvref_t<TCallable>, MoveOnlyTask> &&
                  std::is_invocable_r_v<void, TCallable&>)
    MoveOnlyTask(TCallable&& callable)
        : callable_(std::make_unique<Callable<std::remove_cvref_t<TCallable>>>(
              std::forward<TCallable>(callable)))
    {
    }

    MoveOnlyTask(MoveOnlyTask&&) noexcept = default;
    MoveOnlyTask& operator=(MoveOnlyTask&&) noexcept = default;
    MoveOnlyTask(const MoveOnlyTask&) = delete;
    MoveOnlyTask& operator=(const MoveOnlyTask&) = delete;

    explicit operator bool() const noexcept
    {
        return callable_ != nullptr;
    }

    void operator()()
    {
        if (callable_ != nullptr)
        {
            callable_->Invoke();
        }
    }

private:
    struct ICallable
    {
        virtual ~ICallable() = default;
        virtual void Invoke() = 0;
    };

    template <typename TCallable>
    struct Callable final : ICallable
    {
        explicit Callable(TCallable callable)
            : callable_(std::move(callable))
        {
        }

        void Invoke() override
        {
            callable_();
        }

        TCallable callable_;
    };

    std::unique_ptr<ICallable> callable_;
};

class ITaskExecutor
{
public:
    virtual ~ITaskExecutor() = default;
    [[nodiscard]] virtual std::size_t concurrency() const noexcept = 0;
    virtual bool submit(MoveOnlyTask task) = 0;
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
    ITaskExecutor* task_executor = nullptr;
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
