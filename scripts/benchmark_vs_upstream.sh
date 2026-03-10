#!/usr/bin/env bash
# Compare a local fork build against an upstream release or any two explicit binaries.
set -euo pipefail

BUILD=false
CLEAN=false
FLUSH_DB=true
UPSTREAM_VERSION="30.1"
DURATION="30s"
PORT="12108"
WORK_DIR="${HOME}/.cache/typesense/benchmark"
PROFILE="standard"
SERVER_ARGS=()
FORK_BINARY_OVERRIDE=""
FORK_LABEL_OVERRIDE=""
BASELINE_BINARY_OVERRIDE=""
BASELINE_LABEL_OVERRIDE=""

usage() {
	cat <<'EOF'
Usage:
  scripts/benchmark_vs_upstream.sh [options]

Modes:
  - Default: compare upstream release vs current fork build/cache
  - Explicit binary mode: pass --baseline-binary and --fork-binary

Profiles:
  quick         15s feedback loop for faster iteration
  standard      default mixed import/search comparison
  write-stress  heavier concurrent read/write validation
  full          longer run with preserved history support

Options:
  --build                  Build the fork binary first via bazel_in_docker.sh
  --upstream VER           Upstream version to compare against (default: 30.1)
  --baseline-binary PATH   Explicit baseline binary path
  --baseline-label LABEL   Label for baseline binary (default: upstream-<ver> or file basename)
  --fork-binary PATH       Explicit fork/candidate binary path
  --fork-label LABEL       Label for fork/candidate binary (default: current git SHA or file basename)
  --duration DUR           Duration per benchmark scenario (default: 30s)
  --port PORT              Base HTTP port for Typesense (default: 12108)
  --work-dir DIR           Working directory for binaries and data
  --clean                  Remove work dir and InfluxDB data before running
  --profile NAME           quick, standard, write-stress, full (default: standard)
  --no-flush               Keep existing InfluxDB data for trend analysis
  --server-args ...        Extra args passed through to typesense-server
  -h, --help               Show this help

Environment:
  OPENAI_API_KEY             Passed through to the benchmark CLI when needed
  TYPESENSE_BAZEL_CACHE_DIR  Reused when staging a built fork binary

Examples:
  scripts/benchmark_vs_upstream.sh --build --profile standard
  scripts/benchmark_vs_upstream.sh --profile write-stress --server-args --max-indexing-concurrency=16
  scripts/benchmark_vs_upstream.sh --baseline-binary ./base/typesense-server --baseline-label abc123 \
    --fork-binary ./head/typesense-server --fork-label def456 --duration 1m --no-flush
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--build)
		BUILD=true
		shift
		;;
	--clean)
		CLEAN=true
		shift
		;;
	--no-flush)
		FLUSH_DB=false
		shift
		;;
	--upstream)
		UPSTREAM_VERSION="$2"
		shift 2
		;;
	--baseline-binary)
		BASELINE_BINARY_OVERRIDE="$2"
		shift 2
		;;
	--baseline-label)
		BASELINE_LABEL_OVERRIDE="$2"
		shift 2
		;;
	--fork-binary)
		FORK_BINARY_OVERRIDE="$2"
		shift 2
		;;
	--fork-label)
		FORK_LABEL_OVERRIDE="$2"
		shift 2
		;;
	--duration)
		DURATION="$2"
		shift 2
		;;
	--port)
		PORT="$2"
		shift 2
		;;
	--work-dir)
		WORK_DIR="$2"
		shift 2
		;;
	--profile)
		PROFILE="$2"
		shift 2
		;;
	--server-args)
		shift
		while [[ $# -gt 0 && "$1" != "--build" && "$1" != "--clean" && "$1" != "--no-flush" && "$1" != "--upstream" && "$1" != "--duration" && "$1" != "--port" && "$1" != "--work-dir" && "$1" != "--profile" && "$1" != "-h" && "$1" != "--help" ]]; do
			SERVER_ARGS+=("$1")
			shift
		done
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "Unknown option: $1" >&2
		exit 1
		;;
	esac
done

case "${PROFILE}" in
quick)
	if [[ "${DURATION}" == "30s" ]]; then
		DURATION="15s"
	fi
	;;
standard)
	:
	;;
