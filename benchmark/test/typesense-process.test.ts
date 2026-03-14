import { describe, expect, test } from "vitest";

import { TypesenseProcessLogReducer } from "../src/services/typesense-process";

describe("TypesenseProcessLogReducer", () => {
  test("summarizes repeated threadpool exhaustion warnings without hiding the first line", () => {
    const messages: string[] = [];
    let nowMs = 0;
    const reducer = new TypesenseProcessLogReducer(
      12108,
      { info: (message: unknown) => messages.push(String(message)) },
      () => nowMs,
    );

    const warning = (taskQueueLen: number) =>
      `W0000 00:00:1773428193.015486       7 threadpool.h:103] Threadpool exhaustion detected, ` +
      `task_queue_len: ${taskQueueLen}, thread_pool_len: 4`;

    reducer.handleStderrChunk(`${warning(10)}\n${warning(12)}\n`);
    nowMs = 1_000;
    reducer.handleStderrChunk(`${warning(15)}\n`);
    nowMs = 2_000;
    reducer.handleStderrChunk("actual error line\n");

    expect(messages).toEqual([
      `[Node on port 12108] stderr: ${warning(10)}`,
      "[Node on port 12108] stderr: suppressed 2 repeated threadpool exhaustion warnings over 1.0s " +
        "(max_queue_len=15, thread_pool_len=4)",
      "[Node on port 12108] stderr: actual error line",
    ]);
  });

  test("flushes buffered partial lines and pending suppression summaries", () => {
    const messages: string[] = [];
    let nowMs = 0;
    const reducer = new TypesenseProcessLogReducer(
      12108,
      { info: (message: unknown) => messages.push(String(message)) },
      () => nowMs,
    );

    const warning =
      "W0000 00:00:1773428193.015486       7 threadpool.h:103] Threadpool exhaustion detected, " +
      "task_queue_len: 9, thread_pool_len: 4";

    reducer.handleStdoutChunk("hello");
    reducer.handleStdoutChunk(" world\n");
    reducer.handleStderrChunk(`${warning}\n${warning}`);
    nowMs = 3_000;
    reducer.flush();

    expect(messages).toEqual([
      "[Node on port 12108] stdout: hello world",
      `[Node on port 12108] stderr: ${warning}`,
      "[Node on port 12108] stderr: suppressed 1 repeated threadpool exhaustion warnings over 3.0s " +
        "(max_queue_len=9, thread_pool_len=4)",
    ]);
  });
});
