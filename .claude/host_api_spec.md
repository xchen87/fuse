# FUSE Host API Specification:
***Host* use these APIs to manage *FUSE* and *Module***

## *FUSE* management APIs:

### `stat_enum fuse_init(module_memory_ptr, module_memory_size, log_memory_ptr, log_memory_size)`
- Description: Initialize the fuse runtime operation, then start fuse operation with no modules at beginning
- Parameters:
    - `module_memory_ptr`: ptr to memory that FUSE can assign to modules
    - `module_memory_size`: size of total memory FUSE can assign to modules
    - `log_memory_ptr`: ptr to FUSE security log
    - `log_memory_size`: size of total memory FUSE can use for security logging
- Returns: status enum, `SUCCESS` if all good, proper error code otherwise

### `stat_enum fuse_stop()`
- Description: Stop the fuse runtime operation, pause all loaded modules
- Return: `SUCCESS` if all good, proper error code otherwise

### `stat_enum fuse_restart()`
- Description: Restart the fuse runtime operation, resume all modules, use after stop.
- Return: `SUCCESS` if all good, proper error code otherwise

## *Module* management APIs:

### `stat_enum fuse_module_load(module_ptr, module_size, module_policy_ptr)`
- Description: Load a wasm module to fuse runtime, put the module in executable state
- Parameters:
    - `module_ptr`: ptr to memory that contains module wasm binary
    - `module_size`: module wasm binary size
    - `module_policy_ptr`: ptr to module policy
- Return: `SUCCESS` if all good, proper error code otherwise

### `stat_enum fuse_module_pause(module_id)`
- Description: Pause a wasm module, put the module in pause_by_host state, if module is currently executing, pause after quota
- Parameters:
    - `module_id`: which module this operation is for
- Return: `SUCCESS` if all good, proper error code otherwise

### `stat_enum fuse_module_start(module_id)`
- Description: Start or resume a wasm module, put the module in executable state
- Parameters:
    - `module_id`: which module this operation is for
- Return: `SUCCESS` if all good, proper error code otherwise

### `stat_enum fuse_module_stat(module_id, module_state& state)`
- Description: Query state of a module
- Parameters:
    - `module_id`: which module this operation is for
    - `state`: FUSE pass the module state by reference
- Return: `SUCCESS` if all good, proper error code otherwise

### `stat_enum fuse_module_unload(module_id)`
- Description: Unload a wasm module. Stop all operations, reclaim all resources associated with module
- Parameters:
    - `module_id`: which module this operation is for
- Return: `SUCCESS` if all good, proper error code otherwise
