# mm_hp

- A performant, complete implementation of C++26 hazard pointers ([saferecl.hp]) for Linux x86_64.
- Intended as guidance for standard library implementers on ABI robustness and as a performance reference point.
- For readability, internal names deliberately avoid using the standard library `__x` reserved style.
- Licensed so that standard library implementers can take all or part of this code — see [LICENSES](LICENSES).
- If an implementation needs different terms, additional licenses may be considered.
- Bug reports and written suggestions are welcome; patches and code cannot be accepted in order to keep the copyright in one place to facilitate adding licenses.
- No guarantee of support of any kind.

## Performance results

```
Operation                                    1-thread ns/op   8-thread scalability
load and dereference, unprotected            0.233            7.9x
+ hazard pointer protection                  0.414 (Δ 0.181)  7.8x
+ hazard pointer ctor/dtor per iteration     1.728 (Δ 1.495)  7.9x
```

- 10,000 pre-existing hazard pointers make no measurable difference.
- Measured with [hp_bench](https://github.com/magedm/hp_bench).
- Extracted from [mm_hp-2026-09-03-141532.txt](https://github.com/magedm/hp_bench/blob/main/results/mm_hp-2026-09-03-141532.txt).
