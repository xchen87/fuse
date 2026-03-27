;; Module that calls module_log_event() with a message longer than FUSE_LOG_MSG_MAX (128).
;; Used to test truncation behaviour in fuse_native_module_log_event.
;; Message is 200 'A' characters placed at offset 0.
(module
  (import "env" "module_log_event" (func $module_log_event (param i32 i32 i32)))
  (memory (export "memory") 1)
  (data (i32.const 0)
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
  )
  (func (export "module_step")
    ;; ptr=0, len=200, level=0 (DEBUG) — should be truncated to 127 chars
    (call $module_log_event (i32.const 0) (i32.const 200) (i32.const 0))
  )
)
