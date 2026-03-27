;; Module that calls camera_last_frame() with an out-of-bounds max_len.
;; Linear memory is 1 page (64 KiB = 65536 bytes).
;; We pass offset 0 but max_len=0x7FFFFFFF so the range
;; [0, 2147483647) far exceeds any possible module memory allocation
;; (even including WAMR heap padding in AOT mode).
;; fuse_native_camera_last_frame must reject it via
;; wasm_runtime_validate_native_addr.
(module
  (import "env" "camera_last_frame" (func $camera_last_frame (param i32 i32) (result i64)))
  (memory (export "memory") 1)
  (func (export "module_step")
    ;; offset=0, max_len=0x7FFFFFFF → range far exceeds module memory in all WAMR modes
    (drop (call $camera_last_frame (i32.const 0) (i32.const 0x7FFFFFFF)))
  )
)
