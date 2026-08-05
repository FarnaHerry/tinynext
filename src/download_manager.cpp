// download_manager.cpp — module implementation unit for tinynext.download_manager,
// backed by mcpplibs::tinyhttps.
//
// This is the only translation unit in the project that imports the tinyhttps
// module. Each download runs on its own std::thread with its own HttpClient,
// because HttpClient owns a connection pool and is documented as not
// thread-safe. Progress and state are written under mutex_ so the UI thread can
// snapshot() them freely.

module tinynext.download_manager;

import std;
import mcpplibs.tinyhttps;

namespace dl {

struct TinyHttpsEngine::Task {
    std::uint64_t id;
    std::string url;
    std::filesystem::path destPath;
    State state = State::Queued;
    std::int64_t totalBytes = -1;
    std::int64_t downloadedBytes = 0;
    std::string error;
    double speedBps = 0.0;
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> paused{false};
    // Pause uses a state-based predicate (`!paused || cancelRequested`), so a
    // missed notify is harmless — the next predicate check sees the current
    // flags. Only the resume()/cancel() sides ever notify.
    std::mutex pauseMutex;
    std::condition_variable pauseCv;
    std::thread worker;
};

TinyHttpsEngine::TinyHttpsEngine() {
    // Winsock must be started before any socket call; tinyhttps exposes
    // platform_init()/platform_cleanup() for exactly this but never invokes
    // them itself. The manager owns the network lifetime for the process.
    mcpplibs::tinyhttps::Socket::platform_init();
}

TinyHttpsEngine::~TinyHttpsEngine() {
    shutdown();
    mcpplibs::tinyhttps::Socket::platform_cleanup();
}

void TinyHttpsEngine::shutdown() {
    // Cancel everything, then join without holding the mutex (a worker's
    // progress callback needs it while we are waiting).
    std::vector<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard lock(mutex_);
        tasks = tasks_;
        for (const auto& task : tasks) {
            task->cancelRequested.store(true);
            task->pauseCv.notify_all();  // wake parked (paused) workers so join returns
        }
    }
    for (const auto& task : tasks) {
        if (task->worker.joinable()) {
            task->worker.join();
        }
    }
}

std::uint64_t TinyHttpsEngine::start(const std::string& url,
                                     const std::filesystem::path& destPath,
                                     const StartOptions&) {
    auto task = std::make_shared<Task>();
    {
        std::lock_guard lock(mutex_);
        task->id = nextId_++;
        task->url = url;
        task->destPath = makeUniqueDest(destPath);
        tasks_.push_back(task);
    }
    task->worker = std::thread(&TinyHttpsEngine::runWorker, this, task);
    return task->id;
}

void TinyHttpsEngine::cancel(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    for (const auto& task : tasks_) {
        if (task->id == id) {
            task->cancelRequested.store(true);
            task->pauseCv.notify_all();  // wake a parked (paused) worker
            if (task->state == State::Queued) {
                // No worker running; settle it here so the UI reflects it
                // immediately.
                task->state = State::Cancelled;
            }
            break;
        }
    }
}

void TinyHttpsEngine::remove(std::uint64_t id) {
    // Cancel first (if running), then drop it from the list so the UI forgets
    // it immediately. Keep the shared_ptr so the worker's late state writes
    // stay safe, then join without holding the mutex.
    std::shared_ptr<Task> task;
    {
        std::lock_guard lock(mutex_);
        for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
            if ((*it)->id == id) {
                task = *it;
                task->cancelRequested.store(true);
                task->pauseCv.notify_all();  // wake a parked (paused) worker
                tasks_.erase(it);
                break;
            }
        }
    }
    if (task && task->worker.joinable()) {
        task->worker.join();
    }
}

void TinyHttpsEngine::pause(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    for (const auto& task : tasks_) {
        if (task->id == id && (task->state == State::Queued ||
                               task->state == State::Downloading)) {
            task->paused.store(true);
            task->state = State::Paused;
            task->speedBps = 0.0;
            break;
        }
    }
}

void TinyHttpsEngine::resume(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    for (const auto& task : tasks_) {
        if (task->id == id && task->state == State::Paused) {
            task->paused.store(false);
            task->state = State::Downloading;
            task->pauseCv.notify_all();
            break;
        }
    }
}

void TinyHttpsEngine::pauseAll() {
    std::lock_guard lock(mutex_);
    for (const auto& task : tasks_) {
        if (task->state == State::Queued || task->state == State::Downloading) {
            task->paused.store(true);
            task->state = State::Paused;
            task->speedBps = 0.0;
        }
    }
}

