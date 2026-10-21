# No overfitting to a single sample

Every tool, template, document and pipeline in this project must work for
**arbitrary new instances without code changes**. A feature that only works
for the character, file, path or case it was developed on is a bug, not a
feature.

## Rules

1. **No instance may become an assumption.** Never hardcode a concrete
   character name, asset path, palette, size, voice, or example image into
   shared code or guides. If a value belongs to one instance, it must be
   resolved from that instance's own data (its package, its manifest, its
   settings) or passed as a parameter.
2. **References and inputs resolve per-instance.** When a pipeline needs "the
   character's image / config / material", resolve it from the target
   instance's own directory — never from a global location, a development
   leftover, or another instance's files. Cross-instance reuse of inputs is
   contamination.
3. **Two instances before "generic".** A tool or guide claiming generality
   must have been exercised on at least two different instances (e.g. two
   characters with different shapes/species), or be explicitly marked as
   single-case.
4. **Docs must not normalize one example.** Examples may illustrate, but the
   normative text stays instance-neutral ("the package's own body image", not
   "the parrot's image").
5. **Self-check prompt beats silent guessing.** When a tool auto-resolves an
   instance-specific input, it must print what it picked and let the operator
   override it (`--ref-image`-style flag), so a stale or wrong file is caught
   before money is spent.

## Why

We ship creators' tools used by many characters and devices. Anything tuned
to the first character we tested (a parrot) breaks silently on the next (a
cat). Overfitting is invisible in the happy path — so it must be prevented at
design time, not caught in production.
