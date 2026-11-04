# NIFDiff working conventions

## Task tracking: GitHub Issues

- Use https://github.com/floyd68/NIFDiff/issues as the sole persistent task
  tracker. Issue scope, acceptance criteria, dependencies, and current comments
  take precedence over historical local task notes.
- Read the relevant issue before implementation. For the code-review work,
  start with tracking issue #14 and follow its linked issues and dependencies.
- Record implementation progress, blockers, verification results, and completion
  in the relevant issue. Reuse existing issues rather than creating duplicates.
- Do not create, update, or consult todos.md for active task tracking or working
  instructions. The existing file is a historical archive only. Do not maintain
  a parallel local backlog or automatically migrate its old entries.
- If GitHub is unavailable, report the limitation; do not silently fall back to
  todos.md. Temporary in-conversation planning is allowed.
- The repository is public: keep issue/PR text free of local absolute paths,
  credentials and private data, and do not write bare `@name` mentions.
- Issue format: English first; when a Korean version exists, keep it below a
  `---` separator under `## 한국어 원문`. Tick only the English checklist.

## Labels

| Label | Meaning |
|---|---|
| `priority:high` / `priority:medium` / `priority:low` | Work order |
| `blocked` | Waiting on another issue or PR; skip when picking work |
| `changes-requested` | Review found problems; fix on the same PR branch |
| `approved` | Review passed; ready to merge |
| `needs-manual-test` | Requires running the app; CI cannot verify it |
| `ci` | Build / CI infrastructure |

## Command: "진행" (implement)

1. **Fix rejected work first.** If an open PR carries `changes-requested`, or an
   issue has unresolved review feedback, address that before anything new:
   push fixes to the same branch, reply on the PR with what changed, remove
   `changes-requested`.
2. Otherwise pick the next issue: not `blocked`, highest `priority:*`, then the
   order in tracking issue #14, then the lowest number. Keep **at most one open
   implementation PR** at a time to avoid conflicts.
3. Comment on the issue that work has started; create branch
   `issue-<N>-<short-slug>` from up-to-date `master`.
4. Implement, then build Debug and Release and run the full test suite locally.
   Follow the issue's acceptance criteria; user-visible changes update README /
   CHANGELOG (under `## Unreleased`) in the same PR.
5. Open a PR whose body contains `Fixes #N`, a summary, verification results
   (commands and outcomes), and any remaining risks. Add `needs-manual-test`
   with concrete manual steps when CI cannot verify the change.
6. Record progress / verification on the issue. Do not merge your own PR in
   this step.

## Command: "리뷰" (review)

1. Review the open PR together with its linked issue. Review from a fresh
   context (a reviewer without the implementation conversation, e.g. a separate
   agent given only the diff and the issue), checking the diff against the
   issue's acceptance criteria.
2. Check CI status; a failing or missing required check is a review failure.
3. Check the branch is up to date with `master` and merges cleanly.
4. **Problems found:** leave review comments on the PR (inline where possible),
   add `changes-requested`, keep the PR open. Problems outside the PR's scope go
   into a new issue (or the relevant existing issue) instead.
5. **No problems:** add `approved`, squash-merge (never with `--admin`; `master`
   protection requires `build (Debug)` and `build (Release)` and admins can
   bypass it), delete the branch, confirm the
   issue closed via `Fixes #N`, and tick the item in tracking issue #14.
   The reviewer is authorized to merge every PR that passes review, except a
   `needs-manual-test` PR, which waits until the manual test is reported done.
6. GitHub does not allow approving or requesting changes on a PR authored by
   the same account; review state is expressed with labels and comments.

## Command: "배포" (release)

Issued by the maintainer only after they have checked `master`; it releases
the latest `master` commit. Never start a release without this command.

1. **Preflight:** local `master` equals `origin/master`, the working tree is
   clean, no implementation PR is half-merged, and the latest `master` commit's
   CI (`build (Debug)` / `build (Release)`) is green. Otherwise stop and report.
2. **Release docs:** user-visible PRs add their entries to `## Unreleased` at
   the top of `CHANGELOG.md` as they merge. At release time, check that section
   against the PRs merged since the last `v*` tag (add anything missing), then
   rename it to `## <MAJOR>.<MINOR>.<count+1>`, where `<count>` is
   `git rev-list --count HEAD` before the release commit. Update
   `NexusMods_Mod_Description.bbcode` (version / What's New) and README where
   needed. A MAJOR/MINOR bump goes through a normal PR beforehand, never in
   the release commit.
3. **Release commit:** commit only those docs as `Release <version>: <summary>`,
   run `.\Release.ps1 -Publish` on `master` (preflight, tag `v<version>`, push
   the commit and the tag; atomically with `git push --atomic` once #20 lands).
   If the push is rejected because `master` moved: inspect local and remote
   refs, delete only a local tag created by this attempt that does not exist
   remotely, recompute the version, rewrite the entry, and retry. Never
   force-push `master`.
4. **Publish:**
   - Once #20 is done, the tag push triggers the release workflow, which
     builds, tests and publishes the GitHub Release. Watch it, and report the
     Release URL or the failure.
   - Until then, create the Release manually with `gh release create` using
     the zip from `dist\` and only that version's CHANGELOG section as notes.
5. **On failure:** act on the actual remote state, not the error message.
   First stop any running release workflow for that tag, then check
   `gh release view v<version>`:
   - no release: fix the cause (code goes through the normal issue → PR flow),
     then re-run or re-tag;
   - leftover draft or partial assets: delete the draft, keep the tag if it
     still points at the release commit, re-run;
   - already published: never delete or overwrite it automatically; report
     and let the maintainer decide.
6. The Nexus upload stays manual (maintainer). Report the zip to use.

## Commits

- One logical change per commit; never mix bug fixes with restructuring.
- No `Co-Authored-By` trailers for Claude.
