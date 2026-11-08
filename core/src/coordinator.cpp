#include "hm/coordinator.hpp"
#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace hm {
namespace {
double seconds(Coordinator::Clock::time_point a, Coordinator::Clock::time_point b) {
    return std::chrono::duration<double>(a - b).count();
}
std::string token() {
    std::random_device rd;
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; ++i) {
        auto n = rd();
        for (int j = 0; j < 8; ++j) {
            out += hex[n & 15U];
            n >>= 4U;
        }
    }
    return out;
}
std::string text(const Json &a, const char *key) {
    auto s = a.at(key).get<std::string>();
    if (s.empty() || s.size() > 256)
        throw std::runtime_error(std::string(key) + " must contain 1–256 bytes");
    return s;
}
double number(const Json &a, const char *key, double fallback, double maximum) {
    double v = a.value(key, fallback);
    if (!std::isfinite(v) || v < 0 || v > maximum)
        throw std::runtime_error(std::string("Invalid ") + key);
    return v;
}
} // namespace
void Coordinator::observe(const Sample &s, Clock::time_point now) {
    sample_ = s;
    observed_ = now;
    observed = true;
    std::string next = "normal";
    if (!s.cpu_percent || !s.memory || !s.memory_fresh || !s.memory_pressure || !s.memory->total)
        next = "unknown";
    else {
        double ram = static_cast<double>(s.memory->used) / static_cast<double>(s.memory->total);
        if (*s.cpu_percent >= 95 || ram >= 0.92 || s.memory_pressure == MemoryPressure::critical)
            next = "critical";
        else if (*s.cpu_percent >= 80 || ram >= 0.82 ||
                 s.memory_pressure == MemoryPressure::warning)
            next = "constrained";
    }
    if (next == "normal" && pressure_ != "normal") {
        if (++recovery_ >= 5) {
            pressure_ = next;
            recovery_ = 0;
        }
    } else {
        pressure_ = next;
        recovery_ = 0;
    }
}
Json Coordinator::capacity(Clock::time_point now) {
    const bool fresh = observed && seconds(now, observed_) <= 3;
    double cpuReserved = 0;
    double memoryReserved = 0;
    double diskReserved = 0;
    double gpuReserved = 0;
    int active = 0;
    for (auto &j : jobs_)
        if (j.state == "running" || j.state == "unresponsive") {
            cpuReserved += j.cpu;
            memoryReserved += j.memory;
            diskReserved += j.disk;
            gpuReserved += j.gpu;
            ++active;
        }
    const auto total = sample_.memory ? static_cast<double>(sample_.memory->total) : 0;
    const auto used = sample_.memory ? static_cast<double>(sample_.memory->used) : 0;
    // Conservative: unknown attribution means reservations and observed usage both consume budget.
    const auto cores = static_cast<double>(sample_.core_count);
    double cpu = std::min(std::max(0.0, cores * 0.65 - cpuReserved),
                          cores * std::max(0.0, 70 - sample_.cpu_percent.value_or(100)) / 100);
    double memory = std::min(std::max(0.0, total * 0.70 - memoryReserved),
                             std::max(0.0, total * 0.80 - used - memoryReserved));
    double disk =
        sample_.disk && sample_.disk_fresh
            ? std::max(0.0, static_cast<double>(sample_.disk->total - sample_.disk->used) -
                                10.0 * 1024 * 1024 * 1024 - diskReserved)
            : 0;
    bool allow = !paused_ && fresh && pressure_ == "normal" && active == 0;
    return {{"pressure", fresh ? pressure_ : "unknown"},
            {"sample_age_seconds", observed ? Json(seconds(now, observed_)) : Json(nullptr)},
            {"cpu_cores", allow ? cpu : 0},
            {"memory_bytes", allow ? memory : 0},
            {"disk_bytes", allow ? disk : 0},
            {"gpu_slots", allow && sample_.gpu_percent && *sample_.gpu_percent < 50
                              ? std::max(0.0, 1 - gpuReserved)
                              : 0},
            {"paused", paused_},
            {"admission_open", allow},
            {"enforcement", "advisory"},
            {"max_active_jobs", 1},
            {"accounting", "conservative; reservations may overlap measured usage"},
            {"retry_after_seconds", 5}};
}
Json Coordinator::job_json(const Job &j, Clock::time_point now) const {
    bool started = j.state != "queued" && j.state != "cancelled";
    double elapsed = started ? seconds(now, j.started) : 0;
    Json remaining = j.estimate ? Json(std::max(0.0, *j.estimate - elapsed)) : Json(nullptr);
    return {
        {"id", j.id},
        {"agent_name", agents_.contains(j.agent) ? agents_.at(j.agent).name : "Disconnected agent"},
        {"project", agents_.contains(j.agent) ? agents_.at(j.agent).project : ""},
        {"title", j.title},
        {"state", j.state},
        {"cpu_cores", j.cpu},
        {"memory_bytes", j.memory},
        {"disk_bytes", j.disk},
        {"gpu_slots", j.gpu},
        {"estimated_duration_seconds", j.estimate ? Json(*j.estimate) : Json(nullptr)},
        {"remaining_seconds", remaining},
        {"elapsed_seconds", elapsed},
        {"overdue", j.estimate && elapsed > *j.estimate},
        {"lease_seconds_remaining",
         j.state == "running" ? std::max(0.0, 90 - seconds(now, j.heartbeat)) : 0},
        {"estimate_source", "application"},
        {"queue_wait_seconds", nullptr}};
}
void Coordinator::reconcile(Clock::time_point now) {
    for (auto &j : jobs_) {
        if (j.state == "running" && seconds(now, j.heartbeat) > 90)
            j.state = "unresponsive";
        if (j.state == "queued" && seconds(now, j.heartbeat) > 90)
            j.state = "cancelled";
    }
    for (auto it = agents_.begin(); it != agents_.end();) {
        bool hasWork = std::ranges::any_of(jobs_, [&](auto &j) {
            return j.agent == it->first &&
                   (j.state == "running" || j.state == "queued" || j.state == "unresponsive");
        });
        if (!hasWork && seconds(now, it->second.seen) > 300)
            it = agents_.erase(it);
        else
            ++it;
    }
}
void Coordinator::admit(Job &target, Clock::time_point now) {
    // FIFO, whole-vector admission, one active job initially. Never release unresponsive work
    // automatically.
    auto c = capacity(now);
    for (auto &j : jobs_)
        if (j.state == "queued") {
            if (j.id != target.id)
                break;
            if (c["admission_open"].get<bool>() && j.cpu <= c["cpu_cores"].get<double>() &&
                j.memory <= c["memory_bytes"].get<double>() &&
                j.disk <= c["disk_bytes"].get<double>() && j.gpu <= c["gpu_slots"].get<double>()) {
                j.state = "running";
                j.started = now;
                j.heartbeat = now;
            }
            break;
        }
}
Json Coordinator::snapshot(Clock::time_point now) {
    reconcile(now);
    Json jobs = Json::array();
    for (auto &j : jobs_)
        if (j.state != "completed" && j.state != "cancelled")
            jobs.push_back(job_json(j, now));
    Json system = {
        {"cpu_percent", sample_.cpu_percent ? Json(*sample_.cpu_percent) : Json(nullptr)},
        {"gpu_percent", sample_.gpu_percent ? Json(*sample_.gpu_percent) : Json(nullptr)},
        {"core_count", sample_.core_count},
        {"gpu_core_count", sample_.gpu_core_count.value_or(0)}};
    if (sample_.memory) {
        system["memory_used"] = sample_.memory->used;
        system["memory_total"] = sample_.memory->total;
        system["memory_percent"] =
            used_percent(sample_.memory->used, sample_.memory->total).value_or(0);
    }
    if (sample_.disk) {
        system["disk_free"] = sample_.disk->total - sample_.disk->used;
        system["disk_percent"] = used_percent(sample_.disk->used, sample_.disk->total).value_or(0);
    }
    return {{"system", system},
            {"capacity", capacity(now)},
            {"jobs", jobs},
            {"registered_agents", agents_.size()}};
}
Json Coordinator::call(const std::string &tool, const Json &a, Clock::time_point now) {
    if (!a.is_object())
        throw std::runtime_error("Arguments must be an object");
    reconcile(now);
    if (tool == "set_admissions_paused") {
        paused_ = a.at("paused").get<bool>();
        return {{"paused", paused_}};
    }
    if (tool == "get_capacity" || tool == "list_jobs")
        return snapshot(now);
    if (tool == "register_agent") {
        if (agents_.size() >= 128)
            throw std::runtime_error("Agent limit reached");
        auto name = text(a, "name");
        auto project = text(a, "project");
        auto id = token();
        agents_[id] = {name, project, now};
        return {{"agent_token", id},
                {"heartbeat_interval_seconds", 30},
                {"lease_ttl_seconds", 90},
                {"enforcement", "advisory"}};
    }
    auto id = text(a, "agent_token");
    if (!agents_.contains(id))
        throw std::runtime_error("Unknown agent token; register again");
    agents_.at(id).seen = now;
    if (tool == "request_lease") {
        auto key = text(a, "request_key");
        for (auto &j : jobs_)
            if (j.agent == id && j.key == key) {
                j.heartbeat = now;
                admit(j, now);
                return job_json(j, now);
            }
        if (jobs_.size() >= 512) {
            auto finished = std::ranges::find_if(
                jobs_, [](auto &j) { return j.state == "completed" || j.state == "cancelled"; });
            if (finished != jobs_.end())
                jobs_.erase(finished);
        }
        if (jobs_.size() >= 512)
            throw std::runtime_error(
                "Job limit reached; release jobs and reconnect after service maintenance");
        Job j{};
        j.id = token();
        j.agent = id;
        j.key = key;
        j.title = text(a, "title");
        j.state = "queued";
        j.cpu = number(a, "cpu_cores", 1, 4096);
        j.memory = number(a, "memory_bytes", 0, 1e15);
        j.disk = number(a, "disk_bytes", 0, 1e15);
        j.gpu = number(a, "gpu_slots", 0, 1);
        if (j.cpu <= 0 || j.memory <= 0)
            throw std::runtime_error("CPU and memory budgets must be positive");
        if (a.contains("estimated_duration_seconds"))
            j.estimate = number(a, "estimated_duration_seconds", 0, 604800);
        if (observed &&
            (j.cpu > static_cast<double>(sample_.core_count) * 0.65 ||
             (sample_.memory && j.memory > static_cast<double>(sample_.memory->total) * 0.70)))
            throw std::runtime_error("Request exceeds policy maximum; reduce resource demand");
        j.created = now;
        j.heartbeat = now;
        jobs_.push_back(j);
        admit(jobs_.back(), now);
        return job_json(jobs_.back(), now);
    }
    if (tool == "unregister_agent") {
        for (auto &j : jobs_)
            if (j.agent == id) {
                if (j.state == "queued")
                    j.state = "cancelled";
                else if (j.state == "running")
                    j.state = "unresponsive";
            }
        // Keep identity only while outstanding work needs conservative accounting.
        agents_.at(id).seen = now - std::chrono::minutes(6);
        reconcile(now);
        return {{"status", "unregistered"},
                {"note", "Outstanding running jobs retain their reservations until released"}};
    }
    auto job = text(a, "job_id");
    auto it = std::ranges::find_if(jobs_, [&](auto &j) { return j.id == job && j.agent == id; });
    if (it == jobs_.end())
        throw std::runtime_error("Job not owned by this agent");
    if (tool == "heartbeat" || tool == "get_request") {
        it->heartbeat = now;
        admit(*it, now);
        if (tool == "heartbeat" && it->state == "unresponsive")
            it->state = "running";
        if (a.contains("remaining_seconds")) {
            double remaining = number(a, "remaining_seconds", 0, 604800);
            it->estimate = remaining + (it->state == "queued" ? 0 : seconds(now, it->started));
        }
        auto out = job_json(*it, now);
        out["action"] = capacity(now)["pressure"] == "normal" ? "continue" : "yield_when_safe";
        out["retry_after_seconds"] = 5;
        return out;
    }
    if (tool == "release_lease") {
        if (it->state == "queued")
            it->state = "cancelled";
        else if (it->state == "running" || it->state == "unresponsive")
            it->state = "completed";
        reconcile(now);
        return {{"status", it->state}};
    }
    throw std::runtime_error("Unknown tool");
}
Json mcp_tools() {
    Json tools = Json::array();
    auto add = [&](std::string name, std::string description, Json properties, Json required) {
        tools.push_back({{"name", name},
                         {"description", description},
                         {"inputSchema",
                          {{"type", "object"},
                           {"properties", properties},
                           {"required", required},
                           {"additionalProperties", false}}}});
    };
    Json str = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}};
    Json num = {{"type", "number"}, {"minimum", 0}};
    add("register_agent",
        "Register before requesting local resources. Keep the returned token private. Heartbeat "
        "every 30 seconds during work.",
        {{"name", str}, {"project", str}}, {"name", "project"});
    add("get_capacity",
        "Read available budgets, pressure, and jobs. A snapshot grants no permission to execute.",
        Json::object(), Json::array());
    add("list_jobs", "List registered jobs and application-supplied duration estimates.",
        Json::object(), Json::array());
    add("request_lease",
        "Reserve CPU core equivalents, RAM bytes, optional scratch disk bytes and GPU slot "
        "atomically. Wait until state=running before heavy work. Set build/test parallelism to the "
        "grant. One active job initially. Do not reuse a parent's budget for child jobs. Estimates "
        "are supplied by your application.",
        {{"agent_token", str},
         {"request_key", str},
         {"title", str},
         {"cpu_cores", num},
         {"memory_bytes", num},
         {"disk_bytes", num},
         {"gpu_slots", num},
         {"estimated_duration_seconds", num}},
        {"agent_token", "request_key", "title", "cpu_cores", "memory_bytes"});
    for (const auto *name : {"get_request", "heartbeat", "release_lease"}) {
        Json props = {{"agent_token", str}, {"job_id", str}};
        if (std::string(name) == "heartbeat")
            props["remaining_seconds"] = num;
        add(name,
            std::string(name) == "release_lease"
                ? "Release only after work and descendants have stopped. Idempotent. Cancels "
                  "queued work."
                : "Check/renew a job. Heartbeat during long commands; optionally revise estimated "
                  "remaining seconds. Obey yield_when_safe. Missing heartbeat retains running "
                  "reservations conservatively.",
            props, {"agent_token", "job_id"});
    }
    add("unregister_agent",
        "Cancel queued work and unregister. Release running jobs first after stopping them; "
        "otherwise reservations remain held.",
        {{"agent_token", str}}, {"agent_token"});
    return tools;
}
} // namespace hm
