# Malibu Conformance Baseline

`versions.json` pins the external test inputs and reference browser used for
repeatable compatibility work.

Environment overrides:

- `MALIBU_WPT_ROOT`: WPT checkout, default `~/.wpt`
- `MALIBU_TEST262_ROOT`: Test262 checkout, default `~/.test262`
- `MALIBU_WPT_RUNNER`: `malibu_wpt` binary
- `MALIBU_TEST262_RUNNER`: `malibu_js` binary
- `MAXW`: worker process count
- `MEMCAP_MB`: memory cap per test process

Examples:

```sh
python3 tools/run_wpt.py dom/nodes
python3 tools/run_test262.py language/expressions
python3 tools/fetch_chrome_for_testing.py
```

The runners reject checkouts whose `HEAD` differs from the pinned commit. Update
the lock deliberately when adopting a newer standards baseline.
