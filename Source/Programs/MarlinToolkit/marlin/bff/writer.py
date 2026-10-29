"""Write FASTBuild's .bff syntax."""

from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

type Item = str | Path | StructRef
type Value = Item | bool | int | Sequence[Item]


@dataclass(frozen=True)
class StructRef:
    """A reference to a struct variable, ``.Name``, inside an array."""

    name: str


def escape(text: str) -> str:
    """Return *text* escaped for a single-quoted .bff string."""
    return text.replace("^", "^^").replace("'", "^'").replace("$", "^$")


def _literal(value: Item | bool | int) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, StructRef):
        return f".{value.name}"
    return f"'{escape(str(value))}'"


class BffWriter:
    """Accumulate .bff text, one function call at a time."""

    def __init__(self) -> None:
        self._lines: list[str] = []
        self._indent = 0

    def text(self) -> str:
        return "\n".join(self._lines) + "\n"

    def line(self, text: str = "") -> None:
        self._lines.append(("    " * self._indent + text) if text else "")

    def comment(self, text: str) -> None:
        for part in text.splitlines():
            self.line(f"// {part}")

    def properties(self, props: Mapping[str, Value | None]) -> None:
        """Write a property per item of *props*, skipping ``None`` values."""
        for key, value in props.items():
            if value is None:
                continue
            if isinstance(value, (str, Path, bool, int, StructRef)):
                self.line(f".{key} = {_literal(value)}")
            elif not value:
                self.line(f".{key} = {{}}")
            else:
                self.line(f".{key} =")
                self.line("{")
                self._indent += 1
                for index, item in enumerate(value):
                    self.line(_literal(item) + ("," if index < len(value) - 1 else ""))
                self._indent -= 1
                self.line("}")

    def struct(self, name: str, props: Mapping[str, Value | None]) -> None:
        self.line(f".{name} =")
        self.line("[")
        self._indent += 1
        self.properties(props)
        self._indent -= 1
        self.line("]")

    def function(
        self,
        function: str,
        name: str | None,
        props: Mapping[str, Value | None],
        structs: Iterable[tuple[str, Mapping[str, Value | None]]] = (),
    ) -> None:
        """Write a call of *function*, declaring *structs* before the *props* that refer to them."""
        self.line(f"{function}('{escape(name)}')" if name is not None else function)
        self.line("{")
        self._indent += 1
        for struct_name, struct_props in structs:
            self.struct(struct_name, struct_props)
        self.properties(props)
        self._indent -= 1
        self.line("}")
        self.line()
