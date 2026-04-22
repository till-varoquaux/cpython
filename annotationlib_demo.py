import typing
import annotationlib

class Example:
    fwd: undefined
    a: undefined @ "metadata"
    b: typing.Annotated[undefined, "metadata"]
    c: undefined @ "m1" @ "m2"
    d: undefined1 | undefined2
    e: list[undefined]

print("Annotations evaluated with TYPE:\n")
annos = annotationlib.get_annotations(Example, format=annotationlib.Format.TYPE)

for key, value in annos.items():
    print(f"{key}: {repr(value)}")
    print(f"  origin: {typing.get_origin(value)}")
    print(f"  args:   {typing.get_args(value)}\n")
