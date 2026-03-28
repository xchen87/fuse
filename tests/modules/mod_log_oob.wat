;; Module that calls module_log_event() with a buffer range that far exceeds
;; the module's linear memory. offset=0, len=0x7FFFFFFF.
;; fuse_native_module_log_event must reject it via wasm_runtime_validate_native_addr
;; and transition the module to TRAPPED state.
(module
  (import "env" "module_log_event" (func $module_log_event (param i32 i32 i32)))
  (memory (export "memory") 1)
  (func (export "module_step")
    ;; ptr=0, len=0x7FFFFFFF → range far exceeds module memory; level=0 (DEBUG)
    (call $module_log_event (i32.const 0) (i32.const 0x7FFFFFFF) (i32.const 0))
  )
)