write-stress)
	if [[ "${DURATION}" == "30s" ]]; then
		DURATION="60s"
	fi
	;;
	full)
		if [[ "${DURATION}" == "30s" ]]; then
			DURATION="60s"
		fi
		;;
	*)
	echo "Unknown profile: ${PROFILE}. Use: quick, standard, write-stress, full" >&2
	exit 1
	;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BENCHMARK_DIR="${REPO_DIR}/benchmark"
UPSTREAM_URL="https://dl.typesense.org/releases/${UPSTREAM_VERSION}/typesense-server-${UPSTREAM_VERSION}-linux-amd64.tar.gz"

if [[ -n "${BASELINE_BINARY_OVERRIDE}" ]] && [[ ! -x "${BASELINE_BINARY_OVERRIDE}" ]]; then
	echo "Error: baseline binary is not executable: ${BASELINE_BINARY_OVERRIDE}" >&2
	exit 1
fi

if [[ -n "${FORK_BINARY_OVERRIDE}" ]] && [[ ! -x "${FORK_BINARY_OVERRIDE}" ]]; then
	echo "Error: fork binary is not executable: ${FORK_BINARY_OVERRIDE}" >&2
	exit 1
fi

if [[ -n "${FORK_BINARY_OVERRIDE}" ]] && [[ "${BUILD}" == "true" ]]; then
	echo "Error: --build and --fork-binary are mutually exclusive." >&2
	exit 1
fi

if [[ -n "${BASELINE_BINARY_OVERRIDE}" ]] && [[ -z "${BASELINE_LABEL_OVERRIDE}" ]]; then
	BASELINE_LABEL_OVERRIDE="$(basename "${BASELINE_BINARY_OVERRIDE}")"
fi

if [[ -n "${FORK_BINARY_OVERRIDE}" ]] && [[ -z "${FORK_LABEL_OVERRIDE}" ]]; then
	FORK_LABEL_OVERRIDE="$(basename "${FORK_BINARY_OVERRIDE}")"
fi

if [[ "${CLEAN}" == "true" ]]; then
	echo "Cleaning work directory and InfluxDB data..."
	rm -rf "${WORK_DIR}" 2>/dev/null || sudo rm -rf "${WORK_DIR}" 2>/dev/null || true
	rm -rf "${BENCHMARK_DIR}/influxdb-data" 2>/dev/null || sudo rm -rf "${BENCHMARK_DIR}/influxdb-data" 2>/dev/null || true
fi

mkdir -p "${WORK_DIR}"

if [[ "${BUILD}" == "true" ]]; then
	echo "=== Building fork binary ==="
	"${SCRIPT_DIR}/bazel_in_docker.sh" build //:typesense-server
fi

resolve_fork_binary() {
	if [[ -n "${FORK_BINARY_OVERRIDE}" ]]; then
		local fork_dir="${WORK_DIR}/fork-explicit"
		mkdir -p "${fork_dir}"
		chmod u+w "${fork_dir}/typesense-server" 2>/dev/null || true
		cp "${FORK_BINARY_OVERRIDE}" "${fork_dir}/typesense-server"
		chmod +x "${fork_dir}/typesense-server"
		if [[ -d "$(dirname "${FORK_BINARY_OVERRIDE}")/lib" ]]; then
			find "$(dirname "${FORK_BINARY_OVERRIDE}")/lib" -maxdepth 1 -type f -name "*.so*" -exec cp {} "${fork_dir}/" \;
		fi
		FORK_BINARY="${fork_dir}/typesense-server"
		FORK_LABEL="${FORK_LABEL_OVERRIDE}"
		return
	fi

	echo "=== Staging fork binary ==="
	local fork_dir="${WORK_DIR}/fork"
	mkdir -p "${fork_dir}"

	local bazel_cache="${TYPESENSE_BAZEL_CACHE_DIR:-${HOME}/.cache/typesense/bazel-docker}"
	local found=""
	for cache_dir in "${bazel_cache}" /tmp/typesense-bazel-cache-fork; do
		if [[ -d "${cache_dir}" ]]; then
			found=$(find "${cache_dir}" -path "*/bin/typesense-server" -type f -newer "${cache_dir}" 2>/dev/null | head -1 || true)
			if [[ -z "${found}" ]]; then
				found=$(find "${cache_dir}" -name "typesense-server" -not -name "*.pic.*" -not -name "*.params" -type f 2>/dev/null |
					while read -r f; do file "$f" | grep -q "ELF" && echo "$f" && break; done || true)
			fi
			if [[ -n "${found}" ]]; then
				break
			fi
		fi
	done

	if [[ -z "${found}" ]]; then
		echo "Error: Fork binary not found in Bazel cache." >&2
		echo "Build it first: scripts/benchmark_vs_upstream.sh --build" >&2
		exit 1
	fi

	echo "Found fork binary: ${found}"
	chmod u+w "${fork_dir}/typesense-server" 2>/dev/null || true
	cp "${found}" "${fork_dir}/typesense-server"
	chmod +x "${fork_dir}/typesense-server"

	local cache_root
	cache_root="$(dirname "$(dirname "$(dirname "$(dirname "${found}")")")")"
	for lib in libonnxruntime.so.1; do
		local lib_path
		lib_path=$(find "${cache_root}" -name "${lib}" -type f 2>/dev/null | head -1 || true)
		if [[ -n "${lib_path}" ]]; then
			echo "Staging shared lib: ${lib}"
			chmod u+w "${fork_dir}/${lib}" 2>/dev/null || true
			cp "${lib_path}" "${fork_dir}/"
		fi
	done

	FORK_BINARY="${fork_dir}/typesense-server"
	FORK_LABEL="${FORK_LABEL_OVERRIDE:-$(git -C "${REPO_DIR}" rev-parse --short HEAD)}"
}

