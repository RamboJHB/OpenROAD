# dpl2 actionable change alert feed

This branch exists only to keep a dedicated pull request open as the notification feed for actionable `src/dpl2/**` changes.

Do not merge or close the feed pull request while monitoring is desired. The GitHub Actions workflow on `claude/wizardly-carson-secahu` posts deduplicated summaries there with:

- what changed;
- why it matters to the dpl2/filler-repair integration;
- implementation and validation follow-up.

The marker file intentionally does not modify dpl2 runtime code.
