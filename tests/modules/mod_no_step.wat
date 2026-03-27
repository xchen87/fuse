;; Module that deliberately omits the required "module_step" export.
;; FUSE must reject this with FUSE_ERR_MODULE_LOAD_FAILED.
(module
  (memory (export "memory") 1)
  (func (export "some_other_func"))
)
