# Third-party notices

`tekla-db1-sdk` does not vendor the dependencies below. They are discovered in
the build environment and linked into binaries selected by the integrator.
Redistributors remain responsible for satisfying the terms of the exact
dependency versions they ship.

## zlib

The core SDK requires zlib.

- Project: zlib
- Copyright: 1995-2026 Jean-loup Gailly and Mark Adler
- License: zlib License
- Upstream license: <https://www.zlib.net/zlib_license.html>

The zlib license notice must not be removed or altered from a zlib source
distribution. The dependency is not copied into this repository.

## Open CASCADE Technology

The optional topology evaluator can link to Open CASCADE Technology (OCCT).

- Project: Open CASCADE Technology
- Copyright: 1999-2026 OPEN CASCADE S.A.S.
- License: GNU Lesser General Public License version 2.1 with the Open CASCADE
  Exception
- Upstream license and redistribution guidance:
  <https://dev.opencascade.org/doc/overview/html/index.html>

OCCT is not copied into this repository or into the SDK's source archives.
Anyone distributing an OCCT-enabled binary must provide the notices, license
materials, corresponding-source/relinking facilities, and other provisions
required by the OCCT version and distribution method they use.
