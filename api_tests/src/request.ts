const DELAY_INTERVALS = [10, 100, 1000, 2000, 3000, 4000];
const DEFAULT_SINGLE_API_PORT = 8108;
const DEFAULT_MULTI_API_PORTS = [5108, 6108, 7108];

function parsePort(value: string | undefined, fallback: number): number {
  const parsed = Number.parseInt(value ?? "", 10);
  if (Number.isNaN(parsed) || parsed < 1 || parsed > 65535) {
    return fallback;
  }
  return parsed;
}

function parseMultiNodePorts(): number[] {
  const configured = process.env.TYPESENSE_MULTI_API_PORTS;
  if (!configured) {
    return DEFAULT_MULTI_API_PORTS;
  }

  const parsed = configured
    .split(",")
    .map((entry) => Number.parseInt(entry.trim(), 10))
    .filter((port) => !Number.isNaN(port) && port >= 1 && port <= 65535);

  if (parsed.length !== 3) {
    return DEFAULT_MULTI_API_PORTS;
  }

  return parsed;
}

function getApiHost(): string {
  return process.env.TYPESENSE_API_HOST ?? "localhost";
}

function getApiKey(): string {
  return process.env.TYPESENSE_API_KEY ?? "xyz";
}

function getSingleNodePort(): number {
  return parsePort(process.env.TYPESENSE_SINGLE_API_PORT, DEFAULT_SINGLE_API_PORT);
}

function getMultiNodePort(node: number): number {
  if (node < 1 || node > 3) {
    throw new Error(`Multi-node index must be 1..3. Received: ${node}`);
  }

  return parseMultiNodePorts()[node - 1];
}

function buildUrl(port: number, path: string): string {
  return `http://${getApiHost()}:${port}${path}`;
}

export async function fetchSingleNode(url: string, options?: RequestInit, port: number = getSingleNodePort()) {
  return fetch(buildUrl(port, url), {
    ...options,
    headers: {
      ...options?.headers,
      "X-TYPESENSE-API-KEY": getApiKey(),
    },
    signal: AbortSignal.timeout(30000),
  });
}

export async function fetchMultiNodeRequest(node: number, url: string, options?: RequestInit): Promise<Response> {
  return fetch(buildUrl(getMultiNodePort(node), url), {
    ...options,
    headers: {
      ...options?.headers,
      "X-TYPESENSE-API-KEY": getApiKey(),
    },
    signal: AbortSignal.timeout(30000),
  });
}

export async function fetchMultiNode(node: number, url: string, options?: RequestInit): Promise<Response> {
  for (let i = 0; i < DELAY_INTERVALS.length; i++) {
    const isSync = await checkCommitedIndex();
    if (isSync) {
      break;
    }
    await new Promise((resolve) => setTimeout(resolve, DELAY_INTERVALS[i]));
  }

  return fetchMultiNodeRequest(node, url, options);
}

export async function checkCommitedIndex() {
  try {
    const res = await Promise.all([
      fetchMultiNodeRequest(1, "/status"),
      fetchMultiNodeRequest(2, "/status"),
      fetchMultiNodeRequest(3, "/status"),
    ]);

    if (res.some((response) => !response.ok)) {
      return false;
    }

    const data = await Promise.all(res.map((response) => response.json()));
    const committedIndexes = data.map((item: any) => item.committed_index);
    return committedIndexes[0] === committedIndexes[1] && committedIndexes[0] === committedIndexes[2];
  } catch {
    return false;
  }
}

export async function waitForSingleAnalyticsFlush() {
  await fetchSingleNode("/analytics/flush", { method: "POST" });

  for (let i = 0; i < DELAY_INTERVALS.length; i++) {
    const res = await fetchSingleNode("/analytics/status");
    const data: any = await res.json();
    let isSync = true;

    for (const key in data) {
      if (data[key] !== 0) {
        isSync = false;
        break;
      }
    }

    if (isSync) {
      break;
    }

    await new Promise((resolve) => setTimeout(resolve, DELAY_INTERVALS[i]));
  }
}

export async function waitForMultiAnalyticsFlush() {
  await Promise.all([
    fetchMultiNodeRequest(1, "/analytics/flush", { method: "POST" }),
    fetchMultiNodeRequest(2, "/analytics/flush", { method: "POST" }),
    fetchMultiNodeRequest(3, "/analytics/flush", { method: "POST" }),
  ]);

  for (let i = 0; i < DELAY_INTERVALS.length; i++) {
    const res = await Promise.all([
      fetchMultiNodeRequest(1, "/analytics/status"),
      fetchMultiNodeRequest(2, "/analytics/status"),
      fetchMultiNodeRequest(3, "/analytics/status"),
    ]);

    const data: any[] = await Promise.all(res.map((response) => response.json()));
    let isSync = true;

    for (const item of data) {
      for (const key in item) {
        if (item[key] !== 0) {
          isSync = false;
          break;
        }
      }
    }

    if (isSync) {
      break;
    }

    await new Promise((resolve) => setTimeout(resolve, DELAY_INTERVALS[i]));
  }
}
