# Literate AI agent entry point

Read and follow `SKILL.md` before changing this framework or any project that uses its
canonical taxonomy. Treat that file as the provider-neutral onboarding authority.

Before creating a linked Git worktree, run
`litai project worktree location . --json` and use the returned repository-owned
`worktree_root` with a short, portable name. Keep that `.worktrees/` directory
ignored. Do not create sibling worktrees or use external temporary directories
as placement fallbacks.
