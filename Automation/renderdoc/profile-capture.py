"""Profile an existing capture through the local RenderDoc MCP stdio server.

Writes replay counters, actions and the most expensive pipelines to JSON. Replay
timings are diagnostic; use the game's health output for playback percentiles.
"""
import argparse
import json
import pathlib
import queue
import subprocess
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--server", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    replies = queue.Queue()
    with (args.out / "server.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen([str(args.server)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=log,
                                   text=True, encoding="utf-8")
        def read():
            for line in process.stdout:
                replies.put(line)
            replies.put(None)
        threading.Thread(target=read, daemon=True).start()
        serial = 0

        def request(method, params):
            nonlocal serial
            serial += 1
            process.stdin.write(json.dumps(dict(jsonrpc="2.0", id=serial,
                                                method=method, params=params)) + "\n")
            process.stdin.flush()
            line = replies.get(timeout=args.timeout)
            if line is None:
                raise RuntimeError(f"RenderDoc server exited: {process.poll()}")
            reply = json.loads(line)
            if reply.get("id") != serial or "error" in reply:
                raise RuntimeError(reply)
            return reply["result"]

        def call(name, **arguments):
            print(name, arguments, flush=True)
            result = request("tools/call", dict(name=name, arguments=arguments))
            if result.get("isError"):
                raise RuntimeError(result)
            return json.loads(next(item["text"] for item in result["content"]
                                   if item["type"] == "text"))

        def save(name, value):
            (args.out / f"{name}.json").write_text(json.dumps(value, indent=2), encoding="utf-8")

        try:
            request("initialize", dict(protocolVersion="2025-03-26", capabilities={},
                                        clientInfo=dict(name="source-mvm-profile", version="1")))
            process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
            process.stdin.flush()
            save("capture", call("open_capture", path=str(args.capture.resolve())))
            save("resources", call("list_resources", type="Texture"))
            events = call("list_events", limit=0)
            save("events", events)
            counters = call("fetch_counters", counterNames=["GPU Duration"])
            save("counters", counters)
            top = sorted(counters["rows"], key=lambda row: row["value"], reverse=True)[:args.top]
            pipelines = [dict(counter=row, pipeline=call("get_pipeline_state", eventId=row["eventId"]))
                         for row in top]
            save("top-pipelines", pipelines)
            targets = {target["resourceId"] for item in pipelines
                       for target in item["pipeline"].get("renderTargets", [])}
            save("targets", [call("get_resource_info", resourceId=target)
                             for target in sorted(targets)])
            shaders = []
            seen = set()
            for item in pipelines:
                for key, stage in (("pixelShader", "ps"), ("csShader", "cs")):
                    shader = item["pipeline"].get(key)
                    if shader and shader["resourceId"] != "ResourceId::0" and shader["resourceId"] not in seen:
                        seen.add(shader["resourceId"])
                        try:
                            shaders.append(call("get_shader", eventId=item["counter"]["eventId"],
                                                stage=stage, mode="disasm"))
                        except RuntimeError as error:
                            shaders.append(dict(eventId=item["counter"]["eventId"], stage=stage,
                                                error=str(error)))
            save("top-shaders", shaders)
            print(json.dumps(top, indent=2), flush=True)
            call("close_capture")
        finally:
            process.stdin.close()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()  # Only this script's replay helper, never the game.
                process.wait()


if __name__ == "__main__":
    main()