resolve_baseline_binary() {
	if [[ -n "${BASELINE_BINARY_OVERRIDE}" ]]; then
		local baseline_dir="${WORK_DIR}/baseline-explicit"
		mkdir -p "${baseline_dir}"
		chmod u+w "${baseline_dir}/typesense-server" 2>/dev/null || true
		cp "${BASELINE_BINARY_OVERRIDE}" "${baseline_dir}/typesense-server"
		chmod +x "${baseline_dir}/typesense-server"
		if [[ -d "$(dirname "${BASELINE_BINARY_OVERRIDE}")/lib" ]]; then
			find "$(dirname "${BASELINE_BINARY_OVERRIDE}")/lib" -maxdepth 1 -type f -name "*.so*" -exec cp {} "${baseline_dir}/" \;
		fi
		BASELINE_BINARY="${baseline_dir}/typesense-server"
		BASELINE_LABEL="${BASELINE_LABEL_OVERRIDE}"
		return
	fi

	echo "=== Staging upstream binary ==="
	local upstream_dir="${WORK_DIR}/upstream"
	local upstream_binary="${upstream_dir}/typesense-server"
	if [[ -x "${upstream_binary}" ]]; then
		echo "Using cached upstream Typesense ${UPSTREAM_VERSION}"
	else
		echo "Downloading upstream Typesense ${UPSTREAM_VERSION}..."
		mkdir -p "${upstream_dir}"
		curl -fSL "${UPSTREAM_URL}" | tar -xz -C "${upstream_dir}"
		if [[ ! -x "${upstream_binary}" ]]; then
			echo "Error: Upstream binary not found after extraction." >&2
			exit 1
		fi
	fi

	BASELINE_BINARY="${upstream_binary}"
	BASELINE_LABEL="${BASELINE_LABEL_OVERRIDE:-upstream-${UPSTREAM_VERSION}}"
}

verify_binary() {
	local binary_path="$1"
	local binary_dir
	binary_dir="$(dirname "${binary_path}")"
	if ! docker run --rm -e "LD_LIBRARY_PATH=${binary_dir}" \
		-v "${binary_dir}:${binary_dir}:ro" ubuntu:24.04 \
		"${binary_path}" --help >/dev/null 2>&1; then
		echo "Warning: binary may have missing shared libraries: ${binary_path}" >&2
		docker run --rm -v "${binary_dir}:${binary_dir}:ro" ubuntu:24.04 \
			ldd "${binary_path}" 2>&1 | grep "not found" || true
	fi
}

resolve_fork_binary
resolve_baseline_binary
verify_binary "${FORK_BINARY}"
verify_binary "${BASELINE_BINARY}"

echo "=== Starting benchmark infrastructure ==="
cd "${BENCHMARK_DIR}"
docker compose up -d influxdb grafana k6
echo "Waiting for InfluxDB to be healthy..."
timeout 30 bash -c 'until curl -sf http://localhost:8086/ping; do sleep 1; done' || {
	echo "Error: InfluxDB did not become healthy" >&2
	exit 1
}

