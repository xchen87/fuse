;; Module that calls camera_last_frame() with an out-of-bounds buffer pointer.
;; Linear memory is 1 page (64 KiB = 65536 bytes).
;; We pass offset 65280 (0xFF00) with max_len=512, so the range
;; [65280, 65792) extends 256 bytes past the end of the page.
;; WAMR's '*' signature converts the wasm offset to a native pointer;
;; fuse_native_camera_last_frame must then reject it via
;; wasm_runtime_validate_native_addr.
(module
  (import "env" "camera_last_frame" (func $camera_last_frame (param i32 i32) (result i64)))
  (memory (export "memory") 1)
  (func (export "module_step")
    ;; offset = 65280, max_len = 512 → extends 256 bytes past end of page
    (drop (call $camera_last_frame (i32.const 65280) (i32.const 512)))
  )
)
