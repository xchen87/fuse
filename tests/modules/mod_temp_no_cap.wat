;; Module that calls temp_get_reading() from module_step.
;; Used to test capability-denied path: policy must NOT grant FUSE_CAP_TEMP_SENSOR.
;; The bridge must trap the module when the capability bit is absent.
(module
  (import "env" "temp_get_reading" (func $temp_get_reading (result f32)))
  (memory (export "memory") 1)
  (func (export "module_step")
    (drop (call $temp_get_reading))
  )
)
