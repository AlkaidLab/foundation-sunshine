"""Pluggable voice-changer backends.

Each backend is a small module exposing a `create(opts)` factory that returns
an object implementing the `Backend` protocol from `base.py`. New backends
register themselves in the `REGISTRY` mapping below.

Adding a backend (recipe):

  1. create `backends/<name>.py`
  2. implement a class with `name`, `__init__(self, **opts)`, `process(...)`,
     and (optionally) `reset()` / `warmup()`
  3. add `"<name>": "<name>:create"` to REGISTRY
  4. document required deps in requirements-<name>.txt
"""

from __future__ import annotations

from importlib import import_module
from typing import Any

from .base import Backend  # re-export

# name -> "module:factory_callable" (callable inside that module)
REGISTRY: dict[str, str] = {
    "identity": "identity:create",
    "rvc": "rvc:create",
}


def load(name: str, **opts: Any) -> Backend:
    """Instantiate a backend by registry name. Raises ValueError if unknown."""
    if name not in REGISTRY:
        avail = ", ".join(sorted(REGISTRY))
        raise ValueError(f"unknown backend '{name}' (available: {avail})")
    target = REGISTRY[name]
    mod_name, _, factory = target.partition(":")
    mod = import_module(f".{mod_name}", package=__name__)
    create = getattr(mod, factory or "create")
    return create(**opts)


__all__ = ["Backend", "REGISTRY", "load"]
