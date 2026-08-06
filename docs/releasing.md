# Releasing 0.1.x

This checklist separates public CI gates from extended model compatibility
validation. Public CI uses synthetic fixtures and self-contained dependencies.

The project version is declared once in the top-level `project()` call and is
exported through the generated `<tekla/db1/version.hpp>` header and CMake package
version file. Do not tag or publish a release until a distribution license has
been selected and added to the repository.

## CI-enforced public gates

The GitHub Actions workflow enforces the strict matrix, installed consumers,
Linux sanitizer suite, and a 30-second bounded libFuzzer run over a generated
valid seed plus mutations. Run the same commands from a clean checkout at the
release commit before publishing.

### Strict build and deterministic contracts

~~~sh
cmake --preset ci
cmake --build --preset ci
ctest --preset ci
~~~

Acceptance: configuration and compilation succeed with warnings as errors; the
public-header-only external-adapter fixture compiles through
`TEKLA_DB1_EXTRA_ADAPTERS`; and all synthetic package, malformed-input,
semantic, geometry, deterministic-process, and byte-identical GLB tests pass.
GLB determinism is checked by two fresh processes over the same multi-object
public batch stream and compared by SHA-256.

### Sanitizers

On a supported Clang or GCC host:

~~~sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
~~~

Acceptance: every synthetic contract passes without an AddressSanitizer or
UndefinedBehaviorSanitizer finding. Linux runs this gate in public CI.

### Install, export, and consume

~~~sh
cmake --install build/ci --prefix build/stage
cmake -S tests/consumer -B build/consumer-release -DCMAKE_PREFIX_PATH="$PWD/build/stage"
cmake --build build/consumer-release --config Release
ctest --test-dir build/consumer-release -C Release --output-on-failure
cmake -S tests/consumer -B build/consumer-core-release -DCMAKE_PREFIX_PATH="$PWD/build/stage" -DTEKLA_DB1_CONSUMER_REQUIRE_GLTF=OFF
cmake --build build/consumer-core-release --config Release
ctest --test-dir build/consumer-core-release -C Release --output-on-failure
~~~

Acceptance: independent core-only and core-plus-glTF consumers configure from
the installed CMake package, compile against installed headers, observe version
`0.1.0`, link, and run. The GitHub Actions matrix runs the same path on Linux,
macOS, and Windows.

For a release tag, verify the installed documentation and version contract as
one cross-platform gate:

~~~sh
cmake \
  -DSOURCE_DIR="$PWD" \
  -DINSTALL_PREFIX="$PWD/build/stage" \
  -DEXPECTED_TAG=v0.1.0 \
  -DEXPECTED_SOURCE_REVISION="$(git rev-parse HEAD)" \
  -P cmake/VerifyReleaseArtifacts.cmake
~~~

Acceptance: `LICENSE`, `README.md`, `CHANGELOG.md`, and
`THIRD_PARTY_NOTICES.md` exist in source and under the installed documentation
directory with byte-identical contents; the installed header and CMake package
both report the exact version represented by the tag and the exact source
revision being published. GitHub Actions runs this gate automatically for every
`v*` tag and rejects an unlicensed release or mismatched source revision.

### OCCT-enabled install, consume, and worker smoke

On Ubuntu, install the OCCT Foundation, Modeling Data, and Modeling Algorithms
development packages first. Then validate that the exported static package
resolves its OCCT dependencies for an independent consumer and that the
installed supervised worker can start and exit cleanly on end-of-input:

~~~sh
sudo apt-get update
sudo apt-get install -y libocct-foundation-dev libocct-modeling-data-dev libocct-modeling-algorithms-dev
cmake --preset occt
cmake --build --preset occt
ctest --preset occt
cmake --install build/occt --prefix build/stage-occt
cmake -S tests/consumer -B build/consumer-occt -DCMAKE_PREFIX_PATH="$PWD/build/stage-occt"
cmake --build build/consumer-occt --config Release
ctest --test-dir build/consumer-occt -C Release --output-on-failure
build/stage-occt/bin/tekla-db1-occt-worker </dev/null
~~~

Acceptance: the OCCT-enabled SDK passes its own tests; the installed-package
consumer configures, links, and runs against the exported OCCT dependencies;
and the installed worker exits successfully without receiving a request. Public
CI enforces this path on Ubuntu.

### Bounded malformed-input fuzzing

CI builds the fuzzer only after a compile-and-link feature probe confirms that
the selected compiler ships a usable libFuzzer runtime. It drains bounded
identity, property, relation, definition-geometry, display-mesh, curve, and
diagnostic batches. Topology is deliberately disabled in this in-process gate.

~~~sh
cmake -S . -B build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DTEKLA_DB1_BUILD_TOOLS=OFF -DTEKLA_DB1_BUILD_GLTF=OFF -DTEKLA_DB1_BUILD_FUZZERS=ON -DTEKLA_DB1_ENABLE_SANITIZERS=ON
cmake --build build/fuzz
build/fuzz/fuzz/tekla_db1_container_fuzz build/fuzz/fuzz/corpus -dict=fuzz/container.dict -max_total_time=30 -rss_limit_mb=2048
~~~

