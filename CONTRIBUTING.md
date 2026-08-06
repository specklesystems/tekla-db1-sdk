# Contributing

## Source style

- C++20, two-space indentation, and the repository .clang-format.
- Follow the direct, data-oriented C++ style used by the native Speckle
  converters.
- Keep output-specific types outside tekla_db1::core.
- Prefer immutable spans, dense numeric identifiers, and bounded batches over
  per-record heap allocation.
- Errors and unsupported behavior are explicit and machine-readable.
- Floating-point contraction remains disabled for deterministic geometry.

Format changed C++ files before committing:

~~~sh
clang-format -i include/**/*.hpp src/*.cpp adapters/**/*.cpp tests/*.cpp
~~~

## Tests

Tests exercise public interfaces. Add one behavioral test and its minimum
implementation at a time. The default test suite must not require a Tekla
installation or external model files.

Extended compatibility and performance runs are release gates, but their input
datasets and generated output remain outside this repository.

## Dependencies

The core should remain dependency-light. New dependencies require a written
reason covering:

- runtime and binary-size cost,
- allocation and data-copy behavior,
- platform and static-link support,
- license compatibility,
- whether the dependency belongs in the core or an adapter.

## Commit hygiene

Run the strict build and complete test suite before committing:

~~~sh
cmake --preset ci
cmake --build --preset ci
ctest --preset ci
~~~
