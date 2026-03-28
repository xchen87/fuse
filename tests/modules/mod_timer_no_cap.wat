;; Module that calls timer_get_timestamp() from module_step.
;; Used to test capability-denied path: policy must NOT grant FUSE_CAP_TIMER.
;; The bridge must trap the module when the capability bit is absent.
(module
  (import "env" "timer_get_timestamp" (func $timer_get_timestamp (result i64)))
  (memory (export "memory") 1)
  (func (export "module_step")
    (drop (call $timer_get_timestamp))
  )
)
