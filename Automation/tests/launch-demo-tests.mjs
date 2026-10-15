import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import fs from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const launcher = path.join(repositoryRoot, "Automation", "launch", "launch-demo.ps1");

function runPowerShell(args) {
  return new Promise((resolve, reject) => {
    const child = spawn(
      "powershell.exe",
      ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", launcher, ...args],
      { cwd: repositoryRoot, windowsHide: true },
    );
    let stdout = "";
    let stderr = "";
    const timeout = setTimeout(() => {
      child.kill();
      reject(new Error(`launcher timed out\nstdout:\n${stdout}\nstderr:\n${stderr}`));
    }, 20_000);

    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk) => {
      stdout += chunk;
    });
    child.stderr.on("data", (chunk) => {
      stderr += chunk;
    });
    child.on("error", reject);
    child.on("exit", (code) => {
      clearTimeout(timeout);
      resolve({ code, stdout, stderr });
    });
  });
}

test("launch-demo opens a resolved replay and reports an agent-readable ready session", async () => {
  const temporaryRoot = await fs.mkdtemp(path.join(os.tmpdir(), "source-mvm-launch-demo-"));
  const cs2Directory = path.join(temporaryRoot, "Counter-Strike Global Offensive");
  const demoPath = path.join(cs2Directory, "game", "csgo", "replays", "example.dem");
  const outputDirectory = path.join(temporaryRoot, "run");
  await fs.mkdir(path.dirname(demoPath), { recursive: true });
  await fs.writeFile(demoPath, "test demo placeholder");

  const commands = [];
  let probeCount = 0;
  const server = net.createServer((socket) => {
    socket.setEncoding("ascii");
    let pending = "";
    socket.on("data", (chunk) => {
      pending += chunk;
      while (pending.includes("\n")) {
        const newline = pending.indexOf("\n");
        const command = pending.slice(0, newline).replace(/\r$/, "");
        pending = pending.slice(newline + 1);
        commands.push(command);

        if (command.startsWith("playdemo ")) {
          socket.write("Playing demo request accepted\n");
        } else if (command === "mirv_filmmaker health") {
          socket.write("mirv_filmmaker health:\n  => healthy: every version-coupled subsystem resolved.\n");
        } else if (command === "mirv_filmmaker demoprobe") {
          probeCount += 1;
          const level = probeCount === 1 ? "" : "de_dust2";
          socket.write(
            `[demoprobe] IDemoFile=00000001 IsPlayingDemo=1\n` +
              `[demoprobe] RESOLVED = '${demoPath}'\n` +
              `[demoprobe] level = '${level}'\n`,
          );
        }
      }
    });
  });

  try {
    await new Promise((resolve, reject) => {
      server.once("error", reject);
      server.listen(0, "127.0.0.1", resolve);
    });
    const address = server.address();
    assert.equal(typeof address, "object");

    const result = await runPowerShell([
      "-NoLaunch",
      "-Demo",
      "replays/example",
      "-Port",
      String(address.port),
      "-Cs2Dir",
      cs2Directory,
      "-OutDir",
      outputDirectory,
      "-DemoReadyTimeoutSeconds",
      "5",
      "-ReadPollMilliseconds",
      "25",
    ]);

    assert.equal(result.code, 0, `stdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
    assert.ok(commands.includes('playdemo "replays/example"'));
    assert.ok(commands.includes("mirv_filmmaker health"));
    assert.ok(commands.includes("mirv_filmmaker demoprobe"));
    assert.ok(probeCount >= 2, "launcher must wait for a loaded level, not only a resolved demo path");

    const session = JSON.parse(await fs.readFile(path.join(outputDirectory, "session.json"), "utf8"));
    assert.equal(session.state, "ready");
    assert.equal(path.normalize(session.demoPath), path.normalize(demoPath));
    assert.equal(session.playdemoArgument, "replays/example");
    assert.equal(session.hookHealthy, true);
    assert.equal(session.netcon.host, "127.0.0.1");
    assert.equal(session.netcon.port, address.port);
  } finally {
    await new Promise((resolve) => server.close(resolve));
    await fs.rm(temporaryRoot, { recursive: true, force: true });
  }
});
