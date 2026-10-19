export const meta = {
  name: "diablo-doom-loop",
  description: "DiabloDoom farm loop: each cycle a worker reads PROMPT.md/SYSTEM_PROMPT.md, plays the existing binary, and reports kills + screenshot.",
  phases: [{ name: "cycle", title: "Farm cycle" }, { name: "report", title: "Report" }]
};

const inputs = args ?? {};
const cycles = inputs.cycles ?? 1;

const results = [];

for (let i = 0; i < cycles; i++) {
  phase("cycle", `Farm cycle ${i + 1} of ${cycles}`);
  const r = await agent(
    "You are the DiabloDoom farm worker. " +
    "Read ~/workspace/DiabloDoom/scripts/SYSTEM_PROMPT.md and ~/workspace/DiabloDoom/scripts/PROMPT.md and follow them exactly. " +
    "This is farm cycle " + (i + 1) + " of " + cycles + ". " +
    "Setup: verify the game process is running (pgrep -f 'chocolate-doo[m]'); launch per SYSTEM_PROMPT.md if needed. " +
    "NEVER pull from git, NEVER rebuild the binary, NEVER delete progress.dat. " +
    "Play: ~4 minutes of vision-driven play per PROMPT.md (explore, fight in turn-based combat, COLLECT loot on the 'press G to loot' prompt). " +
    "Use the game's SCREENSHOT driver command for observations (write SCREENSHOT to /tmp/doom_cmd.txt). " +
    "Save/resume: send LOAD 0 first after boot, SAVE 0 ~1 min before you finish. " +
    "Report back JSON only.",
    {
      key: "farm-cycle-" + (i + 1),
      label: "Farm cycle " + (i + 1),
      schema: {
        type: "object",
        properties: {
          kills: { type: "number" },
          deaths: { type: "number" },
          hp: { type: "number" },
          screenshot_path: { type: "string" },
          notes: { type: "string" }
        },
        required: ["kills", "screenshot_path", "notes"]
      },
      timeoutMs: 600000
    }
  );
  results.push(r);
  log("Cycle " + (i + 1) + ": " + r.kills + " kills. " + r.notes);
}

phase("report", "Final report");
const totalKills = results.reduce((s, r) => s + r.kills, 0);

return {
  message: "Farm loop complete: " + cycles + " cycle(s), " + totalKills + " total kills.",
  cycles: results
};