;; Module that calls camera_last_frame() with max_len == 0.
;; The bridge must reject this (null/zero-length buffer check) and trap the module
;; before calling the HAL callback or performing memory validation.
(module
  (import "env" "camera_last_frame" (func $camera_last_frame (param i32 i32) (result i64)))
  (memory (export "memory") 1)
  (func (export "module_step")
    ;; buf = offset 0, max_len = 0 → must be rejected
    (drop (call $camera_last_frame (i32.const 0) (i32.const 0)))
  )
)
