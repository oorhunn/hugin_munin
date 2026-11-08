#include "hm/coordinator.hpp"
#include <iostream>
int main() try {
    bool initialized = false;
    std::string line;
    // stdio only: no banners or logs on stdout. Limit frame size before allocation grows.
    while (true) {
        line.clear();
        char c;
        while (std::cin.get(c) && c != '\n') {
            if (line.size() >= 262144)
                return 1;
            line += c;
        }
        if (line.empty() && !std::cin)
            break;
        hm::Json request;
        hm::Json id = nullptr;
        hm::Json result;
        try {
            request = hm::Json::parse(line);
            if (!request.is_object() || request.value("jsonrpc", "") != "2.0" ||
                !request.contains("method") || !request["method"].is_string())
                throw std::runtime_error("Invalid JSON-RPC request");
            id = request.value("id", hm::Json(nullptr));
            const auto method = request["method"].get<std::string>();
            if (!request.contains("id"))
                continue;
            auto p = request.value("params", hm::Json::object());
            if (method == "initialize") {
                initialized = true;
                result = {
                    {"protocolVersion", "2025-11-25"},
                    {"serverInfo", {{"name", "hugin-munin"}, {"version", "0.1.0"}}},
                    {"capabilities",
                     {{"tools", hm::Json::object()}, {"resources", hm::Json::object()}}},
                    {"instructions",
                     "Register and reserve before expensive local work. Wait for running state. "
                     "Heartbeat every 30 seconds, update application estimates, obey granted "
                     "concurrency and yield instructions, release only when processes stop. "
                     "Reservations are advisory, not OS quotas. Restart invalidates tokens; "
                     "re-register and reconcile running work before requesting new capacity."}};
            } else if (method == "ping")
                result = hm::Json::object();
            else if (!initialized)
                throw std::runtime_error("Initialize first");
            else if (method == "tools/list")
                result = {{"tools", hm::mcp_tools()}};
            else if (method == "resources/list")
                result = {{"resources",
                           hm::Json::array({{{"uri", "hugin-munin://status"},
                                             {"name", "Resource capacity and registered jobs"},
                                             {"mimeType", "application/json"}}})}};
            else if (method == "resources/templates/list")
                result = {{"resourceTemplates", hm::Json::array()}};
            else if (method == "tools/call" || method == "resources/read") {
                if (method == "resources/read" && p.at("uri") != "hugin-munin://status")
                    throw std::runtime_error("Unknown resource");
                try {
                    hm::ensure_coordinator();
                    auto value = hm::coordinator_request(
                        {{"tool", method == "tools/call" ? p.at("name") : hm::Json("get_capacity")},
                         {"arguments", p.value("arguments", hm::Json::object())}});
                    if (method == "tools/call")
                        result = {{"content",
                                   hm::Json::array({{{"type", "text"}, {"text", value.dump()}}})},
                                  {"structuredContent", value},
                                  {"isError", false}};
                    else
                        result = {{"contents", hm::Json::array({{{"uri", "hugin-munin://status"},
                                                                 {"mimeType", "application/json"},
                                                                 {"text", value.dump()}}})}};
                } catch (const std::exception &e) {
                    if (method != "tools/call")
                        throw;
                    result = {
                        {"content", hm::Json::array({{{"type", "text"}, {"text", e.what()}}})},
                        {"isError", true}};
                }
            } else {
                std::cout << hm::Json(
                                 {{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"error", {{"code", -32601}, {"message", "Method not found"}}}})
                                 .dump()
                          << '\n'
                          << std::flush;
                continue;
            }
            std::cout << hm::Json({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}).dump()
                      << '\n'
                      << std::flush;
        } catch (const hm::Json::parse_error &) {
            std::cout << hm::Json({{"jsonrpc", "2.0"},
                                   {"id", nullptr},
                                   {"error", {{"code", -32700}, {"message", "Parse error"}}}})
                             .dump()
                      << '\n'
                      << std::flush;
        } catch (const std::exception &e) {
            std::cout << hm::Json({{"jsonrpc", "2.0"},
                                   {"id", id},
                                   {"error", {{"code", -32602}, {"message", e.what()}}}})
                             .dump()
                      << '\n'
                      << std::flush;
        }
    }
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
