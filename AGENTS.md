# PureMCL

- Application code is ISO C17. Use Win32 wide APIs, UTF-8 for JSON, and no embedded web UI.
- Keep native UI separate from the launch pipeline. Worker callbacks may post messages; they must not touch controls.
- Treat metadata and archives as untrusted. Verify downloaded files before promotion. Keep account tokens out of logs and persist them only through DPAPI.
- Use MSVC x64, CMake and Ninja. Run CTest after changes to launch logic or file handling.