void TinyHttpsEngine::resumeAll() {
    std::lock_guard lock(mutex_);
    for (const auto& task : tasks_) {
        if (task->state == State::Paused) {
            task->paused.store(false);
            task->state = State::Downloading;
            task->pauseCv.notify_all();
        }
    }
}

std::vector<TaskView> TinyHttpsEngine::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<TaskView> out;
    out.reserve(tasks_.size());
    for (auto it = tasks_.rbegin(); it != tasks_.rend(); ++it) {
        const Task& task = **it;
        out.push_back(TaskView{task.id,
                               task.url,
                               task.destPath,
                               task.state,
                               task.totalBytes,
                               task.downloadedBytes,
                               task.error,
                               task.speedBps,
                               1});
    }
    return out;
}

bool TinyHttpsEngine::busy() const {
    std::lock_guard lock(mutex_);
    for (const auto& task : tasks_) {
        if (task->state == State::Queued || task->state == State::Downloading ||
            task->state == State::Paused) {
            return true;
        }
    }
    return false;
}

void TinyHttpsEngine::runWorker(std::shared_ptr<Task> task) {
    mcpplibs::tinyhttps::HttpClientConfig config;
    config.connectTimeoutMs = 15000;
    config.readTimeoutMs = 60000;
    config.keepAlive = true;
    config.maxRedirects = 10;
    mcpplibs::tinyhttps::HttpClient client(config);

    // Park here if the task was paused while still queued, so a pause that
    // lands between start() and the first block is honoured.
    {
        std::unique_lock lock(task->pauseMutex);
        task->pauseCv.wait(lock, [&] {
            return !task->paused.load() || task->cancelRequested.load();
        });
        if (task->cancelRequested.load()) {
            std::lock_guard managerLock(mutex_);
            task->state = State::Cancelled;
            return;
        }
    }

    {
        std::lock_guard lock(mutex_);
        task->state = State::Downloading;
    }

    // Speed measurement: average over ~0.25s windows so the number is stable.
    auto lastTime = std::chrono::steady_clock::now();
    std::int64_t lastBytes = 0;

    const auto onProgress = [&](std::int64_t total, std::int64_t downloaded) {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - lastTime).count();
        const std::int64_t delta = downloaded - lastBytes;
        std::lock_guard lock(mutex_);
        task->totalBytes = total;
        task->downloadedBytes = downloaded;
        if (dt >= 0.25 && delta >= 0) {
            task->speedBps = static_cast<double>(delta) / dt;
            lastTime = now;
            lastBytes = downloaded;
        }
    };

    // tinyhttps calls this between every body block. Parking here implements
    // pause: the connection stays open and no bytes are read while paused.
    // Returning true (cancelled) — including a cancel issued during a pause —
    // aborts the download.
    const auto isCancelled = [&]() -> bool {
        std::unique_lock lock(task->pauseMutex);
        task->pauseCv.wait(lock, [&] {
            return !task->paused.load() || task->cancelRequested.load();
        });
        return task->cancelRequested.load();
    };

    const auto result =
        client.download_to_file(task->url, task->destPath, onProgress, isCancelled);

    std::lock_guard lock(mutex_);
    task->downloadedBytes = result.bytesWritten;
    if (result.expectedBytes) {
        task->totalBytes = *result.expectedBytes;
    }
    task->speedBps = 0.0;

    if (task->cancelRequested.load() || result.error == "cancelled") {
        task->state = State::Cancelled;
    } else if (result.ok()) {
        task->state = State::Done;
    } else {
        task->state = State::Failed;
        task->error = result.error.empty()
                          ? "HTTP " + std::to_string(result.statusCode)
                          : result.error;
    }
}

std::filesystem::path TinyHttpsEngine::makeUniqueDest(const std::filesystem::path& dest) const {
    // A download is fast enough that its file can land on disk before the next
    // task's dedup runs, so checking the filesystem alone has a race: two
    // queued tasks could pick the same " (1)" name and then truncate each
    // other's output. Reserve names in memory too, from every in-flight task.
    std::vector<std::filesystem::path> reserved;
    reserved.reserve(tasks_.size());
    for (const auto& task : tasks_) {
        reserved.push_back(task->destPath);
    }

    auto taken = [&](const std::filesystem::path& candidate) {
        return std::filesystem::exists(candidate) ||
               std::ranges::count(reserved, candidate) > 0;
    };
    if (!taken(dest)) {
        return dest;
    }

    const std::filesystem::path parent = dest.parent_path();
    const std::string stem = dest.stem().string();
    const std::string ext = dest.extension().string();
    for (int i = 1;; ++i) {
        const auto candidate = parent / (stem + " (" + std::to_string(i) + ")" + ext);
        if (!taken(candidate)) {
            return candidate;
        }
    }
}

} // namespace dl
