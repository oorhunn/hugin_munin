#include "hm/coordinator.hpp"
#include <csignal>
#include <iostream>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int signal)
{
  interrupted = signal;
}
} // namespace
int main(int argc, char **argv)
try {
  hm::Json args = {{"name", "hm-run"}, {"project", "local"}};
  double cpu = 1;
  double memory = 512 * 1024 * 1024.0;
  std::optional<double> estimate;
  std::string title = "Local command";
  int command = 1;
  try {
    while (command < argc && std::string(argv[command]) != "--") {
      std::string option = argv[command++];
      if (command >= argc)
        throw std::runtime_error("Missing option value");
      std::string value = argv[command++];
      if (option == "--cpu")
        cpu = std::stod(value);
      else if (option == "--memory-mb")
        memory = std::stod(value) * 1024 * 1024;
      else if (option == "--estimate-seconds")
        estimate = std::stod(value);
      else if (option == "--title")
        title = value;
      else if (option == "--project")
        args["project"] = value;
      else
        throw std::runtime_error("Unknown option " + option);
    }
    if (++command >= argc)
      throw std::runtime_error(
          "Usage: hm-run [--cpu N] [--memory-mb N] [--estimate-seconds "
          "N] [--title TEXT] [--project TEXT] -- command args...");
    hm::ensure_coordinator();
    auto call = [&](const char *tool, const hm::Json &a) {
      return hm::coordinator_request({{"tool", tool}, {"arguments", a}});
    };
    auto registration = call("register_agent", args);
    hm::Json auth = {{"agent_token", registration.at("agent_token")}};
    args = auth;
    args["request_key"] = "command";
    args["title"] = title;
    args["cpu_cores"] = cpu;
    args["memory_bytes"] = memory;
    if (estimate)
      args["estimated_duration_seconds"] = *estimate;
    hm::Json job;
    try {
      job = call("request_lease", args);
    } catch (...) {
      call("unregister_agent", auth);
      throw;
    }
    hm::Json lease = auth;
    lease["job_id"] = job.at("id");
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    while (job["state"] == "queued" && !interrupted) {
      std::cerr << "Hugin & Munin: waiting for resources\n";
      for (int i = 0; i < 50 && !interrupted; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      job = call("get_request", lease);
    }
    if (interrupted || job["state"] != "running") {
      call("release_lease", lease);
      call("unregister_agent", auth);
      return 1;
    }
    // Build tools inherit a conservative parallelism hint; arbitrary commands
    // remain advisory.
    auto parallelism = std::to_string(std::max(1, static_cast<int>(cpu)));
    // hm-run has one thread, so nothing can read the environment during these
    // calls. NOLINTBEGIN(concurrency-mt-unsafe)
    setenv("CMAKE_BUILD_PARALLEL_LEVEL", parallelism.c_str(), 1);
    setenv("CTEST_PARALLEL_LEVEL", parallelism.c_str(), 1);
    setenv("OMP_NUM_THREADS", parallelism.c_str(), 1);
    // NOLINTEND(concurrency-mt-unsafe)
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t child;
    int error = posix_spawnp(&child, argv[command], nullptr, &attributes,
                             argv + command, environ);
    posix_spawnattr_destroy(&attributes);
    if (error) {
      call("release_lease", lease);
      call("unregister_agent", auth);
      throw std::runtime_error("Command could not start");
    }
    setpriority(PRIO_PROCESS, child, 10);
    int status = 0;
    bool exited = false;
    auto heartbeat = std::chrono::steady_clock::now();
    while (true) {
      if (interrupted) {
        kill(-child, interrupted);
        interrupted = 0;
      }
      if (!exited) {
        auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
          exited = true;
      }
      if (exited && kill(-child, 0) < 0 && errno == ESRCH)
        break;
      if (std::chrono::steady_clock::now() - heartbeat >=
          std::chrono::seconds(20)) {
        try {
          auto response = call("heartbeat", lease);
          if (response.value("action", "") == "yield_when_safe")
            std::cerr << "Hugin & Munin: system pressure; finish or reduce "
                         "work when safe\n";
        } catch (const std::exception &e) {
          // Do not drop the live process or its conservative reservation on
          // transient IPC failure.
          std::cerr << "Hugin & Munin heartbeat: " << e.what() << '\n';
        }
        heartbeat = std::chrono::steady_clock::now();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    call("release_lease", lease);
    call("unregister_agent", auth);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
