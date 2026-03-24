# Skill: Implement a feature or an API
**Goal:** Implement with MISRA-C compliance, make sure all implementations or code changes are fully tested, verified, and security checked

## Workflow Steps:
1. **Design**: Reference `@architecture.md`, `@host_api_spec.md`, and `@module_api_spec.md` for context and pre-defined API spec.
   - Define the necessary API signatures and the execution flow, with security and safety as first priority.
2. **Coding**:
   - Delegate to @developer to write the C implementations, including adding necessary .h and .c files.
   - After codeing is done, go to **Testing** phase for verification.
3. **Testing**:
   - Delegate to @validator to write googletest based test cases. Ensure implementation correctness.
   - Reach 100% code coverage on code addition and modifications.
   - If any test failed, first check if the test case is written correctly.
   - Notify @developer about the details on the failure and ask for a code update.
   - After all testings is good, go to **Security Audit** phase.
4. **Security Audit**
   - Delegate to @sentinel to perform a security audit.
   - Output: Must receive a "PASS" verdict from @sentinel, once received, go to **Final** phase.
   - If received a "FAIL" verdict, notify @developer on the details of the failure and ask for a code update.
5. **Final**
   - Git commit for all code implementations, test cases, and audit trails.
