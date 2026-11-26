#include "timestamp_job_manager.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace craybridge::api {
namespace {

constexpr std::size_t kWorkerCount = 2;
constexpr std::size_t kQueueCapacity = 16;
constexpr std::size_t kMaximumRetainedJobs = 128;

enum class InternalJobState {
    Queued,
    Running,
    Succeeded,
    CompletedUnverified,
    Failed,
    Cancelled
};

const char* status_name(InternalJobState state) noexcept {
    switch (state) {
    case InternalJobState::Queued: return "queued";
    case InternalJobState::Running: return "running";
    case InternalJobState::Succeeded: return "succeeded";
    case InternalJobState::CompletedUnverified: return "completed_unverified";
    case InternalJobState::Failed: return "failed";
    case InternalJobState::Cancelled: return "cancelled";
    }
    return "failed";
}

bool is_terminal(InternalJobState state) noexcept {
    return state != InternalJobState::Queued &&
           state != InternalJobState::Running;
}

bool generate_job_id(std::string& output) noexcept {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(
            nullptr,
            bytes.data(),
            static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return false;
    }

    constexpr char digits[] = "0123456789abcdef";
    output.clear();
    output.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        output.push_back(digits[(byte >> 4) & 0x0f]);
        output.push_back(digits[byte & 0x0f]);
    }
    return true;
}

} // namespace

struct TimestampJobManager::Impl {
    struct Job {
        std::string id;
        InternalJobState state = InternalJobState::Queued;
        std::uint64_t sequence = 0;
        std::uint64_t playlist_index = 0;
        std::uint64_t item_index = 0;
        std::wstring physical_path;
        file_time::TimestampUpdate requested;
        file_time::TimestampWriteResult result;
        bool has_result = false;
    };

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs;
    std::deque<std::string> queue;
    std::vector<std::thread> workers;
    bool started = false;
    bool stopping = false;
    std::uint64_t next_sequence = 1;

    void prune_completed_locked() {
        while (jobs.size() >= kMaximumRetainedJobs) {
            auto oldest = jobs.end();
            for (auto it = jobs.begin(); it != jobs.end(); ++it) {
                if (!is_terminal(it->second->state)) continue;
                if (oldest == jobs.end() ||
                    it->second->sequence < oldest->second->sequence) {
                    oldest = it;
                }
            }
            if (oldest == jobs.end()) return;
            jobs.erase(oldest);
        }
    }

    void worker_loop() noexcept {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] {
                    return stopping || !queue.empty();
                });

                if (stopping && queue.empty()) return;
                if (queue.empty()) continue;

                const auto id = std::move(queue.front());
                queue.pop_front();
                const auto found = jobs.find(id);
                if (found == jobs.end()) continue;

                job = found->second;
                if (job->state != InternalJobState::Queued) continue;
                job->state = InternalJobState::Running;
            }

            file_time::TimestampWriteResult result{};
            try {
                result = file_time::write_file_timestamps(
                    job->physical_path, job->requested);
            } catch (...) {
                result.win32_error = ERROR_GEN_FAILURE;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                job->result = result;
                job->has_result = true;

                if (!result.success) {
                    job->state = InternalJobState::Failed;
                } else if (result.readback_success) {
                    job->state = InternalJobState::Succeeded;
                } else {
                    job->state = InternalJobState::CompletedUnverified;
                }
            }
        }
    }
};

TimestampJobManager::TimestampJobManager()
    : impl_(std::make_unique<Impl>()) {}

TimestampJobManager::~TimestampJobManager() {
    stop();
}

bool TimestampJobManager::start() noexcept {
    if (!impl_) return false;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->started) return !impl_->stopping;
        impl_->started = true;
        impl_->stopping = false;
    }

    try {
        impl_->workers.reserve(kWorkerCount);
        for (std::size_t i = 0; i < kWorkerCount; ++i) {
            impl_->workers.emplace_back([state = impl_.get()] {
                state->worker_loop();
            });
        }
        return true;
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->stopping = true;
        }
        impl_->condition.notify_all();
        for (auto& worker : impl_->workers) {
            if (worker.joinable()) worker.join();
        }
        impl_->workers.clear();
        return false;
    }
}

