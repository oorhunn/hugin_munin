#pragma once
#include "hm/sampler.hpp"
#include <chrono>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace hm {
using Json = nlohmann::json;
// Jobs from agents registered with this project are this repository's own
// builds. They skip the RAM-percentage limits that hold other heavy work,
// because the owner's Mac routinely runs above them; critical pressure, macOS
// memory-pressure warnings, and the CPU limits still apply.
inline constexpr std::string_view self_build_project = "hugin_munin";
// Access serialized by the service. Time is injected for deterministic lease
// tests.
class Coordinator {
public:
  using Clock = std::chrono::steady_clock;
  void observe(const Sample &sample, Clock::time_point now);
  Json call(const std::string &tool, const Json &arguments,
            Clock::time_point now);
  Json snapshot(Clock::time_point now);

private:
  struct Agent {
    std::string name, project;
    Clock::time_point seen;
  };
  struct Job {
    std::string id, agent, key, title, state;
    double cpu, memory, disk, gpu;
    std::optional<double> estimate;
    Clock::time_point created, started, heartbeat;
  };
  std::map<std::string, Agent> agents_;
  std::vector<Job> jobs_;
  Sample sample_;
  Clock::time_point observed_;
  bool observed = false;
  bool paused_ = false;
  // Pressure with hysteresis, for all work and for the project's own builds.
  struct Pressure {
    std::string level = "unknown";
    unsigned recovery = 0;
    void step(std::string_view next);
  };
  Pressure pressure_, self_pressure_;
  [[nodiscard]] bool self_build(const Job &job) const;
  Json capacity(Clock::time_point now, bool own_build = false);
  [[nodiscard]] Json job_json(const Job &job, Clock::time_point now) const;
  void admit(Job &target, Clock::time_point now);
  void reconcile(Clock::time_point now);
};
// One newline-delimited JSON call over private local IPC. Throws on unavailable
// service.
Json coordinator_request(const Json &request);
// Starts a sibling hm-service if no service is reachable. No UI dependency.
void ensure_coordinator();
int run_coordinator_service();
Json mcp_tools();
} // namespace hm
