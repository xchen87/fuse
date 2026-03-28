# Skill: Implement a demo application with *FUSE*
### Description
Generate host control code and WASM modules, with policies written in JSON.
Each demo should be treated as a separate CMake project that links and uses FUSE. Each demo should meet the goal described by user.
Create dedicated folder under `/demos`.

### Required User Input
The user MUST provide a clear GOAL describing:
- The intended functionality of the system
- What the WASM module should do
- What the host application should control or enforce

### Instructions
1. **Goal Clarification**: If the GOAL is missing or vague, ask the user to clarify before proceeding.
  - Do NOT generate code until a concrete GOAL is provided.
  - Once GOAL is clear, proceed to **Design** phase.
2. **Design**: Reference `@architecture.md`, `@host_api_spec.md`, and `@module_api_spec.md` for context and pre-defined API spec.
  - Define the necessary API signatures and the execution flow.
  - Review and refine until it can meet the GOAL. Then proceed to **Coding** phase
3. **Coding**: delegate to @developer to write the C implementations. Coding should include:
  - Generate host control code, including *FUSE* management, *Module* loading and calling sequence, post processing after each *Module* step. Teardown after everything is done, if the task does not require continuously running.
  - Generate WASM module(s), that implements the core functionalities required for the task.
  - Generate JSON policy, with fields matches the *Module* policy defintion. In build system, use json to binary convertion script for converting the policy to execution ready binary. If such script doesn't exist, ask @scripter to create one.
  - Structure as separate CMake projects that links FUSE library.
  - Once done, proceed to **Security Audit**.
4. **Security Audit**
   - Delegate to @sentinel to perform a security audit.
   - Output: Must receive a "PASS" verdict from @sentinel, once received, go to **Final** phase.
   - If received a "FAIL" verdict, notify @developer on the details of the failure and ask for a code update.
5. **Final**
   - Git commit for all code implementations, audit trails.
