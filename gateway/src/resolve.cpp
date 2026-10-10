// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Moving a venue to its next address after a failed connect, and re-resolving its
// name off the loop thread.

#include "gateway/gateway.hpp"

#include "core/limits.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/upstream.hpp"

namespace llmbridge
{
    using namespace detail;

    void Gateway::note_connect_failure(int slot, const char* why) noexcept
    {
        if (slot < 0 || static_cast<size_t>(slot) >= _upstreams.size()) return;
        Upstream& up = _upstreams[static_cast<size_t>(slot)];
        const std::string failed = up.ip;
        if (up.ips.size() > 1)
        {
            const auto it = std::find(up.ips.begin(), up.ips.end(), up.ip);
            const size_t i = it == up.ips.end() ? 0 : static_cast<size_t>(it - up.ips.begin());
            up.ip = up.ips[(i + 1) % up.ips.size()];
        }
        if (up.ip != failed)
            LB_WARN("venue ", static_cast<int64_t>(slot), " connect ", why, " at ", failed, ":",
                    static_cast<int64_t>(up.port), "; next address ", up.ip);
        else
            LB_WARN("venue ", static_cast<int64_t>(slot), " connect ", why, " at ", failed, ":",
                    static_cast<int64_t>(up.port));
        if (!up.host.empty()) request_reresolve(slot);
    }

    void Gateway::request_reresolve(int slot) noexcept
    {
        uint8_t& inflight = _rr_inflight[static_cast<size_t>(slot)];
        if (inflight) return;
        try
        {
            std::lock_guard<std::mutex> g(_rr.mutex);
            if (!_rr.thread.joinable()) _rr.thread = std::thread([this] { reresolve_loop(); });
            _rr.pending.push_back(slot);
        }
        catch (...)
        {
            LB_WARN("venue ", static_cast<int64_t>(slot), " re-resolution could not be queued");
            return;
        }
        inflight = 1;
        _rr.cv.notify_one();
    }

    void Gateway::reresolve_loop() noexcept
    {
        for (;;)
        {
            int slot = -1;
            {
                std::unique_lock<std::mutex> lk(_rr.mutex);
                _rr.cv.wait(lk, [this] { return _rr.stop || !_rr.pending.empty(); });
                if (_rr.stop) return;
                slot = _rr.pending.front();
                _rr.pending.erase(_rr.pending.begin());
            }
            // `host` is written in the constructor and never again, so reading it
            // here races with nothing; `ip` and `ips` belong to the loop thread.
            std::vector<std::string> ips;
            try
            {
                std::string err;
                ips = net::resolve_host_ipv4(_upstreams[static_cast<size_t>(slot)].host, &err);
                std::lock_guard<std::mutex> g(_rr.mutex);
                _rr.done.emplace_back(slot, std::move(ips));
                _rr.ready.store(true, std::memory_order_release);
            }
            catch (...)
            {
                // Out of memory on the resolver thread: the venue keeps its list and
                // the next failure asks again.
            }
        }
    }

    void Gateway::apply_reresolved() noexcept
    {
        if (!_rr.ready.load(std::memory_order_acquire)) return;
        std::vector<std::pair<int, std::vector<std::string>>> done;
        {
            std::lock_guard<std::mutex> g(_rr.mutex);
            done.swap(_rr.done);
            _rr.ready.store(false, std::memory_order_relaxed);
        }
        for (auto& [slot, ips] : done)
        {
            _rr_inflight[static_cast<size_t>(slot)] = 0;
            Upstream& up = _upstreams[static_cast<size_t>(slot)];
            if (ips.empty())
            {
                LB_WARN("venue ", static_cast<int64_t>(slot), " re-resolving ", up.host,
                        " failed; keeping ", up.ip);
                continue;
            }
            const bool changed = ips != up.ips;
            up.ips = std::move(ips);
            // The rotation already moved off the failed address; keep that choice when
            // the fresh list still has it, else start the fresh list from the top.
            if (std::find(up.ips.begin(), up.ips.end(), up.ip) == up.ips.end())
                up.ip = up.ips.front();
            ++_stats.upstream_reresolved;
            LB_INFO("venue ", static_cast<int64_t>(slot), " re-resolved ", up.host, ": ",
                    static_cast<int64_t>(up.ips.size()), " addresses",
                    changed ? "" : " (unchanged)", ", using ", up.ip);
        }
    }

} // namespace llmbridge
