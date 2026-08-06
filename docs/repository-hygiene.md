# Repository hygiene

Keep the repository reproducible, reviewable, and free of machine-specific
inputs and generated validation output.

## Never commit

- model databases or model folders,
- generated conversion output,
- screenshots and visual-comparison output,
- executables, shared libraries, SDK package caches, or build trees,
- machine-specific paths, credentials, tokens, or license material.

The .gitignore blocks common binary and model extensions, but review remains
mandatory because ignore rules are not a security boundary.

## Specifications and tests

Format-specific behavior belongs in reviewed code or versioned declarative
specifications. New behavior should carry explicit supported-format conditions
and focused public-interface tests.

The SDK must build and test from a standalone checkout. Optional external test
datasets may be used for release validation but are never build dependencies.

## Validation split

- Public SDK CI: synthetic fixtures, malformed-input tests, API behavior,
  determinism, fuzzing, install/consume checks.
- Extended validation: supported-version coverage, visual regression checks,
  and performance benchmarks over separately managed datasets.

Only aggregate release metrics and minimal reproducible regression fixtures are
committed.
