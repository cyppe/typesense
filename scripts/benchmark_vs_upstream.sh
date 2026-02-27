#!/usr/bin/env bash
# Compare fork build against upstream Typesense release.
# Everything runs inside Docker — no host dependencies beyond Docker itself.
#
# Usage:
#   scripts/benchmark_vs_upstream.sh [options]
#
# Options:
#   --build          Build the fork binary first (via bazel_in_docker.sh)
#   --upstream VER   Upstream version to compare against (default: 30.1)
#   --duration DUR   Duration per benchmark scenario (default: 30s)
#   --port PORT      Base HTTP port for Typesense (default: 12108)
#   --work-dir DIR   Working directory for binaries and data (default: /tmp/typesense-benchmark)
#   --clean          Remove work dir and InfluxDB data before running
#   --profile NAME   Benchmark profile: quick, standard, write-stress, full (default: standard)
#   --no-flush       Don't flush InfluxDB data (keep historical data for trend analysis)
#   --server-args    Extra args to pass to typesense-server (e.g. --server-args --max-indexing-concurrency=16)
#
# Profiles:
#   quick        - 15s per scenario, search only (fastest feedback loop)
#   standard     - 30s per scenario, import + search (default)
#   write-stress - 60s import, parallel writes with 4 VUs, 3 iterations each
#   full         - 60s per scenario, import + search + stress import + metrics collection
#
# Examples:
#   scripts/benchmark_vs_upstream.sh --build                     # build + benchmark
#   scripts/benchmark_vs_upstream.sh                             # benchmark only (binary must exist)
#   scripts/benchmark_vs_upstream.sh --upstream 30.1 --duration 60s
#   scripts/benchmark_vs_upstream.sh --clean --build             # fresh start
#   scripts/benchmark_vs_upstream.sh --profile write-stress      # heavy write testing
#   scripts/benchmark_vs_upstream.sh --profile full --no-flush   # everything, keep history
set -euo pipefail

# --- Defaults ---
BUILD=false
CLEAN=false
FLUSH_DB=true
UPSTREAM_VERSION="30.1"
DURATION="30s"
PORT="12108"
WORK_DIR="${HOME}/.cache/typesense/benchmark"
PROFILE="standard"
SERVER_ARGS=()

# --- Parse args ---
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
		head -36 "$0" | tail -34
		exit 0
		;;
	*)
		echo "Unknown option: $1" >&2
		exit 1
		;;
	esac
done

# --- Apply profile defaults ---
case "${PROFILE}" in
quick)
	DURATION="${DURATION:-15s}"
	;;
standard)
	# defaults are fine
	;;
write-stress)
	DURATION="${DURATION:-60s}"
	;;
full)
	DURATION="${DURATION:-60s}"
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

# --- Clean if requested ---
if [[ "${CLEAN}" == "true" ]]; then
	echo "Cleaning work directory and InfluxDB data..."
	rm -rf "${WORK_DIR}" 2>/dev/null || sudo rm -rf "${WORK_DIR}" 2>/dev/null || true
	rm -rf "${BENCHMARK_DIR}/influxdb-data" 2>/dev/null || sudo rm -rf "${BENCHMARK_DIR}/influxdb-data" 2>/dev/null || true
fi

mkdir -p "${WORK_DIR}"

# --- Step 1: Build fork binary (optional) ---
if [[ "${BUILD}" == "true" ]]; then
	echo "=== Building fork binary ==="
	"${SCRIPT_DIR}/bazel_in_docker.sh" build //:typesense-server
fi

# --- Step 2: Locate and stage fork binary + shared libs ---
echo "=== Staging fork binary ==="
FORK_DIR="${WORK_DIR}/fork"
mkdir -p "${FORK_DIR}"

# Find the built binary in the Bazel cache
# The bazel-bin symlink points into the Docker build cache
BAZEL_CACHE="${TYPESENSE_BAZEL_CACHE_DIR:-${HOME}/.cache/typesense/bazel-docker}"
FORK_BINARY=""

