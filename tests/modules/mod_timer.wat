;; Module that calls timer_get_timestamp() from module_step.
;; Policy must grant FUSE_CAP_TIMER for this to succeed.
(module
  (import "env" "timer_get_timestamp" (func $timer_get_timestamp (result i64)))
  (memory (export "memory") 1)
  (func (export "module_step")
    (drop (call $timer_get_timestamp))
  )
)
