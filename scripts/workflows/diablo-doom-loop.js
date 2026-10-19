export const meta = {
  name: "diablo-doom-loop",
  description: "DiabloDoom farm: one long-lived driver agent runs all observe->vision->act iterations.",
  phases: [
    { name: "setup", title: "Game setup" },
    { name: "farm", title: "Farm run" },
    { name: "report", title: "Report" }
  ]
};

const inputs = args ?? {};
const iterations = inputs.iterations ?? 40;
const SCRIPT = "~/workspace/DiabloDoom/scripts/doom_loop.sh";
const CMD_FILE = "/tmp/doom_next_cmds.txt";

const setupSchema = {
  type: "object",
  properties: {
    ok: { type: "boolean" },
    launched: { type: "boolean" },
    killed: { type: "number" },
    save: { type: "string" },
    notes: { type: "string" }
  },
  required: ["ok", "notes"]
};

const reportSchema = {
  type: "object",
  properties: {
    iterations_completed: { type: "number" },
    kills: { type: "number" },
    hung_at: {
      type: ["object", "null"],
      properties: {
        iteration: { type: "number" },
        stage: { type: "string" }
      }
    },
    notes: { type: "string" },
    cycles: {
      type: "array",
      items: {
        type: "object",
        properties: {
          iteration: { type: "number" },
          commands: { type: "array", items: { type: "string" } },
          kills: { type: "number" }
        }
      }
    }
  },
  required: ["iterations_completed", "kills", "notes"]
};

async function runSetup(key) {
  return await agent(
    "Run: bash " + SCRIPT + " --subagent setup. " +
    "It pulls the latest from GitHub and rebuilds the binary if anything changed, " +
    "kills any existing DiabloDoom game processes, launches a fresh game, " +
    "then LOADs autosave slot 1 if it exists or SAVE 0 to initialize it. " +
    "NEVER delete progress.dat. " +
    "Return the JSON it prints.",
    { key: key, label: "Game setup", schema: setupSchema, timeoutMs: 600000 }
  );
}

// ---- setup ----
phase("setup", "Game setup");
const setup = await runSetup("setup");
log("Setup: " + setup.notes);
if (!setup.ok) {
  return {
    message: "Farm setup failed: " + setup.notes,
    hung_at: { iteration: 0, stage: "setup" },
    cycles: []
  };
}

// ---- farm: one long-lived driver agent, no per-iteration respawn ----
phase("farm", "Farm run (" + iterations + " iterations)");
const report = await agent(
  "You are the DiabloDoom farm driver. This is a LONG-RUNNING task: you stay alive for the whole run, " +
  "no respawning. Read ~/workspace/DiabloDoom/scripts/SYSTEM_PROMPT.md (command reference and mechanics) " +
  "and ~/workspace/DiabloDoom/scripts/PROMPT.md (strategy) ONCE at the start, then run " + iterations + " iterations. " +
  "The loop is flipped: ACT -> REPLAY -> THINK, repeated. The replay always follows your commands, " +
  "so the video shows real gameplay, never idle thinking time.\n" +
  "First iteration: OBSERVE once to see the starting state (bash " + SCRIPT + " --subagent observe, " +
  "which sends REPLAY and prints JSON {replay_path, log_tail, kills}), then enter the loop.\n" +
  "Each iteration:\n" +
  "1. ACT: Write your commands (one per line) to " + CMD_FILE + " and run: bash " + SCRIPT + " --subagent act --file " + CMD_FILE + ". " +
  "Queue aggressively: batch 6-12 commands (moves + attacks + END), never just 1-2.\n" +
  "2. REPLAY: Wait ~5 seconds for your commands to start playing out, then run: bash " + SCRIPT + " --subagent observe. " +
  "It sends REPLAY (dumps the 15s ring buffer) and prints JSON {replay_path, log_tail, kills}. " +
  "This replay shows what your commands just did.\n" +
  "3. THINK: Extract 2 frames at half resolution: ffmpeg -y -i \"<replay_path>\" -vf fps=1/8,scale=480:300 -q:v 3 /tmp/vf/f%02d.jpg (mkdir -p /tmp/vf first). " +
  "View them, decide your next batch of commands per PROMPT.md. Then loop back to ACT.\n" +
  "If observe reports an error (game not running), run: bash " + SCRIPT + " --subagent setup, then retry once. " +

  "If still failing, record hung_at={iteration, stage:'observe'} and stop.\n" +

  "Target ~30-45 seconds per iteration. If a step hangs beyond its natural time, note which stage hung: " +
  "skip to the next iteration, or stop the run if the game is dead (record hung_at).\n" +
  "After all iterations (or on stop), SAVE the game: write the single line SAVE 0 to " + CMD_FILE +
  " and run the act script once more.\n" +
  "NEVER delete progress.dat. Only the setup script pulls/rebuilds; never do it yourself. " +
  "Never kill the game yourself; only the setup script may do that. " +
  "Return JSON {iterations_completed, kills (last cumulative count), hung_at (or null), " +
  "notes, cycles: [{iteration, commands, kills}]}.",
  { key: "driver", label: "Farm driver", schema: reportSchema, timeoutMs: 3600000 }
);

// ---- report ----
phase("report", "Final report");
let message = "Farm run: " + report.iterations_completed + " iteration(s), " + report.kills + " kills.";
if (report.hung_at) {
  message += " Hung at iteration " + report.hung_at.iteration + " (stage: " + report.hung_at.stage + ").";
}
message += " " + report.notes;

return {
  message: message,
  hung_at: report.hung_at || null,
  cycles: report.cycles || []
};