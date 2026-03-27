;; Module that calls module_log_event() from module_step.
;; Writes a short message from linear memory offset 0.
;; Policy must grant FUSE_CAP_LOG for this to succeed.
(module
  (import "env" "module_log_event" (func $module_log_event (param i32 i32 i32)))
  (memory (export "memory") 1)
  ;; Store "hello" at offset 0 via data segment
  (data (i32.const 0) "hello")
  (func (export "module_step")
    ;; ptr=0, len=5, level=1 (INFO)
    (call $module_log_event (i32.const 0) (i32.const 5) (i32.const 1))
  )
)
