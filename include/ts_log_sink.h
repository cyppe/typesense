#pragma once

#include <cstdio>
#include <mutex>
#include <string>

#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"

// Custom Abseil log sink that writes all severities to a single file.
// Replicates glog's SetLogDestination(INFO, path) + FLAGS_logbuflevel = -1.
class TsFileSink final : public absl::LogSink {
public:
    explicit TsFileSink(const std::string& path) {
        fp_ = std::fopen(path.c_str(), "ae");  // append, close-on-exec
    }

    ~TsFileSink() override {
        if (fp_) std::fclose(fp_);
    }

    TsFileSink(const TsFileSink&) = delete;
    TsFileSink& operator=(const TsFileSink&) = delete;

    bool ok() const { return fp_ != nullptr; }

    void Send(const absl::LogEntry& entry) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!fp_) return;
        auto msg = entry.text_message_with_prefix_and_newline();
        std::fwrite(msg.data(), 1, msg.size(), fp_);
        std::fflush(fp_);
    }

    void Flush() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (fp_) std::fflush(fp_);
    }

private:
    std::mutex mu_;
    std::FILE* fp_ = nullptr;
};
