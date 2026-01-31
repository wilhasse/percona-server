# Repository Guidelines

## Project Structure & Module Organization
- `sql/`, `storage/`, `plugin/`, `mysys/`, `vio/`: core server and storage engine code.
- `include/`: public and internal headers.
- `client/`, `libmysql/`, `router/`: client tools and libraries.
- `mysql-test/` and `unittest/`: test suites.
- `cmake/`, `scripts/`, `packaging/`: build and release helpers.
- `doc/` and `Docs/`: documentation sources.

## Build, Test, and Development Commands
- `git submodule update --init` fetches third-party submodules.
- Out-of-tree build is recommended. Example:
```bash
mkdir -p ../BUILD-my-percona-server
cmake -S . -B ../BUILD-my-percona-server -DCMAKE_BUILD_TYPE=Debug -DDOWNLOAD_BOOST=ON -DWITH_BOOST=/path/to/boost
cmake --build ../BUILD-my-percona-server -j 16
```
- To debug `mysqld-debug`, run from the build tree:
`cd ../BUILD-my-percona-server/mysql-test && ./mtr --debug-server --manual-debug main.1st`

## Coding Style & Naming Conventions
- C/C++ style follows existing file conventions; avoid sweeping reformatting.
- Run `git clang-format` on changed files before pushing.
- Branch names: `PS-1234-8.0-short_description`.
- Commit subjects typically follow `PS-1234 [8.0]: Short summary`; include the Jira URL and details in the body.

## Testing Guidelines
- Use `mysql-test` (MTR) for integration tests and `unittest/` for unit tests.
- Example: `cd ../BUILD-my-percona-server/mysql-test && ./mtr --suite=main main.1st`.
- Add or extend tests for fixes/features; keep tests close to the affected area.

## Commit & Pull Request Guidelines
- A Jira issue is required before coding; reference it in branch, commits, and PRs.
- Squash to one or a small number of logical commits.
- PR title example: `PS-1234 (8.0) Short summary`; description starts with the Jira link and a brief summary.
- Expect CI checks and peer review; address reviewer feedback promptly.

## Security & Configuration Tips
- Do not open GitHub issues for security reports; email `secalert_us@oracle.com` per `SECURITY.md`.
- Builds often require Boost; set `-DDOWNLOAD_BOOST=ON` or point `-DWITH_BOOST=/path`.
