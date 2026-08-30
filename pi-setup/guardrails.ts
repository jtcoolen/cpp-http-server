/**
 * pi-guardrails — hard guardrails derived from the 2026-08-30 http-server session
 * (see http-server/REVIEW.md §4–§5). These are *blocking* rules; the soft habits
 * live in AGENTS.md.
 *
 * Install:
 *   mkdir -p ~/.pi/agent/extensions
 *   cp guardrails.ts ~/.pi/agent/extensions/guardrails.ts
 *   # then add  "extensions": ["extensions/guardrails.ts"]  to ~/.pi/agent/settings.json
 *   # (paths in the global settings resolve relative to ~/.pi/agent)
 * Reload with /reload or restart pi.
 *
 * What it does:
 *   1. Puts a default timeout on every bash call (the original tar ran 35 min).
 *   2. Refuses `tar -c … .` (and cp/rsync of a bare ".") without -C — the missing
 *      `cd`/`-C` that streamed $HOME into the VM.
 *   3. Asks before `rm -rf` (blocks it in non-interactive mode).
 *   4. Blocks a single oversized `write` — the shape that lost the 27 KB test file
 *      and overflowed the context.
 */

import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { isToolCallEventType } from "@earendil-works/pi-coding-agent";

const DEFAULT_BASH_TIMEOUT_MS = 600_000; // 10 min
const MAX_WRITE_CHARS = 24_000; // ~6k tokens; the lost write was ~27 KB

function isRecursiveForceRm(cmd: string): boolean {
  if (!/\brm\b/.test(cmd)) return false;
  if (/\brm\b[^|&;\n]*\s-[a-zA-Z]*r[a-zA-Z]*f/.test(cmd)) return true; // -rf, -Rf, ...
  if (/\brm\b[^|&;\n]*\s-[a-zA-Z]*f[a-zA-Z]*r/.test(cmd)) return true; // -fr
  const hasR = /\brm\b[^|&;\n]*(\s-[a-zA-Z]*r|\s--recursive)/.test(cmd);
  const hasF = /\brm\b[^|&;\n]*(\s-[a-zA-Z]*f|\s--force)/.test(cmd);
  return hasR && hasF;
}

// tar creating an archive whose source is a bare "." with no -C, or cp/rsync of ".".
// Inspect each pipeline/list segment on its own: a "-C" on the *extract* side of a
// pipe must not excuse a missing "-C" on the *create* side (this is the exact shape
// of the incident: `tar -cf - . | limactl ... 'tar -xf - -C ~/proj'`).
function isRelativeBulkCopy(cmd: string): boolean {
  for (const seg of cmd.split(/\||&&|;/)) {
    const hasDashC = /\s-C(\s|=)/.test(seg); // tar -C <dir>
    const dotSource = /(^|\s)\.(\s|$|'|")/.test(seg);
    const tarCreate = /\btar\b[^\n]*(\s-c|\s--create|\s-[a-zA-Z]*c[a-zA-Z]*f)/.test(seg);
    if (tarCreate && !hasDashC && dotSource) return true;
    if (/\b(cp|rsync)\b[^\n]*\s\.(\s|$)/.test(seg) && !hasDashC) return true;
  }
  return false;
}

export default function (pi: ExtensionAPI) {
  pi.on("tool_call", async (event, ctx) => {
    if (isToolCallEventType("bash", event)) {
      const cmd = event.input.command ?? "";

      // 1) never run untimed
      if (event.input.timeout == null) event.input.timeout = DEFAULT_BASH_TIMEOUT_MS;

      // 2) the home-directory-copy trap
      if (isRelativeBulkCopy(cmd)) {
        return {
          block: true,
          reason:
            "Bulk copy from a relative '.' without -C: this copies the current directory, " +
            "which is often $HOME. Use an absolute source, e.g. `tar -C <absolute-dir> -cf - .`.",
        };
      }

      // 3) rm -rf
      if (isRecursiveForceRm(cmd)) {
        if (!ctx.hasUI) {
          return { block: true, reason: "rm -rf blocked in non-interactive mode" };
        }
        const ok = await ctx.ui.confirm("Allow recursive delete?", cmd);
        if (!ok) {
          ctx.ui.notify("rm -rf cancelled", "info");
          return { block: true, reason: "user declined rm -rf" };
        }
      }
      return undefined;
    }

    // 4) oversized single write
    if (isToolCallEventType("write", event)) {
      const content = (event.input as { content?: string }).content ?? "";
      if (content.length > MAX_WRITE_CHARS) {
        if (ctx.hasUI) {
          ctx.ui.notify(`Blocked ${content.length}-char write (limit ${MAX_WRITE_CHARS})`, "warning");
        }
        return {
          block: true,
          reason:
            `write is ${content.length} chars; keep a single write under ${MAX_WRITE_CHARS}. ` +
            "Write a smaller file first, then append the rest with `cat >> <file> <<'EOF' … EOF`.",
        };
      }
    }
    return undefined;
  });
}
