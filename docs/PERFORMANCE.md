# Permission evaluation costs

Read-only shell classification reuses fixed regular expressions for file
descriptors, printf formats and head/tail counts, plus an immutable flag table.
Repeated command admission no longer recompiles these expressions or rebuilds
the flag table. Pattern syntax, flag allowlists, filesystem confinement, protected
paths, redirect handling and interpreter rejection remain unchanged.

Dynamic rule globs remain evaluated from the actual rule value; there is no
unbounded global cache keyed by client input. Filesystem checks continue to run
on every admission so a changed path or symlink is not accepted using stale
authorization data. The shell policy tests exercise repeated accepted/rejected
commands and preserve the existing fail-closed cases.
