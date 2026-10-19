export const meta = {
  name: "diablo-doom-loop",
  description: "DiabloDoom farm: setup once, then loop {observe replay -> vision subagent -> act via FIFO}. Reports which stage hangs.",
  phases: [
    { name: "setup", title: "Game setup" },
    { name: "loop", title: "Observe/vision/act loop" },
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
    game_count: { type: "number" },
    save: { type: "string" },
    notes: { type: "string" }
  },
  required: ["ok", "notes"]
};

const observeSchema = {
  type: "object",
  properties: {
    replay_path: { type: "string" },
    log_tail: { type: "string" },
    kills: { type: "number" },
    notes: { type: "string" }
  },
  required: ["replay_path", "log_tail"]
};

const visionSchema = {
  type: "object",
  properties: {
    commands: { type: "array", items: { type: "string" } },
    notes: { type: "string" }
  },
  required: ["commands", "notes"]
};

const actSchema = {
  type: "object",
  properties: {
    ok: { type: "boolean" }
  },
  required: ["ok"]
};

async function runSetup(key) {
  return await agent(
    "Run: bash " + SCRIPT + " --subagent setup. " +
    "It ensures the DiabloDoom game is running (launches only if needed; never kills an existing game), " +
    "then LOADs autosave slot 1 if it exists or SAVE 0 to initialize it. " +
    "NEVER pull from git, NEVER rebuild the binary, NEVER delete progress.dat, NEVER kill the game. " +
    "Return the JSON it prints.",
    { key: key, label: "Game setup", schema: setupSchema, timeoutMs: 120000 }
  );
}

async function runObserve(key) {
  return await agent(
    "Run: bash " + SCRIPT + " --subagent observe. " +
    "It sends REPLAY to the running game, waits for the 15s video, and prints JSON " +
    "with replay_path, log_tail, and kills. " +
    "If it reports an error, return {\"replay_path\": \"\", \"log_tail\": \"\", \"kills\": 0, \"notes\": \"<the error>\"}. " +
    "Otherwise return the JSON it printed.",
    { key: key, label: "Observe", schema: observeSchema, timeoutMs: 120000 }
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

// ---- loop ----
const history = [];
let hung_at = null;

for (let i = 0; i < iterations; i++) {
  phase("loop", "Iteration " + (i + 1) + " of " + iterations);
  const rec = { iteration: i + 1, stage: "observe", commands: [], kills: 0, notes: "" };

  // 1. Observe: REPLAY capture + log tail.
  try {
    let obs = await runObserve("observe-" + (i + 1));
    // Game died mid-loop: re-run setup once, then re-observe.
    if (!obs.replay_path) {
      log("Observe failed (" + (obs.notes || "no replay") + "); re-running setup.");
      await runSetup("setup-retry-" + (i + 1));
      obs = await runObserve("observe-retry-" + (i + 1));
      if (!obs.replay_path) {
        rec.stage = "observe";
        rec.notes = "observe failed after setup retry: " + (obs.notes || "unknown");
        history.push(rec);
        hung_at = { iteration: i + 1, stage: "observe" };
        log("Hung at iteration " + (i + 1) + ", stage: observe");
        break;
      }
    }
    rec.kills = obs.kills || 0;

    // 2. Vision: watch replay, decide commands, write command file.
    rec.stage = "vision";
    const vision = await agent(
      "You are the DiabloDoom farm worker. Read ~/workspace/DiabloDoom/scripts/PROMPT.md and follow it. " +
      "A 15-second replay video was captured at: " + obs.replay_path + " " +
      "Extract frames with ffmpeg (e.g. ffmpeg -y -i \"" + obs.replay_path + "\" -vf fps=1 /tmp/vision_frames/f%02d.png) " +
      "and view them to understand the game state (HUD, enemies, loot, 'press G to loot' prompt). " +
      "Recent game log:\n" + obs.log_tail + "\n" +
      "Decide the next driver commands per PROMPT.md " +
      "(explore: WALK, TURNL, TURNR, COLLECT; combat: ATTACK, MOVE_FWD, MOVE_BACK, STRAFE_L, STRAFE_R, END; system: INVENTORY). " +
      "Write one command per line to " + CMD_FILE + " (plain text, real newlines). " +
      "Return JSON {\"commands\": [\"WALK\", ...], \"notes\": \"...\"}.",
      { key: "vision-" + (i + 1), label: "Vision " + (i + 1), schema: visionSchema, timeoutMs: 180000 }
    );
    rec.commands = vision.commands;
    rec.notes = vision.notes;

    // 3. Act: drive the game via FIFO.
    rec.stage = "act";
    const act = await agent(
      "Run: bash " + SCRIPT + " --subagent act --file " + CMD_FILE + ". " +
      "It writes the commands to the game's FIFO. Return the JSON it prints.",
      { key: "act-" + (i + 1), label: "Act " + (i + 1), schema: actSchema, timeoutMs: 60000 }
    );
    rec.stage = "done";
    if (!act.ok) {
      rec.notes = rec.notes + " [act reported not-ok]";
    }
  } catch (e) {
    rec.notes = "hung/failed at stage '" + rec.stage + "': " + (e && e.message ? e.message : e);
    history.push(rec);
    hung_at = { iteration: i + 1, stage: rec.stage };
    log("Hung at iteration " + (i + 1) + ", stage: " + rec.stage);
    break;
  }

  history.push(rec);
  log("Iter " + (i + 1) + ": [" + rec.commands.join(", ") + "] kills=" + rec.kills);
}

// ---- report ----
phase("report", "Final report");
const lastKills = history.length > 0 ? history[history.length - 1].kills : 0;
let message = "Farm loop complete: " + history.length + " iteration(s), " + lastKills + " kills (log cumulative).";
if (hung_at) {
  message += " Hung at iteration " + hung_at.iteration + " (stage: " + hung_at.stage + ").";
}

return {
  message: message,
  hung_at: hung_at,
  cycles: history
};