void TimestampJobManager::stop() noexcept {
    if (!impl_) return;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;

        for (const auto& id : impl_->queue) {
            const auto found = impl_->jobs.find(id);
            if (found != impl_->jobs.end() &&
                found->second->state == InternalJobState::Queued) {
                found->second->state = InternalJobState::Cancelled;
            }
        }
        impl_->queue.clear();
    }

    impl_->condition.notify_all();
    for (auto& worker : impl_->workers) {
        if (worker.joinable()) worker.join();
    }
    impl_->workers.clear();
}

TimestampJobSubmitStatus TimestampJobManager::submit(
    std::wstring physical_path,
    std::uint64_t playlist_index,
    std::uint64_t item_index,
    const file_time::TimestampUpdate& update,
    std::string& job_id) noexcept {
    if (!impl_) return TimestampJobSubmitStatus::InternalError;

    std::string identifier;
    if (!generate_job_id(identifier)) {
        return TimestampJobSubmitStatus::IdentifierFailure;
    }

    try {
        auto job = std::make_shared<Impl::Job>();
        job->id = identifier;
        job->playlist_index = playlist_index;
        job->item_index = item_index;
        job->physical_path = std::move(physical_path);
        job->requested = update;

        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->started || impl_->stopping) {
            return TimestampJobSubmitStatus::ServiceStopping;
        }
        if (impl_->queue.size() >= kQueueCapacity) {
            return TimestampJobSubmitStatus::QueueFull;
        }

        impl_->prune_completed_locked();
        if (impl_->jobs.size() >= kMaximumRetainedJobs) {
            return TimestampJobSubmitStatus::InternalError;
        }

        job->sequence = impl_->next_sequence++;
        impl_->jobs.emplace(identifier, job);
        try {
            impl_->queue.push_back(identifier);
        } catch (...) {
            impl_->jobs.erase(identifier);
            throw;
        }

        job_id = identifier;
        impl_->condition.notify_one();
        return TimestampJobSubmitStatus::Accepted;
    } catch (...) {
        return TimestampJobSubmitStatus::InternalError;
    }
}

bool TimestampJobManager::query(
    std::string_view job_id,
    TimestampJobSnapshot& output) const noexcept {
    if (!impl_) return false;

    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->jobs.find(std::string(job_id));
        if (found == impl_->jobs.end()) return false;

        const auto& job = *found->second;
        output.id = job.id;
        output.status = status_name(job.state);
        output.playlist_index = job.playlist_index;
        output.item_index = job.item_index;
        output.requested = job.requested;
        output.has_result = job.has_result;
        output.result = job.result;
        return true;
    } catch (...) {
        return false;
    }
}

TimestampJobCancelStatus TimestampJobManager::cancel(
    std::string_view job_id) noexcept {
    if (!impl_) return TimestampJobCancelStatus::NotFound;

    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->jobs.find(std::string(job_id));
        if (found == impl_->jobs.end()) {
            return TimestampJobCancelStatus::NotFound;
        }

        auto& job = *found->second;
        if (job.state == InternalJobState::Running) {
            return TimestampJobCancelStatus::Running;
        }
        if (job.state != InternalJobState::Queued) {
            return TimestampJobCancelStatus::AlreadyCompleted;
        }

        job.state = InternalJobState::Cancelled;
        const auto queued = std::find(
            impl_->queue.begin(), impl_->queue.end(), job.id);
        if (queued != impl_->queue.end()) impl_->queue.erase(queued);
        return TimestampJobCancelStatus::Cancelled;
    } catch (...) {
        return TimestampJobCancelStatus::NotFound;
    }
}

} // namespace craybridge::api
