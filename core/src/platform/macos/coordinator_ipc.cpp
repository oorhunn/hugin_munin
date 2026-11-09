#include "hm/coordinator.hpp"
#include <array>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mach-o/dyld.h>
#include <mutex>
#include <pwd.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;
namespace hm {
namespace {
struct Fd {
  int n;
  ~Fd()
  {
    if (n >= 0)
      close(n);
  }
};
std::string socket_path()
{
  passwd entry{};
  passwd *found = nullptr;
  std::array<char, 16384> strings{};
  if (getpwuid_r(getuid(), &entry, strings.data(), strings.size(), &found) !=
          0 ||
      !found)
    throw std::runtime_error("Cannot locate user home");
  auto dir = std::filesystem::path(found->pw_dir) /
             "Library/Application Support/HuginMunin";
  std::filesystem::create_directories(dir);
  struct stat st{};
  if (lstat(dir.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid())
    throw std::runtime_error("Unsafe service directory");
  chmod(dir.c_str(), 0700);
  return (dir / "service.sock").string();
}
sockaddr_un address(const std::string &path)
{
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  if (path.size() >= sizeof(a.sun_path))
    throw std::runtime_error("Service socket path too long");
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  return a;
}
void configure(int fd)
{
  timeval timeout{1, 0};
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
}
void send_json(int fd, const Json &j)
{
  std::string s = j.dump() + "\n";
  size_t done = 0;
  while (done < s.size()) {
    auto n = write(fd, s.data() + done, s.size() - done);
    if (n <= 0)
      throw std::runtime_error("Service write failed");
    done += n;
  }
}
Json read_json(int fd)
{
  std::string s;
  char buffer[4096];
  while (s.size() < 2UZ * 1024 * 1024) {
    auto n = read(fd, buffer, sizeof(buffer));
    if (n <= 0)
      throw std::runtime_error("Service read failed");
    s.append(buffer, static_cast<size_t>(n));
    auto newline = s.find('\n');
    if (newline != std::string::npos)
      return Json::parse(s.substr(0, newline));
  }
  throw std::runtime_error("Request too large");
}

} // namespace
Json coordinator_request(const Json &request)
{
  Fd fd{socket(AF_UNIX, SOCK_STREAM, 0)};
  if (fd.n < 0)
    throw std::runtime_error("Socket creation failed");
  configure(fd.n);
  auto a = address(socket_path());
  if (connect(fd.n, reinterpret_cast<sockaddr *>(&a), sizeof(a)))
    throw std::runtime_error("Resource service unavailable");
  send_json(fd.n, request);
  auto response = read_json(fd.n);
  if (response.contains("error"))
    throw std::runtime_error(response["error"].get<std::string>());
  return response;
}
void ensure_coordinator()
{
  try {
    coordinator_request(
        {{"tool", "get_capacity"}, {"arguments", Json::object()}});
    return;
  } catch (...) { // NOLINT(bugprone-empty-catch): no service is running, so
                  // start one below
  }
  char executable[4096];
  uint32_t size = sizeof(executable);
  if (_NSGetExecutablePath(executable, &size))
    throw std::runtime_error("Cannot locate service executable");
  auto path = (std::filesystem::weakly_canonical(executable).parent_path() /
               "hm-service")
                  .string();
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  for (int fd : {0, 1, 2})
    posix_spawn_file_actions_addopen(&actions, fd, "/dev/null",
                                     fd == 0 ? O_RDONLY : O_WRONLY, 0600);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
  pid_t pid;
  char *args[] = {path.data(), nullptr};
  int result = posix_spawn(&pid, path.c_str(), &actions, &attr, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  if (result)
    throw std::runtime_error("Cannot start hm-service");
  // Service detaches; reap the launcher to avoid zombie processes.
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  for (int i = 0; i < 20; ++i) {
    try {
      coordinator_request(
          {{"tool", "get_capacity"}, {"arguments", Json::object()}});
      return;
    } catch (...) { // NOLINT(bugprone-empty-catch): the service is still
                    // starting, so retry
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("Resource service did not become ready");
}
int run_coordinator_service()
{
  auto path = socket_path();
  auto a = address(path);
  Fd lock{open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600)};
  if (lock.n < 0 || flock(lock.n, LOCK_EX | LOCK_NB))
    return 0;
  Fd server{socket(AF_UNIX, SOCK_STREAM, 0)};
  if (server.n < 0)
    return 1;
  unlink(path.c_str());
  if (bind(server.n, reinterpret_cast<sockaddr *>(&a), sizeof(a)) ||
      chmod(path.c_str(), 0600) || listen(server.n, 16))
    return 1;
  Coordinator core;
  std::mutex mutex;
  Sampler sampler([&](const Sample &s) {
    const std::scoped_lock guard(mutex);
    core.observe(s, Coordinator::Clock::now());
  });
  while (true) {
    Fd client{accept(server.n, nullptr, nullptr)};
    if (client.n < 0) {
      if (errno == EINTR)
        continue;
      return 1;
    }
    configure(client.n);
    uid_t uid;
    gid_t gid;
    if (getpeereid(client.n, &uid, &gid) || uid != getuid())
      continue;
    try {
      auto req = read_json(client.n);
      Json response;
      {
        const std::scoped_lock guard(mutex);
        response = core.call(req.at("tool").get<std::string>(),
                             req.value("arguments", Json::object()),
                             Coordinator::Clock::now());
      }
      send_json(client.n, response);
    } catch (const std::exception &e) {
      try {
        send_json(client.n, {{"error", e.what()}});
      } catch (...) { // NOLINT(bugprone-empty-catch): the client has gone, so
                      // drop the reply
      }
    }
  }
}
} // namespace hm
