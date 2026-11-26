# Build/deploy workflow

- Build/deploy only via subagents `mpc-hc-build` / `mpc-hc-deploy`.
- The main session does not run `tools\build.bat`, `robocopy` or copy commands itself:
  it asks for targets, invokes the agent, and reads the report.
- Pass all targets of the current stage at once (arch/config for build; both
  `C:\Program Files\MPC-HC` and `samsung-pc` for deploy). The agent handles them in
  one invocation; do not return to the main session between targets.
- Detailed procedures live in `.opencode/commands/build.md` and `.opencode/commands/deploy.md`.
- Deploy: check whether `mpc-hc64` is running (`Get-Process mpc-hc64`). If it is, do not kill
  it — the deploy script renames locked files to `.old` and copies the new ones; remind the
  user to restart the player.
