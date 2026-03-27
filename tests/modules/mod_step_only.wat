;; Minimal module: exports only module_step (no init, no deinit).
;; Used to test basic load → start → run_step → unload lifecycle.
(module
  (memory (export "memory") 1)
  (func (export "module_step"))
)