# Try the bazel_in_docker.sh cache first, then the legacy cache
for cache_dir in "${BAZEL_CACHE}" /tmp/typesense-bazel-cache-fork; do
	if [[ -d "${cache_dir}" ]]; then
		found=$(find "${cache_dir}" -path "*/bin/typesense-server" -type f -newer "${cache_dir}" 2>/dev/null | head -1 || true)
		if [[ -z "${found}" ]]; then
			# Fallback: find any typesense-server binary
			found=$(find "${cache_dir}" -name "typesense-server" -not -name "*.pic.*" -not -name "*.params" -type f 2>/dev/null |
				while read -r f; do file "$f" | grep -q "ELF" && echo "$f" && break; done || true)
		fi
		if [[ -n "${found}" ]]; then
			FORK_BINARY="${found}"
			break
		fi
	fi
done

if [[ -z "${FORK_BINARY}" ]]; then
	echo "Error: Fork binary not found in Bazel cache." >&2
	echo "Build it first: scripts/benchmark_vs_upstream.sh --build" >&2
	exit 1
fi

echo "Found fork binary: ${FORK_BINARY}"
# Ensure previous binary is writable before overwriting (Bazel cache files are read-only)
chmod u+w "${FORK_DIR}/typesense-server" 2>/dev/null || true
cp "${FORK_BINARY}" "${FORK_DIR}/typesense-server"
chmod +x "${FORK_DIR}/typesense-server"

# Copy shared libraries that the binary needs (e.g. libonnxruntime)
CACHE_ROOT="$(dirname "$(dirname "$(dirname "$(dirname "${FORK_BINARY}")")")")"
for lib in libonnxruntime.so.1; do
	lib_path=$(find "${CACHE_ROOT}" -name "${lib}" -type f 2>/dev/null | head -1 || true)
	if [[ -n "${lib_path}" ]]; then
		echo "Staging shared lib: ${lib}"
		chmod u+w "${FORK_DIR}/${lib}" 2>/dev/null || true
		cp "${lib_path}" "${FORK_DIR}/"
	fi
done

# Verify the binary runs
if ! docker run --rm -e "LD_LIBRARY_PATH=${FORK_DIR}" \
	-v "${FORK_DIR}:${FORK_DIR}:ro" ubuntu:24.04 \
	"${FORK_DIR}/typesense-server" --help >/dev/null 2>&1; then
	echo "Warning: Fork binary may have missing shared libraries." >&2
	echo "Checking..." >&2
	docker run --rm -v "${FORK_DIR}:${FORK_DIR}:ro" ubuntu:24.04 \
		ldd "${FORK_DIR}/typesense-server" 2>&1 | grep "not found" || true
fi

# --- Step 3: Download upstream binary (cached) ---
echo "=== Staging upstream binary ==="
UPSTREAM_DIR="${WORK_DIR}/upstream"
UPSTREAM_BINARY="${UPSTREAM_DIR}/typesense-server"
if [[ -x "${UPSTREAM_BINARY}" ]]; then
	echo "Using cached upstream Typesense ${UPSTREAM_VERSION}"
else
	echo "Downloading upstream Typesense ${UPSTREAM_VERSION}..."
	mkdir -p "${UPSTREAM_DIR}"
	curl -fSL "${UPSTREAM_URL}" | tar -xz -C "${UPSTREAM_DIR}"
	if [[ ! -x "${UPSTREAM_BINARY}" ]]; then
		echo "Error: Upstream binary not found after extraction." >&2
		exit 1
	fi
fi

# --- Step 4: Start benchmark infrastructure ---
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

# --- Step 5: Build and run the benchmark CLI ---
echo "=== Building benchmark CLI ==="
cd "${BENCHMARK_DIR}"
pnpm install --frozen-lockfile
pnpm build

FORK_SHA="$(git -C "${REPO_DIR}" rev-parse --short HEAD)"

echo ""
echo "========================================="
echo "  Profile: ${PROFILE}"
echo "  upstream-${UPSTREAM_VERSION} vs fork-${FORK_SHA}"
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
	--binaries "${UPSTREAM_BINARY}" "${FORK_DIR}/typesense-server" \
	-c "upstream-${UPSTREAM_VERSION}" "${FORK_SHA}" \
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

printf "upstream=%s\nfork_sha=%s\nprofile=%s\nduration=%s\nport=%s\nserver_args=%s\nmetrics_dir=%s\n" \
	"${UPSTREAM_VERSION}" "${FORK_SHA}" "${PROFILE}" "${DURATION}" "${PORT}" \
	"${SERVER_ARGS[*]:-}" "${METRICS_DIR}" >"${ARCHIVE_DIR}/run-info.txt"

echo "Archived benchmark snapshot: ${ARCHIVE_DIR}"
