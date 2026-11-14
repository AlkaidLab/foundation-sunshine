# Configuration upgrade fixtures

Each directory is one released format:

- 2026.1031/ - the latest stable release when the test was written
- 2026.1112/ - the latest prerelease when the test was written

sunshine.conf carries every key the release's src/config.cpp parses, each
with a value that release accepts. apps.json is the release's bundled
application list plus one user entry that uses every per-application field.
sunshine_state.json is the paired-client state that release writes.

The fixtures feed tests/unit/test_config_upgrade.cpp,
tests/unit/test_default_apps.cpp and tests/unit/test_pairing_pending.cpp.
Every key in a fixture is a promise: adding one that the release did not parse
would make the upgrade test claim more than the release supported.
