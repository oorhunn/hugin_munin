"""Two independent stdio clients must observe one service and owned, idempotent jobs."""
import json
import subprocess
import sys

clients = []
registrations = []
sequence = 0

def rpc(client, method, params=None):
    global sequence
    sequence += 1
    client.stdin.write(json.dumps({"jsonrpc": "2.0", "id": sequence, "method": method, "params": params or {}}) + "\n")
    client.stdin.flush()
    response = json.loads(client.stdout.readline())
    assert response["id"] == sequence, response
    return response

def call(client, name, arguments=None, error=False):
    result = rpc(client, "tools/call", {"name": name, "arguments": arguments or {}})["result"]
    assert result["isError"] == error, result
    return result.get("structuredContent", result)

try:
    for _ in range(2):
        client = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        clients.append(client)
        assert rpc(client, "initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})["result"]["serverInfo"]["name"] == "hugin-munin"
        assert len(rpc(client, "tools/list")["result"]["tools"]) >= 8
        auth = {"agent_token": call(client, "register_agent", {"name": "Integration test", "project": "hm tests"})["agent_token"]}
        registrations.append((client, auth, []))
    c1, a1, jobs1 = registrations[0]
    c2, a2, jobs2 = registrations[1]
    request = {**a1, "request_key": "test", "title": "Integration test job", "cpu_cores": 0.1, "memory_bytes": 1024 * 1024, "estimated_duration_seconds": 42}
    job = call(c1, "request_lease", request)
    jobs1.append(job["id"])
    assert call(c1, "request_lease", request)["id"] == job["id"]
    snapshot = call(c2, "list_jobs")
    assert any(j["id"] == job["id"] and j["estimated_duration_seconds"] == 42 for j in snapshot["jobs"])
    call(c2, "release_lease", {**a2, "job_id": job["id"]}, error=True)
    assert rpc(c2, "resources/read", {"uri": "hugin-munin://status"})["result"]["contents"]
    call(c1, "heartbeat", {**a1, "job_id": job["id"], "remaining_seconds": 12})
    client = clients[0]
    client.stdin.write("{bad json\n")
    client.stdin.flush()
    assert json.loads(client.stdout.readline())["error"]["code"] == -32700
finally:
    for client, auth, jobs in registrations:
        for job in jobs:
            call(client, "release_lease", {**auth, "job_id": job})
        call(client, "unregister_agent", auth)
    for client in clients:
        client.stdin.close()
        client.wait(timeout=5)