Acceptance: the CI run completes without a crash, timeout, sanitizer finding,
or unbounded resident-set growth.

## Manual public release records

### Synthetic container-open performance record

~~~sh
cmake --preset benchmark
cmake --build --preset benchmark
ctest --preset benchmark -L benchmark
build/benchmark/benchmarks/tekla-db1-container-benchmark --rows 250000 --iterations 500
~~~

Acceptance: the CI smoke test succeeds and the full manual command emits one
JSON object containing payload size, row count, iteration count, wall time,
MiB/s, and peak resident-set bytes. This v0.1 benchmark measures opening an
already-owned byte buffer only. Preserve the manual JSON record and compare
only same-host runs; there is no enforced regression comparator yet. Stable
baseline comparison plus full semantic-processing and geometry benchmarks are
post-v0.1 work.

### Bounded malformed-input fuzzing

Extend the CI fuzz command to five minutes for release validation. The core must
be sanitizer-instrumented as well as the harness.

~~~sh
build/fuzz/fuzz/tekla_db1_container_fuzz build/fuzz/fuzz/corpus -dict=fuzz/container.dict -max_total_time=300 -rss_limit_mb=2048
~~~

Acceptance: the five-minute run completes without a crash, timeout, sanitizer
finding, or unbounded resident-set growth. Keep any minimized redistributable
input that finds a bug as a synthetic regression fixture.

## Extended compatibility gates

These gates use separately managed model datasets. Inputs and generated outputs
must not be copied into this repository.

Build the candidate once, then capture each approved model into an external
output directory. `MODEL` is the primary DB1 path; adjacent model assets
are discovered by `map_model_package`.

~~~sh
cmake --preset occt
cmake --build --preset occt
export MODEL="/validation/models/model/model.db1"
export VALIDATION_OUT="/validation/results/model"
mkdir -p "$VALIDATION_OUT"
build/occt/tools/tekla-db1-inspect "$MODEL" --tables > "$VALIDATION_OUT/tables.json"
build/occt/tools/tekla-db1-inspect "$MODEL" --semantics > "$VALIDATION_OUT/semantics.json"
build/occt/tools/tekla-db1-inspect "$MODEL" --component-definitions > "$VALIDATION_OUT/components.json"
build/occt/tools/tekla-db1-inspect "$MODEL" --geometry > "$VALIDATION_OUT/geometry.json"
build/occt/tools/tekla-db1-inventory "$MODEL" --topology-worker build/occt/tools/tekla-db1-occt-worker > "$VALIDATION_OUT/inventory.jsonl"
build/occt/tools/tekla-db1-glb "$MODEL" "$VALIDATION_OUT/preview.glb" --topology-worker build/occt/tools/tekla-db1-occt-worker
cmake -E sha256sum "$VALIDATION_OUT/tables.json" "$VALIDATION_OUT/semantics.json" "$VALIDATION_OUT/components.json" "$VALIDATION_OUT/geometry.json" "$VALIDATION_OUT/inventory.jsonl" "$VALIDATION_OUT/preview.glb" > "$VALIDATION_OUT/sha256.txt"
~~~

Repeat those commands through the compatibility-test runner and record the
runner revision and invocation with the release results.

1. Convert every approved steel, concrete, mixed, historical, and custom-shape
   test model with the release candidate's inventory and GLB tools.
2. Compare the GUID census, object-family counts, typed properties, relations,
   mesh bounds, triangle and vertex counts, surface area, and signed-volume
   magnitude with the previously approved compatibility baseline.
3. Run the OCCT topology worker over the approved object windows; no object may
   terminate the full run. Timeouts and crashes must remain object-scoped and
   appear as diagnostics.
4. Inspect same-camera renders for every test model. Every absent or extra
   display object must be classified as supported, intentionally filtered,
   explicitly unsupported, intentionally omitted, or a release blocker.
5. Record total wall time, peak memory, diagnostics by code, and unresolved
   profile/shape identifiers. Compare performance on the same host with the
   preceding candidate.

Acceptance: no unexplained primary-part regression from the approved baseline, no
new crash or hang, no silent diagnostic loss, and no unreviewed visual mismatch.
The public support matrix must describe every intentionally unsupported family.

## Final publication checks

- The worktree is clean and the release commit is signed or otherwise
  attributable under the project's chosen policy.
- `project(VERSION ...)`, `<tekla/db1/version.hpp>`, the CMake package version,
  and the intended tag agree.
- The support matrix and README distinguish public CI from extended
  compatibility coverage.
- A distribution license and third-party notices are present.
- Installed artifacts include the selected `LICENSE` and
  `THIRD_PARTY_NOTICES.md` under the package documentation directory.
- Public source archives contain no external model inputs or generated
  validation output.
- Tagging and publication happen only after all public and extended gates are
  recorded by the release owner.
