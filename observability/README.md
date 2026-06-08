# MattSQL Observability

This directory contains the no-cloud Grafana stack for MattSQL performance
metrics.

## Start The Stack

```sh
docker compose -f observability/docker-compose.yml up -d
```

Default local endpoints:

- Grafana: `http://localhost:3000`
- Prometheus: `http://localhost:9090`
- Pushgateway: `http://localhost:9091`

Default Grafana login:

- user: `admin`
- password: `mattsql`

Set `GRAFANA_ADMIN_PASSWORD` before first startup to use a different password:

```sh
GRAFANA_ADMIN_PASSWORD='replace-me' \
  docker compose -f observability/docker-compose.yml up -d
```

Grafana provisions the Prometheus data source and MattSQL performance dashboard
automatically.

## Local Git Automation

Install the tracked git hooks:

```sh
scripts/install-git-hooks.sh
```

This sets the repo-local `core.hooksPath` to `.githooks`.

Hook behavior:

- `post-commit`: runs the profile benchmark suite, writes the static report,
  writes Prometheus metrics, and publishes them to Pushgateway if available.
  Publishing is best-effort so commits are not blocked when Grafana is down.
- `pre-push`: runs the same benchmark/report/publish path and fails the push
  when the current benchmark results exceed the baseline limits.

Useful overrides:

```sh
MATTSQL_SKIP_PERF_HOOKS=1 git commit ...
MATTSQL_SKIP_PERF_HOOKS=1 git push
MATTSQL_PERF_PUBLISH=0 git push
MATTSQL_PERF_CHECK_BASELINE=0 git push
MATTSQL_PERF_REQUIRE_PUBLISH=1 git push
MATTSQL_PERF_ENVIRONMENT=lab git push
```

By default, hooks publish to `http://localhost:9091`. Override that with
`PUSHGATEWAY_URL` when the Pushgateway is running elsewhere.

## Publish Local Benchmark Metrics

```sh
cmake --preset profile
cmake --build --preset profile --target mattsql_benchmarks
./build/profile/mattsql_benchmarks --json > build/profile/benchmark-results.json
python3 benchmarks/prometheus_publish.py \
  --current-json build/profile/benchmark-results.json \
  --baseline benchmarks/baseline.tsv \
  --output-text build/profile/performance-report/prometheus-metrics.prom \
  --pushgateway-url http://localhost:9091 \
  --publish
```

## GitHub Actions Ingestion

The `Performance` workflow can publish to this stack automatically when
GitHub can reach the Pushgateway URL.

GitHub Actions is optional for the no-cloud setup. Use local hooks when you
want only local commit and push automation.

Set these repository secrets:

- `PUSHGATEWAY_URL`: public HTTPS URL for a reverse proxy in front of
  Pushgateway, or a private URL reachable from a self-hosted GitHub runner.
- `PUSHGATEWAY_USERNAME`: optional basic-auth username.
- `PUSHGATEWAY_PASSWORD`: optional basic-auth password.
- `PUSHGATEWAY_HEADERS`: optional comma-separated headers, such as
  `Authorization=Bearer%20...`.

Do not expose the raw Pushgateway directly to the public internet. Put it
behind HTTPS and authentication, or run the GitHub workflow on a self-hosted
runner on the same network and use a private `PUSHGATEWAY_URL`.
