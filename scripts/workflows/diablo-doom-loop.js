export const meta = {
  name: "diablo-doom-loop",
  description: "DiabloDoom farm loop: each cycle a worker runs doom_loop.sh --subagent and reports the JSON result.",
  phases: [{ name: "cycle", title: "Farm cycle" }, { name: "report", title: "Report" }]
};

const inputs = args ?? {};
const cycles = inputs.cycles ?? 1;

const results = [];

for (let i = 0; i < cycles; i++) {
  phase("cycle", `Farm cycle ${i + 1} of ${cycles}`);
  const r = await agent(
    "Run the DiabloDoom farm cycle by executing: bash ~/workspace/DiabloDoom/scripts/doom_loop.sh --subagent " +
    "This is farm cycle " + (i + 1) + " of " + cycles + ". " +
    "The script handles game launch, vision-driven play per PROMPT.md/SYSTEM_PROMPT.md (it reads them itself), and save/resume. " +
    "NEVER pull from git, NEVER rebuild the binary, NEVER delete progress.dat. " +
    "Wait for the script to finish (it runs ~5 minutes). " +
    "The script prints a JSON summary to stdout at the end. Parse it and return it as your result. " +
    "If the script fails, return what you observed: {\"kills\": 0, \"deaths\": 0, \"iterations\": 0, \"screenshot_path\": \"\", \"replay_path\": \"\", \"notes\": \"<what happened>\"}.",
    {
      key: "farm-cycle-" + (i + 1),
      label: "Farm cycle " + (i + 1),
      schema: {
        type: "object",
        properties: {
          kills: { type: "number" },
          deaths: { type: "number" },
          iterations: { type: "number" },
          screenshot_path: { type: "string" },
          replay_path: { type: "string" },
          notes: { type: "string" }
        },
        required: ["kills", "notes"]
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