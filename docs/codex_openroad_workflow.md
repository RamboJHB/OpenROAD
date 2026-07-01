# Codex OpenROAD Workflow

This file records the workflow rule for Codex work on this OpenROAD project.

## Source Of Truth

All future Codex work for this project must use the GitHub branch as the primary workspace:

https://github.com/RamboJHB/OpenROAD/compare/2023-base...RamboJHB:OpenROAD:claude/filler-vt-overlay-repair-plan-2023

The target branch is:

```text
claude/filler-vt-overlay-repair-plan-2023
```

## Required Workflow

1. Read, inspect, edit, and validate files on the GitHub branch first.
2. Do not treat the local `C:\Users\hbjiang\Documents\OpenRoad` directory as the source of truth.
3. Do not default to editing local files and then pushing from local.
4. After GitHub work is complete, sync or copy the final GitHub state back to local as a backup only.
5. If GitHub connector access is unavailable or unstable, report that clearly and provide a GitHub web-editable patch or exact file content for manual update.

## Local Directory Role

The local directory is only a backup/mirror for convenience. It may be used for reading, temporary comparison, or backup after the GitHub branch has been updated, but it must not lead the workflow.
