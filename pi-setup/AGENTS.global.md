# Operating rules (global)

Standing instructions for every session. Distilled from the failures analysed in
`http-server/REVIEW.md` §4–§5. Copy this to `~/.pi/agent/AGENTS.md`.

## Act, don't ruminate
- Emit a tool call early in every turn. Do **not** design a whole project inside a
  reasoning block — thinking is discarded when a turn hits the output limit.
- Build one file at a time: write a compilable skeleton, then refine. Never write
  more than one or two files before compiling/running.
- Anything worth more than a screen of thought is worth a file. Put a design in a
  `DESIGN.md` or in header stubs, where it survives truncation.

## File writes
- Put `path` before `content` in a `write` call.
- Keep a single `write` under ~8 KB. For a larger file, write a small first chunk,
  then append with `cat >> file <<'EOF' … EOF`.
- When context is tight, chunk proactively — do not wait until a write is lost.

## Shell hygiene (the cwd is often `$HOME`)
- Always use absolute paths, or `tar -C <absolute-dir>` / `cd <dir> &&` in the
  **same** command. Never rely on the inherited working directory.
- Never `2>/dev/null` a bulk copy or long operation — capture stderr and check it.
- Run `du -sh <dir>` before streaming a directory anywhere.
- Put a timeout on any command that could hang; report elapsed time for slow ones.
- Avoid `pkill -f <pattern>` where the pattern can match your own shell.

## Destructive / outward-facing actions — state intent and wait one message
- Any `rm` (especially `rm -rf`), even inside a VM or container you consider
  disposable. If a user said "do not remove files", treat every `rm` as ask-first.
- Installing software, creating a VM/container, `git init`/`commit`/`push`,
  choosing a git author identity or a remote/repo name.
- When an accident copies data, enumerate what moved with `ls -la` (dotfiles too —
  `.ssh`, `.*_history`, tokens) and disclose it before cleaning up.

## Verify the negative before you claim success
- "0 sanitizer reports", "leak-free", "handles X" mean nothing unless the detector
  could have fired. Prove it: break the feature (or inject a fault) and confirm the
  check goes red once.
- Point the check at the right stream: assert process **exit codes** and grep the
  **server's** stderr/log, not just the test runner's stdout.
- Measure before you write. Generate READMEs, summaries and metrics from tool
  output you just ran, not from memory; mark any derived number as derived.
- State only what the evidence shows. If the error message answers the question,
  quote it — don't invent supporting "facts" (e.g. platform history) to explain it.

## Diagnosis
- When something surprising happens, re-read your own last few commands verbatim
  before theorising about causes. The culprit is usually in your context already.

## Communication
- After any turn that lost work (output-limit truncation, a failed write, a context
  error), open the next turn with one line saying what was lost and what changed.
- Answer status questions ("where are we?") in text before running tools.
