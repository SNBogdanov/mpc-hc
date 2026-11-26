#pragma once

#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>

// Offline-first "viewed" reporter for Whisparr.
//
// A natural end-of-file queues a small JSON file in
// %APPDATA%\MPC-HC\whisparr-outbox\<xx>\ (xx = first two hex digits of the
// file identity hash, so the directory does not grow without bound). A worker
// claims events atomically (rename to .sending), sends them with
// PUT /api/v3/episode/viewed/file and removes them on success. Events survive
// restarts and are retried whenever the Whisparr server becomes reachable.
//
// State is also written to %APPDATA%\MPC-HC\whisparr-status.log:
//   QUEUED / SENT / FAILED / RETRY
class CWhisparrWatched
{
public:
    CWhisparrWatched() = default;
    ~CWhisparrWatched();

    void Start();
    void Stop();

    // Queue a "viewed" event for a file that reached its natural end.
    void Enqueue(const CString& filePath);

    // Queue a "file was deleted" notification (POST /episodefile/deleted).
    void NotifyDeleted(const CString& filePath);

private:
    struct Config
    {
        bool enabled = false;
        CString url;
        CString apiKey;
        bool verifyTls = false;
    };

    Config LoadConfig() const;
    CString GetConfigPath() const;
    CString GetOutboxDir() const;
    CString GetStatusLogPath() const;
    void AppendStatus(const CString& line);
    void EnsureConfigTemplate() const;
    void WorkerLoop();
    void ProcessOutbox();
    void ReclaimStale(const CString& dir);
    void ProcessEvent(const Config& cfg, const CString& jsonPath);
    bool SendEvent(const Config& cfg, LPCWSTR method, const CString& relPath, const std::string& body, DWORD& status, std::string& response, DWORD& lastError, CString& requestInfo);

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    bool m_wake = false;
};
