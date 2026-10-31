# gc

`gc.collect()` runs a garbage collection sweep and returns the number of objects freed.

`gc.addroot(x)` adds an object to the collector rootset, keeping it and everything it references alive.

- `x`: object to keep from being collected

`gc.removeroot(x)` removes an object from the collector rootset so it may be collected again.

- `x`: object to release
