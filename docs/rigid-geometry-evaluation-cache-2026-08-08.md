# Rigid geometry-evaluation cache benchmark

This benchmark compares the translation-only completed-result cache at
`9106fd3` with explicit-frame rigid canonicalization. Both variants used the
same direct OCCT inventory command with `TEKLA_DB1_OCCT_CACHE_PROFILE=1` in the
same machine session.

| Corpus | Variant | Hits | Successful misses / inserts | Retained bytes | Wall time |
| --- | ---: | ---: | ---: | ---: | ---: |
| Kapali | translation | 963 | 600 | 9,902,744 | 102.23 s |
| Kapali | rigid | 1,173 | 390 | 7,523,276 | 78.90 s |
| Office | translation | 657 | 826 | 3,621,900 | 68.25 s |
| Office | rigid | 776 | 707 | 3,156,936 | 53.45 s |

Rigid canonicalization reduced evaluated Kapali requests by 35.0% and wall
time by 22.8%. It reduced evaluated Office requests by 14.4% and wall time by
21.7%. Retained bytes are the final monotonically increasing cache total from
the profile stream. Current profiling emits explicit `hit`, `miss`, `bypass`,
`insert`, and `not_retained` outcomes, so failed evaluations and precision
bypasses can be distinguished from successful retained misses in subsequent
runs.

The release-gate cache-on/cache-off baselines use the same corpus commands with
`TEKLA_DB1_DISABLE_OCCT_CACHE=1` for the disabled variant:

| Corpus | Cache on | Cache off |
| --- | ---: | ---: |
| Kapali | 75.09 s | 166.97 s |
| Office | 57.91 s | 189.59 s |

The final release candidate was then rerun with the explicit-outcome profiler:

| Corpus | Hits | Misses | Inserts | Bypasses | Not retained | Retained bytes | Wall time |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Kapali | 1,173 | 391 | 390 | 0 | 0 | 7,523,276 | 56.98 s |
| Office | 776 | 709 | 707 | 0 | 0 | 3,156,936 | 49.64 s |

The one Kapali and two Office miss/insert differences are failed evaluations;
they remain retryable and never enter the completed-result cache.

## Equivalence

Record identity, ordering, mesh/curve/diagnostic counts, and every mesh vertex
and triangle count were unchanged:

| Corpus | Meshes | Curves | Diagnostics | Topology-count differences |
| --- | ---: | ---: | ---: | ---: |
| Kapali | 4,155 | 18 | 128 | 0 |
| Office | 13,004 | 34,948 | 2,991 | 0 |

Rotated cache hits necessarily transform a previously evaluated float mesh
instead of asking OCCT to tessellate again at the new model coordinates. The
maximum observed world-bound difference was 0.001953125 mm for Kapali and
0.0078125 mm for Office. Inventory metrics recomputed from those display
floats differed by at most 0.02022% in volume and 0.001116% in area for Kapali,
and 0.02201% in volume and 0.001978% in area for Office. The cache contract
preserves OCCT's exact surface-area and volume fields without recomputation.

The rigid path accepts only finite, orthonormal, right-handed frames. Invalid
frames and requests containing boxes that cannot be represented in the local
frame fall back to the existing translation-only cache. Canonical-byte
equality remains the final match condition.

## Early-cache experiment

This slice reuses completed evaluations across explicit rigid placement, but
the current reader still constructs world meshes, cutters, and the OCCT
request before lookup. A separate fitting-only `LocalFeaturePlan` experiment
avoided 54 repeated constructions on Kapali and 249 on Office while preserving
byte-identical output, but wall time stayed within noise and slightly regressed
(74.66 vs 73.94 seconds; 50.50 vs 50.23 seconds). It is intentionally not part
of this release. Boolean and chamfer planning remain a broader redesign because
relevance, tangent extension, cutter ordering, and chamfer geometry currently
depend on world meshes.
