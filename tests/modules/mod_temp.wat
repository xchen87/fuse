;; Module that calls temp_get_reading() from module_step.
;; Policy must grant FUSE_CAP_TEMP_SENSOR for this to succeed.
(module
  (import "env" "temp_get_reading" (func $temp_get_reading (result f32)))
  (memory (export "memory") 1)
  (func (export "module_step")
    (drop (call $temp_get_reading))
  )
)
