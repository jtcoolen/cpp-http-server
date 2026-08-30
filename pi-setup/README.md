# pi configuration to avoid this session's pitfalls

Ready-to-install config that maps each failure in `../REVIEW.md` to a concrete pi
lever. Everything here is verified against your actual setup: pi `0.80.3`, the
`llama-local` server at `192.168.1.203:8080` running `Qwen3.8-Flash-Next` (Q4_K_XL)
with `n_ctx = 196608`, `total_slots = 1`, `reasoning_format = none`.

## The single biggest issue is already fixed
The "133k context overflow" that cost ~34 minutes and two hard errors was a
too-small window: at session time the model was capped at **133,120** tokens. Your
server now runs **196,608** and `models.json` matches, so that exact failure cannot
recur. Nothing to do — just don't lower it.

## What to install (in priority order)

### 1. A knowledge base — the highest-leverage, lowest-effort fix
pi auto-loads `AGENTS.md` at startup (global + per project). This is where the
"act early / verify the negative / measure before claiming / absolute paths / ask
before rm" habits become permanent instead of being re-learned each session.

```sh
cp pi-setup/AGENTS.global.md   ~/.pi/agent/AGENTS.md      # global habits
cp pi-setup/AGENTS.project.md  /Users/julian/http-server/AGENTS.md   # this repo's build/VM rules
```
Restart pi or run `/reload` after editing either file.

### 2. Bound the thinking budget — fixes the 47 minutes of silent stalls
Three turns spent their entire ~16K output budget *thinking* and ended with no tool
call; pi then dropped the turn, so the design was re-derived three times. The model
reasons well — the problem is it never stops to act. Cap thinking so a turn always
has room left to call a tool.

- **pi side:** merge `settings.snippet.json` into `~/.pi/agent/settings.json`
  (`defaultThinkingLevel: "low"` + `thinkingBudgets`). Do **not** just raise max
  output — the trace shows it would keep drafting.
- **Authoritative side (this server):** `reasoning_format` is `none` and the model
  advertises `supportsReasoningEffort: false`, so the reliable cap is server-side.
  Restart llama-server with a reasoning budget, e.g. `--reasoning-budget 6144`
  (0 disables thinking entirely; Qwen3 also honours a `/no_think` suffix). This is
  the lever that actually bounds a runaway monologue for this model.

### 3. Install the guardrail extension — turns the incidents into hard blocks
`guardrails.ts` uses pi's `tool_call` hook (same API as the bundled
`protected-paths.ts` / `confirm-destructive.ts` examples) to:
- put a 10-minute **default timeout** on every bash call (the tar ran 35 min);
- **refuse** `tar -c … .` (and `cp`/`rsync` of a bare `.`) without `-C` — the exact
  missing-`cd` that streamed `$HOME` into the VM;
- **ask** before `rm -rf` (block it when there's no UI);
- **block** a single `write` over ~24 KB — the shape that lost the 27 KB test file
  and overflowed the context.

```sh
mkdir -p ~/.pi/agent/extensions
cp pi-setup/guardrails.ts ~/.pi/agent/extensions/guardrails.ts
# add  "extensions": ["extensions/guardrails.ts"]  to ~/.pi/agent/settings.json
```
Extensions load from trusted locations; `/reload` or restart to activate. Tune the
two constants at the top to taste.

### 4. Keep auto-compaction on (it is, by default)
`compaction.enabled` defaults to `true`. The snippet bumps `reserveTokens` to 24576
(your per-turn output can be ~16K) and `keepRecentTokens` to 24000 so a summary
never strands a half-finished turn. With a 192K window this is now just insurance.

## Pitfall → lever map

| Pitfall (REVIEW.md) | Lever |
|---|---|
| 47 min of thinking-only turns, design re-derived 3× | `thinkingBudgets` + `--reasoning-budget` (server) + AGENTS "act early" |
| 27 KB write lost → context overflow ×2 | guardrails `write` cap; AGENTS "path first, chunk >8 KB"; window already 192K |
| `$HOME` copied into the VM (35 min, disk full) | guardrails `tar -c … .` block + default bash timeout; AGENTS "absolute paths / `tar -C`" |
| `rm -rf` after "do not remove files" | guardrails `rm -rf` confirm; AGENTS "ask before any rm" |
| "validated clean" from grepping the wrong stream | AGENTS "verify the negative / check exit codes + server logs" (habit, not enforceable) |
| README metrics written from memory | AGENTS "measure before you write" |
| invented "GitHub removed push-to-create" fact | AGENTS "state only what the evidence shows" |
| silent stalls, unanswered status, undisclosed lost work | AGENTS "report lost work / answer status" |

## What pi cannot enforce for you
Guardrails and budgets stop the mechanical accidents. The judgement failures —
trusting your own comment instead of running the code, claiming a clean sanitizer
run without reading the log, writing a metric from memory — are habits. AGENTS.md
states them; the durable fix is the workflow in `REVIEW.md §5.3`: **persist don't
ruminate, verify the negative, measure then write.** For high-stakes correctness,
a second *adversarial* pass (a fresh session told to refute and to run the binary)
is what caught all 20 findings here; a self-review in the same context mostly
re-reads its own intent.

## Optional: a "ship a server change" skill
If you want the verify-the-negative workflow to be invokable on demand, make a
skill at `~/.pi/agent/skills/ship-server-change/SKILL.md` whose steps are: (1) write
a raw-socket test that fails on the current binary; (2) fix; (3) rebuild in the VM
under ASan+TSan; (4) grep every server log and assert exit 0; (5) run the full
suite. pi can scaffold it — ask it to "create a skill for shipping a server change".
