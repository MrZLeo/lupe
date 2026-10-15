# Homebrew packaging

Lupe is distributed from the `mrzleo/homebrew-tap` tap. The Formula installs
the versioned, checksummed macOS universal binary produced by the tagged
release workflow.

Generate a Formula for an existing stable release with:

```sh
ruby packaging/homebrew/generate-formula.rb \
  v0.3.0 \
  packaging/homebrew/testdata/v0.3.0-SHA256SUMS \
  /tmp/lupe.rb
```

The release workflow performs the same operation with the checksums from the
new release, uploads `lupe.rb` as a release asset, and updates the tap when the
`HOMEBREW_TAP_TOKEN` repository secret is configured. Use a fine-grained token
with Contents read/write access to `mrzleo/homebrew-tap`.

The tap repository is already initialized at `mrzleo/homebrew-tap`. The release
workflow updates `Formula/lupe.rb`, so no bootstrap step is required.
Formula URLs use the renamed GitHub repository, `mrzleo/lupe`.
The tap migration replaces `Formula/loupe.rb` with `Formula/lupe.rb` and records
`"loupe": "lupe"` in the tap's `formula_renames.json` for existing installations.
Historical release archives and their embedded executable names remain unchanged.

To validate a Formula in a local tap:

```sh
brew tap-new --no-git mrzleo/lupe-test
cp /tmp/lupe.rb "$(brew --repository mrzleo/lupe-test)/Formula/lupe.rb"
brew style --formula mrzleo/lupe-test/lupe
brew audit --strict --formula mrzleo/lupe-test/lupe
brew install mrzleo/lupe-test/lupe
brew test mrzleo/lupe-test/lupe
```

The checked-in `v0.3.0` checksum fixture is intentionally immutable. It keeps
the generator and both supported macOS architectures testable without depending
on an unreleased version. The generator recognizes historical `loupe` archives
and installs their executable as `lupe`, while checking the original version
output.
