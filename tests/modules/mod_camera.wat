;; Module that calls camera_last_frame() from module_step.
;; Allocates 256 bytes of linear memory and passes offset 0 with len=256.
;; Policy must grant FUSE_CAP_CAMERA for this to succeed.
(module
  (import "env" "camera_last_frame" (func $camera_last_frame (param i32 i32) (result i64)))
  (memory (export "memory") 1)
  (func (export "module_step")
    ;; buf = offset 0, max_len = 256
    (drop (call $camera_last_frame (i32.const 0) (i32.const 256)))
  )
)