# Flush stale data from previous benchmark runs (unless --no-flush)
if [[ "${FLUSH_DB}" == "true" ]]; then
	echo "Flushing InfluxDB k6 database..."
	curl -sf -X POST 'http://localhost:8086/query' --data-urlencode "q=DROP DATABASE k6" >/dev/null 2>&1 || true
	curl -sf -X POST 'http://localhost:8086/query' --data-urlencode "q=CREATE DATABASE k6" >/dev/null 2>&1
else
	echo "Keeping existing InfluxDB data (--no-flush)"
	# Ensure database exists
	curl -sf -X POST 'http://localhost:8086/query' --data-urlencode "q=CREATE DATABASE k6" >/dev/null 2>&1 || true
fi

echo "=== Building benchmark CLI ==="
cd "${BENCHMARK_DIR}"
bun install --frozen-lockfile
bun run build

echo ""
echo "========================================="
echo "  Profile: ${PROFILE}"
echo "  ${BASELINE_LABEL} vs ${FORK_LABEL}"
echo "  Duration: ${DURATION} per scenario"
echo "  Port: ${PORT}"
if [[ ${#SERVER_ARGS[@]} -gt 0 ]]; then
	echo "  Server args: ${SERVER_ARGS[*]}"
fi
echo "========================================="
echo ""

SERVER_ARGS_CMD=()
if [[ ${#SERVER_ARGS[@]} -gt 0 ]]; then
	for arg in "${SERVER_ARGS[@]}"; do
		SERVER_ARGS_CMD+=(--server-args "$arg")
	done
fi

node dist/index.js \
	benchmark \
	--binaries "${BASELINE_BINARY}" "${FORK_BINARY}" \
	-c "${BASELINE_LABEL}" "${FORK_LABEL}" \
	-d "${WORK_DIR}/data" \
	--duration "${DURATION}" \
	--port "${PORT}" \
	"${SERVER_ARGS_CMD[@]}" \
	-y -v

echo ""
echo "========================================="
echo "  Results"
echo "========================================="
echo "Grafana:     http://localhost:3000"
echo "InfluxDB:    http://localhost:8086"
echo "Metrics dir: ${WORK_DIR}/data/metrics/"
echo ""
echo "Cleanup: cd benchmark && docker compose down"

# --- Step 6: Dump RocksDB metrics summary ---
METRICS_DIR="${WORK_DIR}/data/metrics"
if [[ ! -d "${METRICS_DIR}" && -d "${WORK_DIR}/metrics" ]]; then
	METRICS_DIR="${WORK_DIR}/metrics"
fi
if [[ -d "${METRICS_DIR}" ]]; then
	echo ""
	echo "=== RocksDB Metrics Summary ==="
	for f in "${METRICS_DIR}"/*.json; do
		if [[ -f "$f" ]]; then
			echo "--- $(basename "$f") ---"
			# Extract key metrics using python/jq if available
			if command -v jq &>/dev/null; then
				jq -r '.rocksdb_properties // {} | to_entries[] | "  \(.key): \(.value)"' "$f" 2>/dev/null || true
			else
				echo "  (install jq for formatted output)"
			fi
		fi
	done
fi

# --- Step 7: Archive metrics snapshot ---
ARCHIVE_ROOT="${WORK_DIR}/archives"
ARCHIVE_ID="$(date -u +"%Y%m%d-%H%M%S")-${PROFILE}"

if [[ ${#SERVER_ARGS[@]} -gt 0 ]]; then
	SAFE_ARGS="$(printf "%s" "${SERVER_ARGS[*]}" | tr ' /' '__' | tr -cd '[:alnum:]_.-')"
	ARCHIVE_ID="${ARCHIVE_ID}-${SAFE_ARGS:0:80}"
fi

ARCHIVE_DIR="${ARCHIVE_ROOT}/${ARCHIVE_ID}"
mkdir -p "${ARCHIVE_DIR}"

if [[ -d "${METRICS_DIR}" ]]; then
	cp "${METRICS_DIR}"/*.json "${ARCHIVE_DIR}/" 2>/dev/null || true
fi

printf "baseline_label=%s\nfork_label=%s\nprofile=%s\nduration=%s\nport=%s\nserver_args=%s\nmetrics_dir=%s\n" \
	"${BASELINE_LABEL}" "${FORK_LABEL}" "${PROFILE}" "${DURATION}" "${PORT}" \
	"${SERVER_ARGS[*]:-}" "${METRICS_DIR}" >"${ARCHIVE_DIR}/run-info.txt"

echo "Archived benchmark snapshot: ${ARCHIVE_DIR}"
