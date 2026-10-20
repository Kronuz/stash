# The stash interleaving model

An exhaustive explorer of `add`, the walk and `clean` at one step per atomic access, used to prove
the announce board, the windowed far-insert epochs and fail-fast allocation (ARCHITECTURE,
"Inserts in flight"). `stash_model.py` is the model; `stash_model_review.py` adds the code review's
memory-order mutants and the epoch-read race. The `run_*.sh` scripts reproduce the recorded runs:
each configuration reports no violation, and each mutant reports the violation it targets
(first_valid passing an in-flight key, a stranded value, a use-after-free by clean, or a HOLE).
Run them under a `quiet run` hold; the larger configurations take minutes and a few GB.
