# gc

`gc.collect()` performs a collection sweep and returns the number of objects freed.

`gc.addroot(x)` adds object `x` to the GC rootset, preventing it and everything it references from being collected.

`gc.removeroot(x)` removes `x` from the rootset.
