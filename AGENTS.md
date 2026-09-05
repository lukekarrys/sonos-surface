# Repository Guidelines

## Project Structure & Module Organization

This workspace is an initial scaffold containing an empty `README.md`. No source code, tests, assets, or dependency manifests exist yet. Use `README.md` for the project overview and setup instructions; use `AGENTS.md` for contributor guidance.

When implementation begins, choose a layout appropriate to the stack and document it here. Suggested directories are `src/` for application code, `tests/` for tests, and `assets/` for static resources. These are proposed conventions, not existing paths.

## Build, Test, and Development Commands

No build, test, or local development commands are currently configured. Do not assume commands such as `npm test` or `npm run build` are available.

When adding tooling, provide reproducible commands for installation, local development, testing, and production builds in `README.md`. Commit the relevant manifests and lockfiles alongside that documentation.

## Coding Style & Naming Conventions

No programming language, formatter, or linter has been selected. Match the conventions of the chosen stack and configure formatting and linting when introducing source code. Specify indentation in the formatter configuration rather than relying on editor defaults.

For Markdown, use descriptive headings, fenced code blocks for commands, and repository-relative paths. Keep filenames descriptive and naming consistent within each module.

## Testing Guidelines

No test framework or coverage threshold exists yet. Introduce tests with executable behavior, document the test command, and select a consistent naming pattern supported by the framework. Cover new behavior and regression cases for bug fixes. Report what was verified and identify checks that could not run.

## Commit & Pull Request Guidelines

This checkout has no Git metadata, so existing commit conventions cannot be verified. Until conventions are established, use concise, imperative subjects such as `Add development setup instructions` and keep commits focused.

Pull requests should explain the change, its purpose, and validation performed. Link relevant issues and include screenshots when changing a visible interface. Update this guide as the actual structure and tooling take shape.
