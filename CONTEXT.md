# Domain language

**Persisted object** — An addressable record identity present in a model
database. Persistence alone does not imply that the object should appear as an
independent model element in an output format.

**Model element** — An independently meaningful object that may be exposed to
an output adapter with identity, properties, display geometry, or semantic
relationships.

**Evaluation feature** — A persisted operand used to derive another object's
final geometry. Boolean parts and cut planes are evaluation features: they
remain available to raw processing and geometry evaluation but are not model
elements or semantic relationship endpoints.

**Semantic relationship** — An output-neutral relationship between model
elements. The supported relationships are parent-to-child subelements,
member-to-assembly membership, directed component connectivity, and
reinforcement-to-host attachment.

**Raw relationship** — A persisted database relationship retained for
diagnostics or specialized consumers without asserting model-level meaning.

