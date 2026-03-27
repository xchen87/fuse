;; Module with all three lifecycle exports: module_init, module_step, module_deinit.
;; Used to verify init is called once and deinit is called on unload.
(module
  (memory (export "memory") 1)
  (func (export "module_init"))
  (func (export "module_step"))
  (func (export "module_deinit"))
)
