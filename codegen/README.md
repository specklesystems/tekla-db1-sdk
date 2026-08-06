# Schema generation

`generate_schemas.py` converts reviewed neutral schema manifests into the
compact, checked-in C++ tables under `src/generated`.

The generated output contains the structural contract needed at runtime:
format and table identities, tuple widths, descriptor vectors, and checked
native scalar layouts.

Generation is a maintainer operation, not a build dependency. A source archive
and installed package compile from the checked-in C++ file without Python. Pass
manifest paths explicitly when regenerating:

~~~sh
python3 codegen/generate_schemas.py \
  --output src/generated/schemas.cpp \
  /path/to/reviewed/schema-1.json /path/to/reviewed/schema-2.json
~~~
