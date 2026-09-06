# Repository workflow

- Make changes and commits on `devchatgpt` unless the user explicitly requests a different branch.
- Treat `main` as the published branch. When the user requests promotion, validate `devchatgpt` and push that tested commit to `main`; leave the working checkout on `devchatgpt`.
- Preserve existing uncommitted work and inspect it before including it in a requested build promotion.
