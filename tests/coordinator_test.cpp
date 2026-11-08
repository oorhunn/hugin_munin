#include "check.hpp"
#include "hm/coordinator.hpp"
using namespace std::chrono_literals;
int main()
{
  hm::Coordinator c;
  auto t = hm::Coordinator::Clock::now();
  hm::Sample s;
  s.memory_pressure = hm::MemoryPressure::normal;
  s.cpu_percent = 10;
  s.core_count = 10;
  s.memory = hm::Memory{.total = 32ULL << 30U, .used = 8ULL << 30U};
  s.memory_fresh = true;
  s.disk_fresh = true;
  s.disk = hm::Disk{.total = 512ULL << 30U, .used = 100ULL << 30U};
  for (int i = 0; i < 5; ++i)
    c.observe(s, t);
  auto a = c.call("register_agent", {{"name", "Agent"}, {"project", "tests"}},
                  t)["agent_token"];
  auto b = c.call("register_agent", {{"name", "Other"}, {"project", "tests"}},
                  t)["agent_token"];
  hm::Json req = {{"agent_token", a},
                  {"request_key", "build"},
                  {"title", "Build"},
                  {"cpu_cores", 2},
                  {"memory_bytes", 1ULL << 30U},
                  {"estimated_duration_seconds", 120}};
  c.call("set_admissions_paused", {{"paused", true}}, t);
  require(!c.snapshot(t)["capacity"]["admission_open"].get<bool>(),
          "Pause blocks admission");
  c.call("set_admissions_paused", {{"paused", false}}, t);
  auto j = c.call("request_lease", req, t);
  require(j["state"] == "running", "First job admitted");
  require(c.call("request_lease", req, t)["id"] == j["id"], "Retry idempotent");
  req["agent_token"] = b;
  auto queued = c.call("request_lease", req, t);
  require(queued["state"] == "queued", "Second client shares capacity");
  bool rejected = false;
  try {
    c.call("release_lease", {{"agent_token", b}, {"job_id", j["id"]}}, t);
  } catch (...) {
    rejected = true;
  }
  require(rejected, "Ownership enforced");
  c.call("release_lease", {{"agent_token", a}, {"job_id", j["id"]}}, t + 1s);
  require(c.snapshot(t + 1s)["jobs"][0]["state"] == "queued",
          "Status read cannot grant abandoned queue entries");
  auto granted = c.call("get_request",
                        {{"agent_token", b}, {"job_id", queued["id"]}}, t + 1s);
  require(granted["state"] == "running", "Owner poll grants queued work");
  c.call("release_lease", {{"agent_token", b}, {"job_id", queued["id"]}},
         t + 1s);
  req["agent_token"] = a;
  req["request_key"] = "again";
  j = c.call("request_lease", req, t + 1s);
  req["agent_token"] = b;
  req["request_key"] = "again";
  queued = c.call("request_lease", req, t + 1s);
  auto update = c.call(
      "heartbeat",
      {{"agent_token", a}, {"job_id", j["id"]}, {"remaining_seconds", 15}},
      t + 10s);
  require(update["remaining_seconds"] == 15, "App revises remaining estimate");
  c.observe(s, t + 101s);
  auto snap = c.snapshot(t + 101s);
  require(snap["jobs"][0]["state"] == "unresponsive",
          "Missing heartbeat retains reservation");
  require(!snap["capacity"]["admission_open"].get<bool>(),
          "Orphan blocks admissions");
  c.call("release_lease", {{"agent_token", a}, {"job_id", j["id"]}}, t + 101s);
  require(c.snapshot(t + 101s)["jobs"].empty(), "Expired queue cancelled");
  req["request_key"] = "next";
  auto next = c.call("request_lease", req, t + 101s);
  require(next["state"] == "running", "Released resources reused");
  require(c.snapshot(t + 106s)["capacity"]["pressure"] == "unknown",
          "Stale samples explicit");
  s.memory_fresh = false;
  c.observe(s, t + 107s);
  require(c.snapshot(t + 107s)["capacity"]["pressure"] == "unknown",
          "Failed retained memory cannot admit");
  s.memory_fresh = true;
  s.cpu_percent = 99;
  c.observe(s, t + 108s);
  require(c.snapshot(t + 108s)["capacity"]["pressure"] == "critical",
          "Critical pressure detected");
  s.cpu_percent = 10;
  c.observe(s, t + 109s);
  require(c.snapshot(t + 109s)["capacity"]["pressure"] == "critical",
          "Recovery hysteresis");
  return 0;
}
